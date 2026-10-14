/* See diag_report.h: everything a support person needs, blind, in one text report. */
#include "diag_report.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL.h>

#include "app.h"
#include "logging.h"
#include "backend/pcmanager.h"
#include "backend/types.h"
#include "input/input_gamepad.h"
#include "stream/wired_nic.h"
#include "util/diag_logs.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#define COBJMACROS
#include <initguid.h>
#include <dxgi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/utsname.h>
#include <unistd.h>
#if defined(TARGET_ANDROID)
#include <dlfcn.h>
#include "platform/android/android_platform.h"
#endif
#endif

#ifndef ECLIPSE_BUILD_COMMIT
#define ECLIPSE_BUILD_COMMIT "unknown"
#endif

extern pcmanager_t *pcmanager;

/* ---------------------------------------------------------------------------------------------------
 * TEXT BUILDER
 * ------------------------------------------------------------------------------------------------- */

typedef struct {
    char *s;
    size_t len, cap;
} sbuf_t;

static void sb_add(sbuf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void sb_add(sbuf_t *b, const char *fmt, ...) {
    for (;;) {
        va_list ap;
        va_start(ap, fmt);
        const size_t room = b->cap - b->len;
        const int n = b->s != NULL ? vsnprintf(b->s + b->len, room, fmt, ap) : -2;
        va_end(ap);
        if (n >= 0 && (size_t) n < room) {
            b->len += (size_t) n;
            return;
        }
        const size_t want = b->cap * 2 + (n > 0 ? (size_t) n : 0) + 4096;
        char *grown = realloc(b->s, want);
        if (grown == NULL) {
            return;   /* out of memory: keep what we have */
        }
        b->s = grown;
        b->cap = want;
        if (b->len == 0) {
            b->s[0] = '\0';
        }
    }
}

/* ---------------------------------------------------------------------------------------------------
 * SHA-256 OF THE RUNNING BINARY -- the version number is not bumped per build, so this is what says which
 * build a log came from (the same hashes the test notes use). Small self-contained implementation.
 * ------------------------------------------------------------------------------------------------- */

typedef struct {
    uint32_t h[8];
    uint64_t bits;
    uint8_t block[64];
    size_t used;
} sha256_t;

static const uint32_t K256[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(sha256_t *c, const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t) p[i * 4] << 24 | (uint32_t) p[i * 4 + 1] << 16 | (uint32_t) p[i * 4 + 2] << 8 | p[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        const uint32_t s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
    for (int i = 0; i < 64; i++) {
        const uint32_t t1 = h + (ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        const uint32_t t2 = (ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = cc;
        cc = b;
        b = a;
        a = t1 + t2;
    }
    c->h[0] += a;
    c->h[1] += b;
    c->h[2] += cc;
    c->h[3] += d;
    c->h[4] += e;
    c->h[5] += f;
    c->h[6] += g;
    c->h[7] += h;
}

static void sha256_init(sha256_t *c) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(c->h, iv, sizeof(iv));
    c->bits = 0;
    c->used = 0;
}

static void sha256_update(sha256_t *c, const uint8_t *p, size_t n) {
    c->bits += (uint64_t) n * 8;
    while (n > 0) {
        const size_t take = 64 - c->used < n ? 64 - c->used : n;
        memcpy(c->block + c->used, p, take);
        c->used += take;
        p += take;
        n -= take;
        if (c->used == 64) {
            sha256_block(c, c->block);
            c->used = 0;
        }
    }
}

static void sha256_hex(sha256_t *c, char out[65]) {
    const uint64_t bits = c->bits;
    const uint8_t one = 0x80, zero = 0;
    sha256_update(c, &one, 1);
    while (c->used != 56) {
        sha256_update(c, &zero, 1);
    }
    uint8_t len[8];
    for (int i = 0; i < 8; i++) {
        len[i] = (uint8_t) (bits >> (56 - i * 8));
    }
    sha256_update(c, len, 8);
    for (int i = 0; i < 8; i++) {
        snprintf(out + i * 8, 9, "%08x", c->h[i]);
    }
}

static char binary_path[1024];
static long long binary_size = -1;
static char binary_sha256[65];   /* empty until the worker has finished */

static FILE *open_own_binary(void) {
#if defined(_WIN32)
    wchar_t wpath[1024];
    DWORD n = GetModuleFileNameW(NULL, wpath, 1024);
    if (n == 0 || n >= 1024) {
        return NULL;
    }
    WideCharToMultiByte(CP_UTF8, 0, wpath, -1, binary_path, sizeof(binary_path), NULL, NULL);
    return _wfopen(wpath, L"rb");
#elif defined(TARGET_ANDROID)
    /* The program is libmain.so (or, in the co-op peer, the peer executable): /proc/self/exe is the zygote's
     * app_process, the same on every Android device. */
    Dl_info info;
    if (dladdr((const void *) open_own_binary, &info) == 0 || info.dli_fname == NULL) {
        return NULL;
    }
    snprintf(binary_path, sizeof(binary_path), "%s", info.dli_fname);
    return fopen(binary_path, "rb");
#else
    ssize_t n = readlink("/proc/self/exe", binary_path, sizeof(binary_path) - 1);
    if (n <= 0) {
        return NULL;
    }
    binary_path[n] = '\0';
    return fopen("/proc/self/exe", "rb");
#endif
}

static int hash_binary_thread(void *arg) {
    (void) arg;
    FILE *f = open_own_binary();
    if (f == NULL) {
        commons_log_warn("Report", "Binary: cannot open the running executable to hash it");
        return 0;
    }
    sha256_t c;
    sha256_init(&c);
    static uint8_t buf[65536];
    size_t got;
    long long total = 0;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
        sha256_update(&c, buf, got);
        total += (long long) got;
    }
    fclose(f);
    char hex[65];
    sha256_hex(&c, hex);
    binary_size = total;
    memcpy(binary_sha256, hex, sizeof(hex));
    commons_log_info("Report", "Binary: %s, %lld bytes, sha256 %s", binary_path, total, hex);
    return 0;
}

/* ---------------------------------------------------------------------------------------------------
 * SMALL HELPERS
 * ------------------------------------------------------------------------------------------------- */

#if !defined(_WIN32)
static bool read_first_line(const char *path, char *dest, size_t len) {
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return false;
    }
    bool ok = fgets(dest, (int) len, f) != NULL;
    fclose(f);
    if (ok) {
        dest[strcspn(dest, "\r\n")] = '\0';
    }
    return ok;
}

/* "key<sep>value" from a text file: the value of the first line starting with key. */
static bool read_keyed(const char *path, const char *key, char sep, char *dest, size_t len) {
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return false;
    }
    char line[512];
    bool found = false;
    const size_t klen = strlen(key);
    while (!found && fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, key, klen) != 0) {
            continue;
        }
        char *v = strchr(line + klen, sep);
        if (v == NULL) {
            continue;
        }
        v++;
        while (*v == ' ' || *v == '\t' || *v == '"') {
            v++;
        }
        v[strcspn(v, "\"\r\n")] = '\0';
        snprintf(dest, len, "%s", v);
        found = true;
    }
    fclose(f);
    return found;
}
#endif

