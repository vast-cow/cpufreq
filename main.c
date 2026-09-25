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

#ifndef DEVICE_NOTIFY_CALLBACK
/* MinGW-w64 declares the callback-based power APIs but some releases omit
 * the accompanying Windows SDK callback flag and parameter declarations.
 */
typedef ULONG (CALLBACK *DEVICE_NOTIFY_CALLBACK_ROUTINE)(
    PVOID Context,
    ULONG Type,
    PVOID Setting
);

typedef struct _DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS {
    DEVICE_NOTIFY_CALLBACK_ROUTINE Callback;
    PVOID Context;
} DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS;

#define DEVICE_NOTIFY_CALLBACK 2
#endif

#ifdef _MSC_VER
#pragma comment(lib, "PowrProf.lib")
#endif

/* =========================
 * Settings
 * ========================= */

#define DEFAULT_CPU_FREQ_AC_MHZ  800u
#define DEFAULT_CPU_FREQ_DC_MHZ  800u
#define CPU_FREQ_UNLIMITED_MHZ    0u
#define CPU_FREQ_MIN_MHZ          100u
#define CPU_FREQ_MAX_MHZ          64000u
#define CPU_FREQ_DEBOUNCE_MS      5000ULL

/* Apply settings to the currently active power scheme. Effective power modes
 * (also known as overlays) are monitored separately in resident mode.
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

/* GUID_ACTIVE_POWERSCHEME
 * 31F9F286-5084-42FE-B720-2B0264993763
 */
