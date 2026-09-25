#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602  /* Windows 8: PowerRegisterSuspendResumeNotification */
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <powrprof.h>
#include <powerbase.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#pragma comment(lib, "PowrProf.lib")
#endif

/* =========================
 * Settings
 * ========================= */

#define DEFAULT_CPU_FREQ_AC_MHZ  800u
#define DEFAULT_CPU_FREQ_DC_MHZ  800u
#define CPU_FREQ_UNLIMITED_MHZ    0u
#define CPU_FREQ_DEBOUNCE_MS      5000ULL

/* Apply settings to the currently active power scheme.
 * Overlay schemes (OVERLAY_SCHEME_*) are deprecated on current Windows versions.
 */

/* =========================
 * GUIDs
 * ========================= */

/* SUB_PROCESSOR / GUID_PROCESSOR_SETTINGS_SUBGROUP
 * 54533251-82be-4824-96c1-47b60b740d00
 */
static const GUID g_guidProcessorSettingsSubgroup = {
    0x54533251, 0x82be, 0x4824,
    {0x96, 0xc1, 0x47, 0xb6, 0x0b, 0x74, 0x0d, 0x00}
};

/* PROCFREQMAX, PROCFREQMAX1, PROCFREQMAX2 */
static const GUID g_rgProcessorFrequencySettings[] = {
    {
        0x75b0ae3f, 0xbce0, 0x45a7,
        {0x8c, 0x89, 0xc9, 0x61, 0x1c, 0x25, 0xe1, 0x00}
    },
    {
        0x75b0ae3f, 0xbce0, 0x45a7,
        {0x8c, 0x89, 0xc9, 0x61, 0x1c, 0x25, 0xe1, 0x01}
    },
    {
        0x75b0ae3f, 0xbce0, 0x45a7,
        {0x8c, 0x89, 0xc9, 0x61, 0x1c, 0x25, 0xe1, 0x02}
    }
};

/* =========================
 * Global state
 * ========================= */

static volatile LONG g_lExiting = FALSE;
static HPOWERNOTIFY g_hPowerNotify = NULL;
static HANDLE g_hExitEvent = NULL;
static DWORD g_dwAcMHz = DEFAULT_CPU_FREQ_AC_MHZ;
static DWORD g_dwDcMHz = DEFAULT_CPU_FREQ_DC_MHZ;

/* Protect the debounce timestamp in case Windows invokes callbacks concurrently. */
static SRWLOCK g_srwDebounceLock = SRWLOCK_INIT;
static ULONGLONG g_ullLastApplyTick = 0;

/* =========================
 * Helpers
 * ========================= */

static void PrintWin32Error(const char *pszApiName, DWORD dwError)
{
    LPSTR pszMessage = NULL;
    DWORD cchMessage;

    cchMessage = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        dwError,
        0,
        (LPSTR)&pszMessage,
        0,
        NULL
    );

    if (cchMessage != 0 && pszMessage != NULL) {
        while (cchMessage > 0 &&
               (pszMessage[cchMessage - 1] == '\r' ||
                pszMessage[cchMessage - 1] == '\n')) {
            pszMessage[--cchMessage] = '\0';
        }

        fprintf(
            stderr,
            "%s failed: %s (error %lu)\n",
            pszApiName,
            pszMessage,
            (unsigned long)dwError
        );

        LocalFree(pszMessage);
    } else {
        fprintf(
            stderr,
            "%s failed (error %lu)\n",
            pszApiName,
            (unsigned long)dwError
        );
    }
}

/* =========================
 * Power settings API
 * ========================= */

static void ApplyCpuFrequency(DWORD dwAcMHz, DWORD dwDcMHz)
{
    GUID *pSchemeGuid = NULL;
    DWORD dwResult;
    SIZE_T i;

    /* Equivalent to SCHEME_CURRENT: ask Windows for the currently active scheme. */
    dwResult = PowerGetActiveScheme(NULL, &pSchemeGuid);

    /* Preserve the Python version's behavior: if the active scheme cannot be
     * obtained, simply leave the settings unchanged.
     */
    if (dwResult != ERROR_SUCCESS) {
        return;
    }

    for (i = 0; i < ARRAYSIZE(g_rgProcessorFrequencySettings); ++i) {
        /* Equivalent to:
         * powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR <setting> <ac_mhz>
         *
         * Return values are intentionally ignored to match the Python version.
         */
        (void)PowerWriteACValueIndex(
            NULL,
            pSchemeGuid,
            &g_guidProcessorSettingsSubgroup,
            &g_rgProcessorFrequencySettings[i],
            dwAcMHz
        );

        /* Equivalent to:
         * powercfg /setdcvalueindex SCHEME_CURRENT SUB_PROCESSOR <setting> <dc_mhz>
         */
        (void)PowerWriteDCValueIndex(
            NULL,
            pSchemeGuid,
            &g_guidProcessorSettingsSubgroup,
            &g_rgProcessorFrequencySettings[i],
            dwDcMHz
        );
    }

    /* Equivalent to: powercfg /setactive SCHEME_CURRENT
     * Changes to the active scheme take effect after it is re-activated.
     */
    (void)PowerSetActiveScheme(NULL, pSchemeGuid);

    /* PowerGetActiveScheme allocates the GUID; Windows requires LocalFree. */
    LocalFree(pSchemeGuid);
}