#if defined(TARGET_WEBOS)
/* "key":"value" out of a small JSON file, without a parser. */
static bool json_value(const char *path, const char *key, char *dest, size_t len) {
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return false;
    }
    char text[8192];
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = '\0';
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    char *p = strstr(text, needle);
    if (p == NULL) {
        return false;
    }
    p = strchr(p + strlen(needle), ':');
    if (p == NULL) {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '"') {
        p++;
    }
    size_t end = strcspn(p, "\",}\r\n");
    snprintf(dest, len, "%.*s", (int) (end < len ? end : len - 1), p);
    return true;
}
#endif

static const char *yes_no(bool v) {
    return v ? "yes" : "no";
}

/* ---------------------------------------------------------------------------------------------------
 * SECTIONS
 * ------------------------------------------------------------------------------------------------- */

static void section_identity(sbuf_t *b, const char *title) {
    const char *platform =
#if defined(TARGET_WEBOS)
            "webOS";
#elif defined(TARGET_ANDROID)
            "Android";
#elif defined(_WIN32)
            "Windows";
#elif defined(__linux__)
            "Linux";
#else
            "other";
#endif
    sb_add(b, "=== %s ===\n", title);
    sb_add(b, "App: Eclipse %s for %s, commit %s", APP_VERSION, platform, ECLIPSE_BUILD_COMMIT);
#ifdef APPID
    sb_add(b, ", app id %s", APPID);
#endif
    sb_add(b, "\n");
    if (binary_sha256[0] != '\0') {
        sb_add(b, "Binary: %s, %lld bytes, sha256 %s\n", binary_path, binary_size, binary_sha256);
    } else {
        sb_add(b, "Binary: (hash not finished yet -- see the \"Binary:\" line in the log)\n");
    }
    time_t now = time(NULL);
    struct tm local_tm, utc_tm;
#if defined(_WIN32)
    localtime_s(&local_tm, &now);
    gmtime_s(&utc_tm, &now);
#else
    localtime_r(&now, &local_tm);
    gmtime_r(&now, &utc_tm);
#endif
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local_tm);
    utc_tm.tm_isdst = local_tm.tm_isdst;
    const long offset_min = (long) difftime(mktime(&local_tm), mktime(&utc_tm)) / 60;
    sb_add(b, "Time: %s (UTC%+ld:%02ld), this run up %u s\n", stamp, offset_min / 60,
           labs(offset_min % 60), (unsigned) (SDL_GetTicks() / 1000));
}