static const GUID g_guidActivePowerScheme = {
    0x31f9f286, 0x5084, 0x42fe,
    {0xb7, 0x20, 0x2b, 0x02, 0x64, 0x99, 0x37, 0x63}
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
static HPOWERNOTIFY g_hSuspendResumeNotify = NULL;
static HPOWERNOTIFY g_hActiveSchemeNotify = NULL;
static PVOID g_pEffectivePowerModeRegistration = NULL;
static HANDLE g_hExitEvent = NULL;
static DWORD g_dwAcMHz = DEFAULT_CPU_FREQ_AC_MHZ;
static DWORD g_dwDcMHz = DEFAULT_CPU_FREQ_DC_MHZ;

/* Serialize applications because Windows may invoke the notification callbacks
 * concurrently. This lock also protects the resume debounce timestamp.
 */
static SRWLOCK g_srwApplyLock = SRWLOCK_INIT;
static ULONGLONG g_ullLastApplyTick = 0;

/* Avoid a notification loop when reactivating the current scheme after an
 * update. Power-setting callbacks may be invoked concurrently.
 */
static SRWLOCK g_srwSchemeLock = SRWLOCK_INIT;
static GUID g_guidLastActiveScheme;
static BOOL g_fHaveLastActiveScheme = FALSE;

/* PowerRegisterForEffectivePowerModeNotifications was added after the minimum
 * supported Windows version, so use private ABI-compatible function pointer
 * declarations and resolve it at run time rather than importing it.
 */
typedef VOID (CALLBACK *CPUFREQ_EFFECTIVE_POWER_MODE_CALLBACK)(
    DWORD dwMode,
    PVOID pvContext
);
typedef HRESULT (WINAPI *CPUFREQ_POWER_REGISTER_EFFECTIVE_MODE)(
    ULONG ulVersion,
    CPUFREQ_EFFECTIVE_POWER_MODE_CALLBACK pfnCallback,
    PVOID pvContext,
    PVOID *ppvRegistrationHandle
);
typedef HRESULT (WINAPI *CPUFREQ_POWER_UNREGISTER_EFFECTIVE_MODE)(
    PVOID pvRegistrationHandle
);

#define CPUFREQ_EFFECTIVE_POWER_MODE_V1 1u

static CPUFREQ_POWER_UNREGISTER_EFFECTIVE_MODE
    g_pfnUnregisterEffectivePowerMode = NULL;
static SRWLOCK g_srwEffectiveModeLock = SRWLOCK_INIT;
static DWORD g_dwLastEffectivePowerMode = 0;
static BOOL g_fHaveLastEffectivePowerMode = FALSE;

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

static DWORD ApplyCpuFrequency(DWORD dwAcMHz, DWORD dwDcMHz)
{
    GUID *pSchemeGuid = NULL;
    DWORD dwResult;
    DWORD dwFirstError = ERROR_SUCCESS;
    SIZE_T i;

    /* Equivalent to SCHEME_CURRENT: ask Windows for the currently active scheme. */
    dwResult = PowerGetActiveScheme(NULL, &pSchemeGuid);

    if (dwResult != ERROR_SUCCESS) {
        PrintWin32Error("PowerGetActiveScheme", dwResult);
        return dwResult;
    }

    for (i = 0; i < ARRAYSIZE(g_rgProcessorFrequencySettings); ++i) {
        /* Equivalent to:
         * powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR <setting> <ac_mhz>
         */
        dwResult = PowerWriteACValueIndex(
            NULL,
            pSchemeGuid,
            &g_guidProcessorSettingsSubgroup,
            &g_rgProcessorFrequencySettings[i],
            dwAcMHz
        );
        if (dwResult != ERROR_SUCCESS) {
            PrintWin32Error("PowerWriteACValueIndex", dwResult);
            if (dwFirstError == ERROR_SUCCESS) {
                dwFirstError = dwResult;
            }
        }

        /* Equivalent to:
         * powercfg /setdcvalueindex SCHEME_CURRENT SUB_PROCESSOR <setting> <dc_mhz>
         */
        dwResult = PowerWriteDCValueIndex(
            NULL,
            pSchemeGuid,
            &g_guidProcessorSettingsSubgroup,
            &g_rgProcessorFrequencySettings[i],
            dwDcMHz
        );
        if (dwResult != ERROR_SUCCESS) {
            PrintWin32Error("PowerWriteDCValueIndex", dwResult);
            if (dwFirstError == ERROR_SUCCESS) {
                dwFirstError = dwResult;
            }
        }
    }

    if (dwFirstError == ERROR_SUCCESS) {
        /* Equivalent to: powercfg /setactive SCHEME_CURRENT
         * Changes to the active scheme take effect after it is re-activated.
         */
        dwResult = PowerSetActiveScheme(NULL, pSchemeGuid);
        if (dwResult != ERROR_SUCCESS) {
            PrintWin32Error("PowerSetActiveScheme", dwResult);
            dwFirstError = dwResult;
        }
    }

    /* PowerGetActiveScheme allocates the GUID; Windows requires LocalFree. */
    LocalFree(pSchemeGuid);

    return dwFirstError;
}

static DWORD ApplyCpuFrequencyLimit(void)
{
    return ApplyCpuFrequency(g_dwAcMHz, g_dwDcMHz);
}

static DWORD ClearCpuFrequencyLimit(void)
{
    /* 0 means unlimited/default for the processor maximum frequency setting. */
    return ApplyCpuFrequency(CPU_FREQ_UNLIMITED_MHZ, CPU_FREQ_UNLIMITED_MHZ);
}

/* =========================
 * Resume handling
 * ========================= */

static void ApplyCpuFrequencyLimitSerialized(
    BOOL fDebounceResume,
    const char *pszFailureContext)
{
    ULONGLONG ullNow;

    if (InterlockedCompareExchange(&g_lExiting, FALSE, FALSE) != FALSE) {
        return;
    }

    ullNow = GetTickCount64();

    AcquireSRWLockExclusive(&g_srwApplyLock);

    /* The two resume event types may arrive in quick succession. Other event
     * types are deduplicated by their own state and must not be time-debounced.
     */
    if (!fDebounceResume ||
        ullNow - g_ullLastApplyTick >= CPU_FREQ_DEBOUNCE_MS) {
        if (ApplyCpuFrequencyLimit() == ERROR_SUCCESS) {
            if (fDebounceResume) {
                /* Only successful applications suppress later resume events. */
                g_ullLastApplyTick = ullNow;
            }
        } else {
            fprintf(stderr, "error: failed to apply CPU frequency limit %s\n",
                    pszFailureContext);
        }
    }

    ReleaseSRWLockExclusive(&g_srwApplyLock);
}

static void ApplyCpuFrequencyLimitDebounced(void)
{
    ApplyCpuFrequencyLimitSerialized(TRUE, "after resume");
}

static BOOL IsNewActiveScheme(const GUID *pSchemeGuid)
{
    BOOL fChanged = FALSE;

    AcquireSRWLockExclusive(&g_srwSchemeLock);

    if (!g_fHaveLastActiveScheme ||
        !IsEqualGUID(&g_guidLastActiveScheme, pSchemeGuid)) {
        g_guidLastActiveScheme = *pSchemeGuid;
        g_fHaveLastActiveScheme = TRUE;
        fChanged = TRUE;
    }

    ReleaseSRWLockExclusive(&g_srwSchemeLock);

    return fChanged;
}

static ULONG CALLBACK PowerNotificationCallback(
    PVOID pvContext,
    ULONG ulEventType,
    PVOID pvSetting)
{
    UNREFERENCED_PARAMETER(pvContext);

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

    case PBT_POWERSETTINGCHANGE:
        if (pvSetting != NULL) {
            const POWERBROADCAST_SETTING *pSetting =
                (const POWERBROADCAST_SETTING *)pvSetting;

            if (IsEqualGUID(
                    &pSetting->PowerSetting,
                    &g_guidActivePowerScheme) &&
                pSetting->DataLength == sizeof(GUID)) {
                GUID guidNewScheme;

                /* Data is a byte array and is not guaranteed to be GUID-aligned. */
                memcpy(&guidNewScheme, pSetting->Data, sizeof(guidNewScheme));

                if (InterlockedCompareExchange(
                        &g_lExiting,
                        FALSE,
                        FALSE) == FALSE &&
                    IsNewActiveScheme(&guidNewScheme)) {
                    ApplyCpuFrequencyLimitSerialized(
                        FALSE,
                        "after power scheme change"
                    );
                }
            }
        }
        break;

    default:
        break;
    }

    return ERROR_SUCCESS;
}