static void ApplyCpuFrequencyLimit(void)
{
    ApplyCpuFrequency(g_dwAcMHz, g_dwDcMHz);
}

static void ClearCpuFrequencyLimit(void)
{
    /* 0 means unlimited/default for the processor maximum frequency setting. */
    ApplyCpuFrequency(CPU_FREQ_UNLIMITED_MHZ, CPU_FREQ_UNLIMITED_MHZ);
}

/* =========================
 * Resume handling
 * ========================= */

static void ApplyCpuFrequencyLimitDebounced(void)
{
    ULONGLONG ullNow;
    BOOL fShouldApply = FALSE;

    if (InterlockedCompareExchange(&g_lExiting, FALSE, FALSE) != FALSE) {
        return;
    }

    ullNow = GetTickCount64();

    /* PBT_APMRESUMEAUTOMATIC and PBT_APMRESUMESUSPEND may arrive in quick
     * succession, so suppress duplicate executions within five seconds.
     */
    AcquireSRWLockExclusive(&g_srwDebounceLock);

    if (ullNow - g_ullLastApplyTick >= CPU_FREQ_DEBOUNCE_MS) {
        g_ullLastApplyTick = ullNow;
        fShouldApply = TRUE;
    }

    ReleaseSRWLockExclusive(&g_srwDebounceLock);

    if (fShouldApply) {
        ApplyCpuFrequencyLimit();
    }
}

static ULONG CALLBACK PowerNotificationCallback(
    PVOID pvContext,
    ULONG ulEventType,
    PVOID pvSetting)
{
    UNREFERENCED_PARAMETER(pvContext);
    UNREFERENCED_PARAMETER(pvSetting);

    switch (ulEventType) {
    case PBT_APMRESUMESUSPEND:
        /* Resume from sleep/hibernation triggered by user activity. */
        ApplyCpuFrequencyLimitDebounced();
        break;

    case PBT_APMRESUMEAUTOMATIC:
        /* Automatic resume. Some environments emit only this event after hibernation. */
        ApplyCpuFrequencyLimitDebounced();
        break;

    case PBT_APMSUSPEND:
        /* Just before entering sleep/hibernation. No action is needed here. */
        break;

    default:
        break;
    }

    return ERROR_SUCCESS;
}

static DWORD RegisterPowerNotification(void)
{
    DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS subscribeParams;

    ZeroMemory(&subscribeParams, sizeof(subscribeParams));
    subscribeParams.Callback = PowerNotificationCallback;
    subscribeParams.Context = NULL;

    return PowerRegisterSuspendResumeNotification(
        DEVICE_NOTIFY_CALLBACK,
        (HANDLE)&subscribeParams,
        &g_hPowerNotify
    );
}

static void UnregisterPowerNotification(void)
{
    HPOWERNOTIFY hPowerNotify = g_hPowerNotify;

    if (hPowerNotify != NULL) {
        /* Make cleanup idempotent because main() and atexit() may both call it. */
        g_hPowerNotify = NULL;
        (void)PowerUnregisterSuspendResumeNotification(hPowerNotify);
    }
}

/* =========================
 * Console shutdown handling
 * ========================= */

static BOOL WINAPI ConsoleCtrlHandler(DWORD dwCtrlType)
{
    switch (dwCtrlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        InterlockedExchange(&g_lExiting, TRUE);

        if (g_hExitEvent != NULL) {
            SetEvent(g_hExitEvent);
        }

        return TRUE;

    default:
        return FALSE;
    }
}

/* =========================
 * Command-line parsing
 * ========================= */