static void section_system(sbuf_t *b, app_t *app) {
    char *os_str = os_info_str(&app->os_info);
    sb_add(b, "OS: %s\n", os_str != NULL ? os_str : "unknown");
    free(os_str);
#if defined(_WIN32)
    char product[128] = "", display_version[64] = "", build[32] = "";
    DWORD size, ubr = 0;
    const char *key = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    size = sizeof(product);
    RegGetValueA(HKEY_LOCAL_MACHINE, key, "ProductName", RRF_RT_REG_SZ, NULL, product, &size);
    size = sizeof(display_version);
    RegGetValueA(HKEY_LOCAL_MACHINE, key, "DisplayVersion", RRF_RT_REG_SZ, NULL, display_version, &size);
    size = sizeof(build);
    RegGetValueA(HKEY_LOCAL_MACHINE, key, "CurrentBuild", RRF_RT_REG_SZ, NULL, build, &size);
    size = sizeof(ubr);
    RegGetValueA(HKEY_LOCAL_MACHINE, key, "UBR", RRF_RT_REG_DWORD, NULL, &ubr, &size);
    sb_add(b, "Windows: %s %s, build %s.%lu\n", product, display_version, build, (unsigned long) ubr);
    char cpu[128] = "";
    size = sizeof(cpu);
    RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString",
                 RRF_RT_REG_SZ, NULL, cpu, &size);
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    MEMORYSTATUSEX mem = {.dwLength = sizeof(mem)};
    GlobalMemoryStatusEx(&mem);
    sb_add(b, "CPU: %s, %lu logical processors; memory %llu MB total, %llu MB free\n", cpu,
           (unsigned long) si.dwNumberOfProcessors, (unsigned long long) (mem.ullTotalPhys >> 20),
           (unsigned long long) (mem.ullAvailPhys >> 20));
    IDXGIFactory1 *factory = NULL;
    if (SUCCEEDED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **) &factory))) {
        IDXGIAdapter1 *adapter = NULL;
        for (UINT i = 0; IDXGIFactory1_EnumAdapters1(factory, i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 desc;
            if (SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter, &desc))) {
                char name[128];
                WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), NULL, NULL);
                LARGE_INTEGER umd = {0};
                char driver[32] = "unknown";
                if (SUCCEEDED(IDXGIAdapter1_CheckInterfaceSupport(adapter, &IID_IDXGIDevice, &umd))) {
                    snprintf(driver, sizeof(driver), "%u.%u.%u.%u", (unsigned) HIWORD(umd.HighPart),
                             (unsigned) LOWORD(umd.HighPart), (unsigned) HIWORD(umd.LowPart),
                             (unsigned) LOWORD(umd.LowPart));
                }
                sb_add(b, "GPU %u: %s (vendor %04x device %04x), %llu MB dedicated, driver %s%s\n", i, name,
                       desc.VendorId, desc.DeviceId, (unsigned long long) (desc.DedicatedVideoMemory >> 20), driver,
                       (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? " (software)" : "");
            }
            IDXGIAdapter1_Release(adapter);
        }
        IDXGIFactory1_Release(factory);
    }
#else
    struct utsname u;
    if (uname(&u) == 0) {
        sb_add(b, "Kernel: %s %s %s (%s)\n", u.sysname, u.release, u.machine, u.version);
    }
#if defined(TARGET_WEBOS)
    char fw[64] = "", built[64] = "", core[64] = "", release[64] = "";
    json_value("/var/run/nyx/os_info.json", "webos_manufacturing_version", fw, sizeof(fw));
    json_value("/var/run/nyx/os_info.json", "webos_build_datetime", built, sizeof(built));
    json_value("/var/run/nyx/os_info.json", "core_os_release", core, sizeof(core));
    json_value("/var/run/nyx/os_info.json", "webos_release", release, sizeof(release));
    sb_add(b, "webOS: release %s, firmware %s (built %s), core %s\n", release, fw, built, core);
    char model[96] = "", hw[96] = "";
    json_value("/var/run/nyx/device_info.json", "device_name", model, sizeof(model));
    json_value("/var/run/nyx/device_info.json", "hardware_id", hw, sizeof(hw));
    if (model[0] != '\0' || hw[0] != '\0') {
        sb_add(b, "Device: %s %s\n", model, hw);
    }
#elif defined(TARGET_ANDROID)
    sb_add(b, "Android: API %d; device %s\n", android_sdk_int(), android_device_name());
