#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdlib.h>
#include <string.h>

#include "os_info.h"

typedef LONG (WINAPI *rtl_get_version_fn)(OSVERSIONINFOW *version);

int os_info_get(os_info_t *info) {
    memset(info, 0, sizeof(*info));
    info->name = strdup("Windows");
    info->version.major = -1;
    info->version.minor = -1;
    info->version.patch = -1;
    if (info->name == NULL) {
        return -1;
    }

    /* GetVersionEx() can report a compatibility version when the executable
     * has no current-Windows manifest. RtlGetVersion() returns the running OS
     * version and is resolved dynamically so no extra import library is needed. */
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == NULL) {
        return 0;
    }
    rtl_get_version_fn rtl_get_version =
            (rtl_get_version_fn) (void *) GetProcAddress(ntdll, "RtlGetVersion");
    if (rtl_get_version == NULL) {
        return 0;
    }

    OSVERSIONINFOW version = {.dwOSVersionInfoSize = sizeof(version)};
    if (rtl_get_version(&version) == 0) {
        info->version.major = (int) version.dwMajorVersion;
        info->version.minor = (int) version.dwMinorVersion;
        info->version.patch = (int) version.dwBuildNumber;
    }
    return 0;
}