static VOID CALLBACK EffectivePowerModeCallback(
    DWORD dwMode,
    PVOID pvContext)
{
    BOOL fChanged = FALSE;

    UNREFERENCED_PARAMETER(pvContext);

    AcquireSRWLockExclusive(&g_srwEffectiveModeLock);
    if (!g_fHaveLastEffectivePowerMode) {
        /* Registration immediately reports the current mode. Startup has just
         * applied the limit, so remember that first report without duplicating
         * the work.
         */
        g_fHaveLastEffectivePowerMode = TRUE;
        g_dwLastEffectivePowerMode = dwMode;
    } else if (g_dwLastEffectivePowerMode != dwMode) {
        g_dwLastEffectivePowerMode = dwMode;
        fChanged = TRUE;
    }
    ReleaseSRWLockExclusive(&g_srwEffectiveModeLock);

    if (fChanged) {
        ApplyCpuFrequencyLimitSerialized(
            FALSE,
            "after effective power mode change"
        );
    }
}

static DWORD RegisterSuspendResumeCallback(void)
{
    DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS subscribeParams;

    ZeroMemory(&subscribeParams, sizeof(subscribeParams));
    subscribeParams.Callback = PowerNotificationCallback;
    subscribeParams.Context = NULL;

    return PowerRegisterSuspendResumeNotification(
        DEVICE_NOTIFY_CALLBACK,
        (HANDLE)&subscribeParams,
        &g_hSuspendResumeNotify
    );
}

static DWORD RegisterActiveSchemeNotification(void)
{
    DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS subscribeParams;

    ZeroMemory(&subscribeParams, sizeof(subscribeParams));
    subscribeParams.Callback = PowerNotificationCallback;
    subscribeParams.Context = NULL;

    return PowerSettingRegisterNotification(
        &g_guidActivePowerScheme,
        DEVICE_NOTIFY_CALLBACK,
        (HANDLE)&subscribeParams,
        &g_hActiveSchemeNotify
    );
}

static DWORD RegisterEffectivePowerModeNotification(void)
{
    HMODULE hPowrProf;
    CPUFREQ_POWER_REGISTER_EFFECTIVE_MODE pfnRegister;
    FARPROC pfnRegisterAddress;
    FARPROC pfnUnregisterAddress;
    HRESULT hr;

    hPowrProf = GetModuleHandleW(L"powrprof.dll");
    if (hPowrProf == NULL) {
        /* PowrProf is already linked for the other power APIs. Treat this like
         * an unavailable optional API rather than preventing resident mode.
         */
        return ERROR_SUCCESS;
    }

    pfnRegisterAddress = GetProcAddress(
        hPowrProf,
        "PowerRegisterForEffectivePowerModeNotifications"
    );
    pfnUnregisterAddress = GetProcAddress(
        hPowrProf,
        "PowerUnregisterFromEffectivePowerModeNotifications"
    );
    if (pfnRegisterAddress == NULL || pfnUnregisterAddress == NULL) {
        return ERROR_SUCCESS;
    }

    /* GetProcAddress returns FARPROC. Copy its representation to the precise
     * signatures without triggering incompatible-function-cast diagnostics.
     */
    memcpy(&pfnRegister, &pfnRegisterAddress, sizeof(pfnRegister));
    memcpy(
        &g_pfnUnregisterEffectivePowerMode,
        &pfnUnregisterAddress,
        sizeof(g_pfnUnregisterEffectivePowerMode)
    );

    hr = pfnRegister(
        CPUFREQ_EFFECTIVE_POWER_MODE_V1,
        EffectivePowerModeCallback,
        NULL,
        &g_pEffectivePowerModeRegistration
    );
    if (FAILED(hr)) {
        DWORD dwError = HRESULT_CODE(hr);

        g_pfnUnregisterEffectivePowerMode = NULL;
        return dwError != ERROR_SUCCESS ? dwError : ERROR_GEN_FAILURE;
    }

    return ERROR_SUCCESS;
}