#elif defined(__linux__)
    char distro[128] = "unknown";
    read_keyed("/etc/os-release", "PRETTY_NAME", '=', distro, sizeof(distro));
    const char *session = getenv("XDG_SESSION_TYPE"), *desktop = getenv("XDG_CURRENT_DESKTOP");
    const char *wayland = getenv("WAYLAND_DISPLAY"), *x11 = getenv("DISPLAY");
    sb_add(b, "Distribution: %s; session %s, desktop %s, WAYLAND_DISPLAY=%s, DISPLAY=%s\n", distro,
           session ? session : "-", desktop ? desktop : "-", wayland ? wayland : "-", x11 ? x11 : "-");
    for (int i = 0; i < 8; i++) {
        char path[96], vendor[16] = "", device[16] = "", driver[256] = "";
        snprintf(path, sizeof(path), "/sys/class/drm/card%d/device/vendor", i);
        if (!read_first_line(path, vendor, sizeof(vendor))) {
            continue;
        }
        snprintf(path, sizeof(path), "/sys/class/drm/card%d/device/device", i);
        read_first_line(path, device, sizeof(device));
        snprintf(path, sizeof(path), "/sys/class/drm/card%d/device/driver", i);
        ssize_t n = readlink(path, driver, sizeof(driver) - 1);
        driver[n > 0 ? n : 0] = '\0';
        const char *slash = strrchr(driver, '/');
        sb_add(b, "GPU card%d: vendor %s device %s, driver %s\n", i, vendor, device, slash ? slash + 1 : "-");
    }
#endif
    char cpu[128] = "";
    if (!read_keyed("/proc/cpuinfo", "model name", ':', cpu, sizeof(cpu))) {
        if (!read_keyed("/proc/cpuinfo", "Hardware", ':', cpu, sizeof(cpu))) {
            read_keyed("/proc/cpuinfo", "Processor", ':', cpu, sizeof(cpu));
        }
    }
    char total[32] = "?", avail[32] = "?";
    read_keyed("/proc/meminfo", "MemTotal", ':', total, sizeof(total));
    read_keyed("/proc/meminfo", "MemAvailable", ':', avail, sizeof(avail));
    sb_add(b, "CPU: %s, %ld online; memory %s total, %s available\n", cpu[0] ? cpu : "unknown",
           sysconf(_SC_NPROCESSORS_ONLN), total, avail);
#endif
    SDL_version linked;
    SDL_GetVersion(&linked);
    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
        sb_add(b, "SDL %d.%d.%d (video and audio not started yet)\n", linked.major, linked.minor, linked.patch);
        return;
    }
    const char *vdrv = SDL_GetCurrentVideoDriver(), *adrv = SDL_GetCurrentAudioDriver();
    sb_add(b, "SDL %d.%d.%d: video driver %s, audio driver %s\n", linked.major, linked.minor, linked.patch,
           vdrv ? vdrv : "(none)", adrv ? adrv : "(none)");
}

static void section_storage(sbuf_t *b, app_t *app) {
    char dir[1024];
    diag_logs_dir(app->settings.conf_dir, dir, sizeof(dir));
    sb_add(b, "Settings folder: %s (persistent: %s, settings can be saved: %s)\n",
           app->settings.conf_dir ? app->settings.conf_dir : "?", yes_no(app->settings.conf_persistent),
           yes_no(app->settings.settings_save_allowed));
    sb_add(b, "Kept logs: %s, folder %s\n", diag_logs_active() ? "on" : "off", dir);
}

static void section_displays(sbuf_t *b) {
    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
        sb_add(b, "Displays: video not initialised\n");
        return;
    }
    const int n = SDL_GetNumVideoDisplays();
    for (int i = 0; i < n; i++) {
        SDL_DisplayMode cur = {0}, desk = {0};
        SDL_GetCurrentDisplayMode(i, &cur);
        SDL_GetDesktopDisplayMode(i, &desk);
        float ddpi = 0, hdpi = 0, vdpi = 0;
        SDL_GetDisplayDPI(i, &ddpi, &hdpi, &vdpi);
        const char *name = SDL_GetDisplayName(i);
        sb_add(b, "Display %d: %s, current %dx%d@%d, desktop %dx%d@%d, %.0f dpi\n", i, name ? name : "?", cur.w,
               cur.h, cur.refresh_rate, desk.w, desk.h, desk.refresh_rate, ddpi);
    }
#if defined(TARGET_WEBOS)
    int pw = 0, ph = 0, rate = 0;
    SDL_webOSGetPanelResolution(&pw, &ph);
    SDL_webOSGetRefreshRate(&rate);
    sb_add(b, "Panel: %dx%d, %d Hz\n", pw, ph, rate);
#elif defined(TARGET_ANDROID)
    int pw = 0, ph = 0, rate_x100 = 0, max_x100 = 0;
    bool hdr10 = false;
    if (android_display_mode(&pw, &ph, &rate_x100, &max_x100, &hdr10)) {
        sb_add(b, "Panel: %dx%d, %d.%02d Hz now (up to %d.%02d Hz at this size), HDR10 %s\n", pw, ph,
               rate_x100 / 100, rate_x100 % 100, max_x100 / 100, max_x100 % 100, yes_no(hdr10));
    }