typedef enum _RUN_MODE {
    RUN_MODE_RESIDENT = 0,
    RUN_MODE_ONCE,
    RUN_MODE_UNTHROTTLE_ONCE
} RUN_MODE;

typedef struct _PROGRAM_OPTIONS {
    RUN_MODE RunMode;
    DWORD dwAcMHz;
    DWORD dwDcMHz;
} PROGRAM_OPTIONS;

static void PrintUsage(FILE *pStream, const char *pszProgramName)
{
    fprintf(
        pStream,
        "Usage: %s [options]\n"
        "\n"
        "Applies the CPU maximum frequency limit. By default, this program\n"
        "stays resident and reapplies the limit after resume from sleep/hibernation.\n"
        "\n"
        "Frequency options:\n"
        "  --mhz <MHz>         Set both AC and DC maximum frequency.\n"
        "  --ac-mhz <MHz>      Set the AC maximum frequency.\n"
        "  --dc-mhz <MHz>      Set the DC maximum frequency.\n"
        "\n"
        "  If --mhz is combined with --ac-mhz or --dc-mhz, the individual\n"
        "  AC/DC option takes precedence regardless of argument order.\n"
        "  Defaults: AC=%lu MHz, DC=%lu MHz. A value of 0 means unlimited.\n"
        "\n"
        "Mode options:\n"
        "  --once              Apply the CPU frequency limit once, then exit.\n"
        "  --unthrottle-once   Clear the CPU frequency limit once, then exit.\n"
        "  --release-once      Alias for --unthrottle-once.\n"
        "  -h, --help          Show this help.\n",
        pszProgramName,
        (unsigned long)DEFAULT_CPU_FREQ_AC_MHZ,
        (unsigned long)DEFAULT_CPU_FREQ_DC_MHZ
    );
}

static BOOL ParseDwordValue(const char *pszValue, DWORD *pdwValue)
{
    char *pszEnd = NULL;
    unsigned long ulValue;

    if (pszValue == NULL || *pszValue == '\0' || *pszValue == '-') {
        return FALSE;
    }

    errno = 0;
    ulValue = strtoul(pszValue, &pszEnd, 10);

    if (errno == ERANGE ||
        pszEnd == pszValue ||
        *pszEnd != '\0' ||
        ulValue > MAXDWORD) {
        return FALSE;
    }

    *pdwValue = (DWORD)ulValue;
    return TRUE;
}

static int ParseFrequencyOption(
    int argc,
    char **argv,
    int *pi,
    const char *pszOptionName,
    DWORD *pdwValue)
{
    if (*pi + 1 >= argc) {
        fprintf(stderr, "error: %s requires a MHz value\n", pszOptionName);
        return -1;
    }

    ++(*pi);

    if (!ParseDwordValue(argv[*pi], pdwValue)) {
        fprintf(
            stderr,
            "error: invalid MHz value for %s: %s\n",
            pszOptionName,
            argv[*pi]
        );
        return -1;
    }

    return 0;
}

/* Return values:
 *   0  parsed successfully
 *   1  help was printed
 *  -1  invalid command line
 */
