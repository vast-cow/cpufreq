#ifndef CPUFREQ_MINGW_COMPAT_H
#define CPUFREQ_MINGW_COMPAT_H

/*
 * MinGW-w64 declares PowerRegisterSuspendResumeNotification(), but some
 * releases omit the callback-mode declarations supplied by the Windows SDK.
 * DEVICE_NOTIFY_CALLBACK is used as the feature test so that a future
 * MinGW-w64 which supplies the complete API does not see duplicate typedefs.
 */
#if defined(__MINGW32__) && !defined(DEVICE_NOTIFY_CALLBACK)
#include <windows.h>

#define DEVICE_NOTIFY_CALLBACK 0x00000002

typedef ULONG CALLBACK DEVICE_NOTIFY_CALLBACK_ROUTINE(
    PVOID Context,
    ULONG Type,
    PVOID Setting
);
typedef DEVICE_NOTIFY_CALLBACK_ROUTINE *PDEVICE_NOTIFY_CALLBACK_ROUTINE;

typedef struct _DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS {
    PDEVICE_NOTIFY_CALLBACK_ROUTINE Callback;
    PVOID Context;
} DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS, *PDEVICE_NOTIFY_SUBSCRIBE_PARAMETERS;
#endif

#endif /* CPUFREQ_MINGW_COMPAT_H */