#endif
}

static void section_media(sbuf_t *b, app_t *app) {
    const char *vid = SS4S_ModuleInfoGetId(app->ss4s.selection.video_module);
    const char *aud = SS4S_ModuleInfoGetId(app->ss4s.selection.audio_module);
    const SS4S_VideoCapabilities *vc = &app->ss4s.video_cap;
    sb_add(b, "Video decoder: %s (setting: %s) -- codecs%s%s%s%s%s, max %ux%u@%u, max bitrate %u kbps "
              "(suggested %u), HDR %s, full range %s, colour space %d, UI transform 0x%x\n",
           vid ? vid : "none", app->settings.decoder ? app->settings.decoder : "auto",
           (vc->codecs & SS4S_VIDEO_H264) ? " H.264" : "", (vc->codecs & SS4S_VIDEO_H265) ? " HEVC" : "",
           (vc->codecs & SS4S_VIDEO_AV1) ? " AV1" : "", (vc->codecs & SS4S_VIDEO_VP9) ? " VP9" : "",
           vc->codecs == 0 ? " (none reported)" : "", vc->maxWidth, vc->maxHeight, vc->maxFps, vc->maxBitrate,
           vc->suggestedBitrate, yes_no(vc->hdr), yes_no(vc->fullColorRange), (int) vc->colorSpace,
           (unsigned) vc->transform);
    sb_add(b, "Audio output: %s (setting: %s, device: %s) -- max %u channels, codecs 0x%x\n", aud ? aud : "none",
           app->settings.audio_backend ? app->settings.audio_backend : "auto",
           app->settings.audio_device ? app->settings.audio_device : "default", app->ss4s.audio_cap.maxChannels,
           (unsigned) app->ss4s.audio_cap.codecs);
    if (SDL_WasInit(SDL_INIT_AUDIO)) {
        const int n = SDL_GetNumAudioDevices(0);
        for (int i = 0; i < n; i++) {
            const char *name = SDL_GetAudioDeviceName(i, 0);
            sb_add(b, "  audio device %d: %s\n", i, name ? name : "?");
        }
    }
}

static const char *pad_type_name(SDL_GameController *gc) {
#if SDL_VERSION_ATLEAST(2, 0, 12)
    switch (SDL_GameControllerGetType(gc)) {
        case SDL_CONTROLLER_TYPE_XBOX360:
            return "Xbox 360";
        case SDL_CONTROLLER_TYPE_XBOXONE:
            return "Xbox One/Series";
        case SDL_CONTROLLER_TYPE_PS3:
            return "PS3";
        case SDL_CONTROLLER_TYPE_PS4:
            return "PS4";
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
            return "Switch Pro";
#if SDL_VERSION_ATLEAST(2, 0, 14)
        case SDL_CONTROLLER_TYPE_PS5:
            return "PS5";
#endif
        default:
            break;
    }
#endif
    (void) gc;
    return "generic";
}

static void section_input(sbuf_t *b, app_t *app) {
    app_input_t *input = &app->input;
    sb_add(b, "Controllers in use: %d\n", (int) input->gamepads_count);
    for (size_t i = 0; i < input->gamepads_count; i++) {
        app_gamepad_state_t *gp = &input->gamepads[i];
        if (gp->controller == NULL) {
            continue;
        }
        char guid[40];
        SDL_JoystickGetGUIDString(gp->guid, guid, sizeof(guid));
        const char *name = SDL_GameControllerName(gp->controller);
        sb_add(b, "  player slot %d: %s (%s, %04x:%04x, GUID %s), Nintendo %s, A/B X/Y swapped %s\n", gp->gs_id + 1,
               name ? name : "?", pad_type_name(gp->controller), SDL_GameControllerGetVendor(gp->controller),
               SDL_GameControllerGetProduct(gp->controller), guid,
               yes_no(app_input_gamepad_is_nintendo(gp->controller)),
               yes_no(app_input_gamepad_swaps_abxy(gp->controller)));
    }
    if (!SDL_WasInit(SDL_INIT_JOYSTICK)) {
        return;
    }
    const int n = SDL_NumJoysticks();
    sb_add(b, "Input devices seen by SDL: %d\n", n);
    for (int i = 0; i < n; i++) {
        char guid[40];
        SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i), guid, sizeof(guid));
        const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
        bool used = false;
        for (size_t j = 0; j < input->gamepads_count; j++) {
            used = used || input->gamepads[j].instance_id == id;
        }
        const char *name = SDL_JoystickNameForIndex(i);
        sb_add(b, "  %s (GUID %s, %04x:%04x)%s%s\n", name ? name : "?", guid, SDL_JoystickGetDeviceVendor(i),
               SDL_JoystickGetDeviceProduct(i), SDL_IsGameController(i) ? "" : ", no controller mapping",
               used ? "" : ", NOT used as a controller");
    }
}