static int ParseArguments(int argc, char **argv, PROGRAM_OPTIONS *pOptions)
{
    int i;
    BOOL fCommonMHzSpecified = FALSE;
    BOOL fAcMHzSpecified = FALSE;
    BOOL fDcMHzSpecified = FALSE;
    DWORD dwCommonMHz = 0;
    DWORD dwAcMHz = DEFAULT_CPU_FREQ_AC_MHZ;
    DWORD dwDcMHz = DEFAULT_CPU_FREQ_DC_MHZ;

    pOptions->RunMode = RUN_MODE_RESIDENT;
    pOptions->dwAcMHz = DEFAULT_CPU_FREQ_AC_MHZ;
    pOptions->dwDcMHz = DEFAULT_CPU_FREQ_DC_MHZ;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            PrintUsage(stdout, argv[0]);
            return 1;
        }

        if (strcmp(argv[i], "--once") == 0) {
            if (pOptions->RunMode != RUN_MODE_RESIDENT) {
                fprintf(stderr, "error: mode options are mutually exclusive\n");
                PrintUsage(stderr, argv[0]);
                return -1;
            }

            pOptions->RunMode = RUN_MODE_ONCE;
            continue;
        }

        if (strcmp(argv[i], "--unthrottle-once") == 0 ||
            strcmp(argv[i], "--release-once") == 0) {
            if (pOptions->RunMode != RUN_MODE_RESIDENT) {
                fprintf(stderr, "error: mode options are mutually exclusive\n");
                PrintUsage(stderr, argv[0]);
                return -1;
            }

            pOptions->RunMode = RUN_MODE_UNTHROTTLE_ONCE;
            continue;
        }

        if (strcmp(argv[i], "--mhz") == 0) {
            if (ParseFrequencyOption(argc, argv, &i, "--mhz", &dwCommonMHz) != 0) {
                PrintUsage(stderr, argv[0]);
                return -1;
            }

            fCommonMHzSpecified = TRUE;
            continue;
        }

        if (strcmp(argv[i], "--ac-mhz") == 0) {
            if (ParseFrequencyOption(argc, argv, &i, "--ac-mhz", &dwAcMHz) != 0) {
                PrintUsage(stderr, argv[0]);
                return -1;
            }

            fAcMHzSpecified = TRUE;
            continue;
        }

        if (strcmp(argv[i], "--dc-mhz") == 0) {
            if (ParseFrequencyOption(argc, argv, &i, "--dc-mhz", &dwDcMHz) != 0) {
                PrintUsage(stderr, argv[0]);
                return -1;
            }

            fDcMHzSpecified = TRUE;
            continue;
        }

        fprintf(stderr, "error: unrecognized argument: %s\n", argv[i]);
        PrintUsage(stderr, argv[0]);
        return -1;
    }

    if (pOptions->RunMode == RUN_MODE_UNTHROTTLE_ONCE &&
        (fCommonMHzSpecified || fAcMHzSpecified || fDcMHzSpecified)) {
        fprintf(
            stderr,
            "error: frequency options cannot be used with --unthrottle-once/--release-once\n"
        );
        PrintUsage(stderr, argv[0]);
        return -1;
    }

    if (fCommonMHzSpecified) {
        pOptions->dwAcMHz = dwCommonMHz;
        pOptions->dwDcMHz = dwCommonMHz;
    }

    if (fAcMHzSpecified) {
        pOptions->dwAcMHz = dwAcMHz;
    }

    if (fDcMHzSpecified) {
        pOptions->dwDcMHz = dwDcMHz;
    }

    return 0;
}

/* =========================
 * main
 * ========================= */

int main(int argc, char **argv)
{
    PROGRAM_OPTIONS options;
    int nParseResult;
    DWORD dwResult;
    DWORD dwWaitResult;

    nParseResult = ParseArguments(argc, argv, &options);
    if (nParseResult > 0) {
        return 0;
    }

    if (nParseResult < 0) {
        return 2;
    }

    g_dwAcMHz = options.dwAcMHz;
    g_dwDcMHz = options.dwDcMHz;

    /* One-shot execution. Do not register notifications or stay resident. */
    if (options.RunMode == RUN_MODE_ONCE) {
        ApplyCpuFrequencyLimit();
        return 0;
    }

    if (options.RunMode == RUN_MODE_UNTHROTTLE_ONCE) {
        ClearCpuFrequencyLimit();
        return 0;
    }

    if (atexit(UnregisterPowerNotification) != 0) {
        fprintf(stderr, "warning: atexit registration failed\n");
    }

    g_hExitEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (g_hExitEvent == NULL) {
        PrintWin32Error("CreateEventW", GetLastError());
        return 1;
    }

    if (!SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE)) {
        PrintWin32Error("SetConsoleCtrlHandler", GetLastError());
        CloseHandle(g_hExitEvent);
        g_hExitEvent = NULL;
        return 1;
    }

    /* Apply immediately at startup. Debouncing this call would skip the
     * initial application when the system uptime is less than five seconds.
     */
    ApplyCpuFrequencyLimit();

    dwResult = RegisterPowerNotification();
    if (dwResult != ERROR_SUCCESS) {
        PrintWin32Error("PowerRegisterSuspendResumeNotification", dwResult);
        SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
        CloseHandle(g_hExitEvent);
        g_hExitEvent = NULL;
        return 1;
    }

    dwWaitResult = WaitForSingleObject(g_hExitEvent, INFINITE);
    if (dwWaitResult == WAIT_FAILED) {
        PrintWin32Error("WaitForSingleObject", GetLastError());
        InterlockedExchange(&g_lExiting, TRUE);
    }

    UnregisterPowerNotification();

    SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
    CloseHandle(g_hExitEvent);
    g_hExitEvent = NULL;

    return (dwWaitResult == WAIT_FAILED) ? 1 : 0;
}