static void UnregisterPowerNotifications(void)
{
    HPOWERNOTIFY hPowerNotify;
    PVOID pvRegistration;

    pvRegistration = g_pEffectivePowerModeRegistration;
    if (pvRegistration != NULL) {
        g_pEffectivePowerModeRegistration = NULL;
        (void)g_pfnUnregisterEffectivePowerMode(pvRegistration);
        g_pfnUnregisterEffectivePowerMode = NULL;
    }

    /* Make cleanup idempotent because main() and atexit() may both call it. */
    hPowerNotify = g_hActiveSchemeNotify;
    if (hPowerNotify != NULL) {
        g_hActiveSchemeNotify = NULL;
        (void)PowerSettingUnregisterNotification(hPowerNotify);
    }

    hPowerNotify = g_hSuspendResumeNotify;
    if (hPowerNotify != NULL) {
        g_hSuspendResumeNotify = NULL;
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
        "stays resident and reapplies the limit after resume from sleep/hibernation\n"
        "or after the active Windows power scheme/effective power mode changes.\n"
        "\n"
        "Frequency options:\n"
        "  --mhz <MHz>         Set both AC and DC maximum frequency.\n"
        "  --ac-mhz <MHz>      Set the AC maximum frequency.\n"
        "  --dc-mhz <MHz>      Set the DC maximum frequency.\n"
        "\n"
        "  If --mhz is combined with --ac-mhz or --dc-mhz, the individual\n"
        "  AC/DC option takes precedence regardless of argument order.\n"
        "  Valid values are 0 (unlimited) or %lu-%lu MHz.\n"
        "  Defaults: AC=%lu MHz, DC=%lu MHz.\n"
        "\n"
        "Mode options:\n"
        "  --once              Apply the CPU frequency limit once, then exit.\n"
        "  --unthrottle-once   Clear the CPU frequency limit once, then exit.\n"
        "  --release-once      Alias for --unthrottle-once.\n"
        "  -h, --help          Show this help.\n",
        pszProgramName,
        (unsigned long)CPU_FREQ_MIN_MHZ,
        (unsigned long)CPU_FREQ_MAX_MHZ,
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

    if (*pdwValue != CPU_FREQ_UNLIMITED_MHZ &&
        (*pdwValue < CPU_FREQ_MIN_MHZ || *pdwValue > CPU_FREQ_MAX_MHZ)) {
        fprintf(
            stderr,
            "error: MHz value for %s must be 0 (unlimited) or %lu-%lu: %s\n",
            pszOptionName,
            (unsigned long)CPU_FREQ_MIN_MHZ,
            (unsigned long)CPU_FREQ_MAX_MHZ,
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
        return (ApplyCpuFrequencyLimit() == ERROR_SUCCESS) ? 0 : 1;
    }

    if (options.RunMode == RUN_MODE_UNTHROTTLE_ONCE) {
        return (ClearCpuFrequencyLimit() == ERROR_SUCCESS) ? 0 : 1;
    }

    if (atexit(UnregisterPowerNotifications) != 0) {
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
    if (ApplyCpuFrequencyLimit() != ERROR_SUCCESS) {
        fprintf(stderr, "error: failed to apply CPU frequency limit at startup\n");
        SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
        CloseHandle(g_hExitEvent);
        g_hExitEvent = NULL;
        return 1;
    }

    dwResult = RegisterSuspendResumeCallback();
    if (dwResult != ERROR_SUCCESS) {
        PrintWin32Error("PowerRegisterSuspendResumeNotification", dwResult);
        SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
        CloseHandle(g_hExitEvent);
        g_hExitEvent = NULL;
        return 1;
    }

    dwResult = RegisterActiveSchemeNotification();
    if (dwResult != ERROR_SUCCESS) {
        PrintWin32Error("PowerSettingRegisterNotification", dwResult);
        UnregisterPowerNotifications();
        SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
        CloseHandle(g_hExitEvent);
        g_hExitEvent = NULL;
        return 1;
    }

    dwResult = RegisterEffectivePowerModeNotification();
    if (dwResult != ERROR_SUCCESS) {
        PrintWin32Error(
            "PowerRegisterForEffectivePowerModeNotifications",
            dwResult
        );
        UnregisterPowerNotifications();
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

    UnregisterPowerNotifications();

    SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
    CloseHandle(g_hExitEvent);
    g_hExitEvent = NULL;

    return (dwWaitResult == WAIT_FAILED) ? 1 : 0;
}