static void section_network(sbuf_t *b, app_t *app) {
#if defined(_WIN32)
    ULONG size = 16384;
    IP_ADAPTER_ADDRESSES *list = NULL;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && rc == ERROR_BUFFER_OVERFLOW; attempt++) {
        free(list);
        list = malloc(size);
        if (list == NULL) {
            return;
        }
        rc = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST |
                                             GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, NULL, list, &size);
    }
    if (rc != NO_ERROR) {
        sb_add(b, "Network: GetAdaptersAddresses failed (%lu)\n", (unsigned long) rc);
        free(list);
        return;
    }
    for (IP_ADAPTER_ADDRESSES *a = list; a != NULL; a = a->Next) {
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }
        char name[128], desc[160];
        WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, name, sizeof(name), NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, a->Description, -1, desc, sizeof(desc), NULL, NULL);
        char mac[32] = "";
        size_t mac_used = 0;
        for (ULONG i = 0; i < a->PhysicalAddressLength && i < 8; i++) {
            mac_used += (size_t) snprintf(mac + mac_used, sizeof(mac) - mac_used, "%s%02x", i ? ":" : "",
                                          a->PhysicalAddress[i]);
        }
        sb_add(b, "Interface %s (%s): %s, %s, %llu Mb/s, MAC %s\n", name, desc,
               a->OperStatus == IfOperStatusUp ? "up" : "down",
               a->IfType == IF_TYPE_IEEE80211 ? "Wi-Fi" : a->IfType == IF_TYPE_ETHERNET_CSMACD ? "Ethernet" : "other",
               (unsigned long long) (a->TransmitLinkSpeed / 1000000), mac);
        for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u != NULL; u = u->Next) {
            char ip[64] = "";
            getnameinfo(u->Address.lpSockaddr, u->Address.iSockaddrLength, ip, sizeof(ip), NULL, 0, NI_NUMERICHOST);
            sb_add(b, "  address %s/%u\n", ip, (unsigned) u->OnLinkPrefixLength);
        }
        for (IP_ADAPTER_GATEWAY_ADDRESS *g = a->FirstGatewayAddress; g != NULL; g = g->Next) {
            char ip[64] = "";
            getnameinfo(g->Address.lpSockaddr, g->Address.iSockaddrLength, ip, sizeof(ip), NULL, 0, NI_NUMERICHOST);
            sb_add(b, "  gateway %s\n", ip);
        }
    }
    free(list);
    (void) app;
#else
    struct ifaddrs *all = NULL;
    if (getifaddrs(&all) != 0) {
        sb_add(b, "Network: getifaddrs failed\n");
        return;
    }
    char seen[16][IFNAMSIZ];
    int seen_count = 0;
    for (struct ifaddrs *a = all; a != NULL; a = a->ifa_next) {
        if (a->ifa_name == NULL || (a->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }
        bool known = false;
        for (int i = 0; i < seen_count; i++) {
            known = known || strcmp(seen[i], a->ifa_name) == 0;
        }
        if (known || seen_count >= 16) {
            continue;
        }
        snprintf(seen[seen_count++], IFNAMSIZ, "%s", a->ifa_name);
        char path[128], mac[24] = "?", oper[16] = "?", speed[16] = "?";
        snprintf(path, sizeof(path), "/sys/class/net/%s/address", a->ifa_name);
        read_first_line(path, mac, sizeof(mac));
        snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", a->ifa_name);
        read_first_line(path, oper, sizeof(oper));
        snprintf(path, sizeof(path), "/sys/class/net/%s/speed", a->ifa_name);
        read_first_line(path, speed, sizeof(speed));
        snprintf(path, sizeof(path), "/sys/class/net/%s/wireless", a->ifa_name);
        const bool wireless = access(path, F_OK) == 0;
        sb_add(b, "Interface %s: %s (flags %s), %s, %s Mb/s, MAC %s\n", a->ifa_name, oper,
               (a->ifa_flags & IFF_UP) ? "UP" : "down", wireless ? "Wi-Fi" : "wired/other", speed, mac);
        for (struct ifaddrs *c = all; c != NULL; c = c->ifa_next) {
            if (c->ifa_name == NULL || strcmp(c->ifa_name, a->ifa_name) != 0 || c->ifa_addr == NULL) {
                continue;
            }
            char ip[64] = "";
            int prefix = 0;
            if (c->ifa_addr->sa_family == AF_INET) {
                inet_ntop(AF_INET, &((struct sockaddr_in *) c->ifa_addr)->sin_addr, ip, sizeof(ip));
                if (c->ifa_netmask != NULL) {
                    uint32_t m = ntohl(((struct sockaddr_in *) c->ifa_netmask)->sin_addr.s_addr);
                    while (m & 0x80000000u) {
                        prefix++;
                        m <<= 1;
                    }
                }
                sb_add(b, "  address %s/%d\n", ip, prefix);
            } else if (c->ifa_addr->sa_family == AF_INET6) {
                inet_ntop(AF_INET6, &((struct sockaddr_in6 *) c->ifa_addr)->sin6_addr, ip, sizeof(ip));
                sb_add(b, "  address %s\n", ip);
            }
        }
    }
    freeifaddrs(all);
    FILE *route = fopen("/proc/net/route", "r");
    if (route != NULL) {
        char line[256];
        while (fgets(line, sizeof(line), route) != NULL) {
            char iface[32];
            unsigned dest, gw, flags;
            int metric;
            if (sscanf(line, "%31s %x %x %x %*d %*d %d", iface, &dest, &gw, &flags, &metric) == 5 && dest == 0) {
                struct in_addr g = {.s_addr = gw};
                sb_add(b, "Default route: via %s on %s (metric %d)\n", inet_ntoa(g), iface, metric);
            }
        }
        fclose(route);
    }
#if defined(TARGET_WEBOS)
    wired_nic_t nic;
    const bool wired = wired_nic_discover(&nic);
    sb_add(b, "Wired network setting: %s; wired card %s%s%s\n",
           app->settings.wired_pin_mode == WIRED_PIN_OFF ? "Off" :
           app->settings.wired_pin_mode == WIRED_PIN_ON ? "On" : "Automatic",
           wired ? nic.name : "none up", wired ? " " : "", wired ? nic.mac : "");
#else
    (void) app;
#endif
#endif
}

static void host_codecs(char *dest, size_t len, int mask) {
    snprintf(dest, len, "%s%s%s%s%s%s%s", (mask & SCM_H264) ? " H.264" : "", (mask & SCM_HEVC) ? " HEVC" : "",
             (mask & SCM_HEVC_MAIN10) ? " HEVC-Main10" : "", (mask & SCM_AV1_MAIN8) ? " AV1" : "",
             (mask & SCM_AV1_MAIN10) ? " AV1-Main10" : "", (mask & SCM_MASK_YUV444) ? " 4:4:4" : "",
             mask == 0 ? " (not reported)" : "");
}

static const char *host_state_name(int code) {
    switch (code) {
        case SERVER_STATE_AVAILABLE:
            return "online, paired";
        case SERVER_STATE_NOT_PAIRED:
            return "online, NOT paired";
        case SERVER_STATE_ONLINE:
            return "online";
        case SERVER_STATE_QUERYING:
            return "checking";
        case SERVER_STATE_OFFLINE:
            return "OFFLINE";
        case SERVER_STATE_ERROR:
            return "ERROR";
        default:
            return "not checked yet";
    }
}

/* One computer, as one line (also the change-detection key for diag_report_log_host). */
static void host_line(char *dest, size_t len, const pclist_t *node) {
    const SERVER_DATA *s = node->server;
    if (s == NULL) {
        snprintf(dest, len, "(no data) %s", host_state_name(node->state.code));
        return;
    }
    char codecs[96], now[48];
    host_codecs(codecs, sizeof(codecs), s->codecModeSupport);
    if (s->currentGame) {
        snprintf(now, sizeof(now), "RUNNING app %d", s->currentGame);
    } else {
        snprintf(now, sizeof(now), "free");
    }
    snprintf(dest, len,
             "%s at %s (%s)%s: %s; host %s%s, GFE-version %s; GPU %s, encoder %s; codecs%s; 4K %s, HDR %s; "
             "Umbra caps %s%d; now %s; lock screen %s; MAC %s",
             s->hostname ? s->hostname : "?", s->serverInfo.address ? s->serverInfo.address : "?",
             s->uuid ? s->uuid : "?", node->selected ? " [selected]" : "", host_state_name(node->state.code),
             s->serverInfo.serverInfoAppVersion ? s->serverInfo.serverInfoAppVersion : "?",
             s->isGfe ? " (NVIDIA GameStream)" : "",
             s->serverInfo.serverInfoGfeVersion ? s->serverInfo.serverInfoGfeVersion : "-",
             s->gpuType && s->gpuType[0] ? s->gpuType : "not reported", s->encoderVendorKnown && s->encoderVendor ? s->encoderVendor : "not reported",
             codecs, yes_no(s->supports4K), yes_no(s->supportsHdr), s->umbraCapsKnown ? "" : "not reported/",
             s->umbraCaps, now,
             s->sessionLockedKnown ? yes_no(s->sessionLocked) : "not reported", s->mac ? s->mac : "?");
}

static void section_hosts(sbuf_t *b) {
    if (pcmanager == NULL) {
        return;
    }
    int n = 0;
    for (const pclist_t *node = pcmanager_servers(pcmanager); node != NULL; node = node->next) {
        char line[1024];
        host_line(line, sizeof(line), node);
        sb_add(b, "Computer %d: %s%s\n", ++n, line, node->gpu_vendor ? " (GPU make set by the user)" : "");
    }
    if (n == 0) {
        sb_add(b, "Computers: none known\n");
    }
}

static void section_settings(sbuf_t *b, app_t *app) {
    const app_settings_t *c = &app->settings;
    sb_add(b, "Stream settings: %dx%d, %d fps, %d kbps%s, HEVC %s, AV1 %s, HDR %s, audio %d ch, host audio %s, "
              "optimise game settings %s, quit app after %s, fullscreen %s, language %s\n",
           c->stream.width, c->stream.height, c->stream.fps, c->stream.bitrate,
           c->stream_bitrate_defaulted ? " (automatic)" : "", yes_no(c->hevc), yes_no(c->av1), yes_no(c->hdr),
           CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(c->stream.audioConfiguration), yes_no(c->localaudio),
           yes_no(c->sops), yes_no(c->quitappafter), yes_no(c->fullscreen), c->language ? c->language : "auto");
    sb_add(b, "Input settings: absolute mouse %s, virtual mouse %s, swap A/B X/Y %s, Nintendo layout %d, "
              "deadzone %d%%\n",
           yes_no(c->absmouse), yes_no(c->virtual_mouse), yes_no(c->swap_abxy), c->nintendo_layout,
           c->stick_deadzone);
    sb_add(b, "Co-op settings: orientation %d, audio mode %d, encoder %d, host tuning %s, bitrate override h/v %d/%d "
              "kbps (0 = half the Video bitrate); wired network mode %d; debug level %d\n",
           c->coop_orientation, c->coop_audio_mode, c->coop_encoder, yes_no(c->coop_host_tuning),
           c->coop_bitrate[COOP_ORIENTATION_HORIZONTAL], c->coop_bitrate[COOP_ORIENTATION_VERTICAL],
           c->wired_pin_mode, c->debug_level);
}

char *diag_report_build_start(app_t *app) {
    sbuf_t b = {0};
    section_identity(&b, "Eclipse diagnostic report, at start (before video, audio and input are up)");
    section_system(&b, app);
    section_storage(&b, app);
    section_network(&b, app);
    section_settings(&b, app);
    sb_add(&b, "=== end of start report (the full report follows once the app is up) ===\n");
    return b.s != NULL ? b.s : strdup("");
}

static void log_lines(char *text) {
    for (char *line = strtok(text, "\n"); line != NULL; line = strtok(NULL, "\n")) {
        commons_log_info("Report", "%s", line);
    }
    free(text);
}

static void start_hashing(void) {
    static bool hashing = false;
    if (!hashing) {
        hashing = true;
        SDL_Thread *t = SDL_CreateThread(hash_binary_thread, "diaghash", NULL);
        if (t != NULL) {
            SDL_DetachThread(t);
        }
    }
}

void diag_report_log_start(app_t *app) {
    log_lines(diag_report_build_start(app));
    start_hashing();
}

char *diag_report_build(app_t *app) {
    sbuf_t b = {0};
    section_identity(&b, "Eclipse diagnostic report");
    section_system(&b, app);
    section_storage(&b, app);
    section_displays(&b);
    section_media(&b, app);
    section_input(&b, app);
    section_network(&b, app);
    section_hosts(&b);
    section_settings(&b, app);
    sb_add(&b, "=== end of report ===\n");
    return b.s != NULL ? b.s : strdup("");
}

void diag_report_log(app_t *app) {
    log_lines(diag_report_build(app));
    start_hashing();
}

void diag_report_log_host(const pclist_t *node) {
    static struct {
        uuidstr_t id;
        uint32_t sig;
    } last[16];
    static int count = 0;
    char line[1024];
    host_line(line, sizeof(line), node);
    uint32_t sig = 2166136261u;   /* FNV-1a */
    for (const char *p = line; *p != '\0'; p++) {
        sig = (sig ^ (uint8_t) *p) * 16777619u;
    }
    int slot = -1;
    for (int i = 0; i < count; i++) {
        if (uuidstr_t_equals_t(&last[i].id, &node->id)) {
            slot = i;
            break;
        }
    }
    if (slot >= 0 && last[slot].sig == sig) {
        return;   /* unchanged since the last line for this computer */
    }
    if (slot < 0) {
        slot = count < 16 ? count++ : 15;
        last[slot].id = node->id;
    }
    last[slot].sig = sig;
    commons_log_info("Host", "%s", line);
}
