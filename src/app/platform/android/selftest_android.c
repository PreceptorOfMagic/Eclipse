/*
 * TEST BUILDS ONLY (cmake -DECLIPSE_ANDROID_SELFTEST=ON, scripts/android/build_apk.sh --selftest): the
 * emulator smoke tests in .github/workflows/build-android.yml. A release build never contains this file.
 *
 * Started by launch arguments (EclipseActivity passes the "selftest_*" Intent extras through):
 *
 *   --selftest-video <file>   play an Annex B H.264/H.265 file through SS4S, i.e. the MediaCodec module and
 *   --selftest-codec <c>      the video surface under the UI, exactly as a stream's pictures go (h264|hevc)
 *   --selftest-size <WxH>
 *   --selftest-peer           exec the co-op peer executable against a socket nothing listens on: it must
 *                             start, fail to reach the coordinator and exit 1 -- proving the package manager
 *                             extracted it executable and its libraries resolve beside it
 *
 * Results are "SELFTEST ..." lines in the log (logcat), then the app quits.
 */
#include "android_platform.h"

#include "app.h"
#include "logging.h"
#include "util/path.h"

#include "ss4s.h"

#include <SDL.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static struct {
    char video[PATH_MAX];
    SS4S_VideoCodec codec;
    int width, height;
    bool peer;
} selftest;

void android_selftest_parse(int argc, char *argv[]) {
    memset(&selftest, 0, sizeof(selftest));
    for (int i = 1; i < argc; ++i) {
        const char *value = i + 1 < argc ? argv[i + 1] : NULL;
        if (strcmp(argv[i], "--selftest-video") == 0 && value != NULL) {
            snprintf(selftest.video, sizeof(selftest.video), "%s", value);
            ++i;
        } else if (strcmp(argv[i], "--selftest-codec") == 0 && value != NULL) {
            selftest.codec = strcmp(value, "h264") == 0 ? SS4S_VIDEO_H264 : SS4S_VIDEO_H265;
            ++i;
        } else if (strcmp(argv[i], "--selftest-size") == 0 && value != NULL) {
            sscanf(value, "%dx%d", &selftest.width, &selftest.height);
            ++i;
        } else if (strcmp(argv[i], "--selftest-peer") == 0) {
            selftest.peer = true;
        }
    }
}

static void selftest_peer(void) {
    char exe[PATH_MAX];
    if (!path_self_executable(exe, sizeof(exe))) {
        commons_log_error("SELFTEST", "SELFTEST peer exe=UNRESOLVED result=FAIL");
        return;
    }
    const pid_t pid = fork();
    if (pid == 0) {
        sigset_t all;
        sigfillset(&all);
        sigprocmask(SIG_UNBLOCK, &all, NULL);
        execl(exe, exe, "--coop-secondary", "/proc/self/fd/-1/selftest.sock", (char *) NULL);
        _exit(127);
    }
    int status = 0;
    if (pid < 0 || waitpid(pid, &status, 0) != pid) {
        commons_log_error("SELFTEST", "SELFTEST peer exe=%s fork/wait failed: %s result=FAIL", exe, strerror(errno));
        return;
    }
    const bool exited = WIFEXITED(status);
    const int code = exited ? WEXITSTATUS(status) : WTERMSIG(status);
    /* 1 = it ran, and refused the unreachable coordinator (main.c); 127 = exec failed; a signal = crashed. */
    commons_log_info("SELFTEST", "SELFTEST peer exe=%s %s=%d result=%s", exe, exited ? "exit" : "signal", code,
                     exited && code == 1 ? "PASS" : "FAIL");
}

/* Access units: a new one starts at the first parameter set, SEI or AUD -- or the first slice of a picture --
 * after a picture's slices (Annex B, H.264 and H.265). */
static bool starts_access_unit(SS4S_VideoCodec codec, const unsigned char *nal, size_t len, bool *vcl) {
    if (codec == SS4S_VIDEO_H264) {
        const unsigned type = nal[0] & 0x1fu;
        *vcl = type == 1 || type == 5;
        if (*vcl) {
            return len > 1 && (nal[1] & 0x80u) != 0;   /* first_mb_in_slice == 0 */
        }
        return type >= 6 && type <= 9;
    }
    const unsigned type = (nal[0] >> 1) & 0x3fu;
    *vcl = type <= 31;
    if (*vcl) {
        return len > 2 && (nal[2] & 0x80u) != 0;       /* first_slice_segment_in_pic_flag */
    }
    return (type >= 32 && type <= 35) || type == 39;
}

static bool is_irap(SS4S_VideoCodec codec, const unsigned char *nal) {
    if (codec == SS4S_VIDEO_H264) {
        return (nal[0] & 0x1fu) == 5;
    }
    const unsigned type = (nal[0] >> 1) & 0x3fu;
    return type >= 16 && type <= 23;
}

static void selftest_video(void) {
    FILE *f = fopen(selftest.video, "rb");
    if (f == NULL) {
        commons_log_error("SELFTEST", "SELFTEST video file=%s unreadable: %s result=FAIL", selftest.video,
                          strerror(errno));
        return;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *data = size > 0 ? malloc((size_t) size) : NULL;
    const size_t got = data != NULL ? fread(data, 1, (size_t) size, f) : 0;
    fclose(f);
    if (got == 0) {
        free(data);
        commons_log_error("SELFTEST", "SELFTEST video file=%s empty result=FAIL", selftest.video);
        return;
    }
    SS4S_Player *player = SS4S_PlayerOpen();
    SS4S_VideoInfo info = {.codec = selftest.codec, .width = selftest.width, .height = selftest.height,
            .frameRateNumerator = 60, .frameRateDenominator = 1};
    const SS4S_VideoOpenResult opened = player != NULL ? SS4S_PlayerVideoOpen(player, &info) : SS4S_VIDEO_OPEN_ERROR;
    unsigned aus = 0, ok = 0, notready = 0, reqkey = 0, err = 0;
    if (opened == SS4S_VIDEO_OPEN_OK) {
        size_t au_start = 0, pos = 0;
        bool seen_vcl = false, irap = false;
        while (pos + 3 < got) {
            size_t sc = 0;
            if (data[pos] == 0 && data[pos + 1] == 0 && data[pos + 2] == 1) sc = 3;
            else if (pos + 4 < got && data[pos] == 0 && data[pos + 1] == 0 && data[pos + 2] == 0 && data[pos + 3] == 1) sc = 4;
            if (sc == 0) {
                ++pos;
                continue;
            }
            const unsigned char *nal = data + pos + sc;
            const size_t remaining = got - pos - sc;
            bool vcl = false;
            const bool boundary = remaining > 2 && starts_access_unit(selftest.codec, nal, remaining, &vcl);
            if (boundary && seen_vcl) {
                SS4S_VideoFeedFlags flags = SS4S_VIDEO_FEED_DATA_FRAME_START | SS4S_VIDEO_FEED_DATA_FRAME_END;
                if (irap) flags |= SS4S_VIDEO_FEED_DATA_KEYFRAME;
                const SS4S_VideoFeedResult r = SS4S_PlayerVideoFeed(player, data + au_start, pos - au_start, flags);
                aus++;
                ok += r == SS4S_VIDEO_FEED_OK;
                notready += r == SS4S_VIDEO_FEED_NOT_READY;
                reqkey += r == SS4S_VIDEO_FEED_REQUEST_KEYFRAME;
                err += r == SS4S_VIDEO_FEED_ERROR;
                SDL_Delay(16);   /* about 60 fps, as a stream would arrive */
                au_start = pos;
                seen_vcl = false;
                irap = false;
            }
            if (vcl) {
                seen_vcl = true;
                irap = irap || (remaining > 0 && is_irap(selftest.codec, nal));
            }
            pos += sc;
        }
        if (seen_vcl) {
            SS4S_VideoFeedFlags flags = SS4S_VIDEO_FEED_DATA_FRAME_START | SS4S_VIDEO_FEED_DATA_FRAME_END;
            if (irap) flags |= SS4S_VIDEO_FEED_DATA_KEYFRAME;
            const SS4S_VideoFeedResult r = SS4S_PlayerVideoFeed(player, data + au_start, got - au_start, flags);
            aus++;
            ok += r == SS4S_VIDEO_FEED_OK;
        }
        SDL_Delay(1000);   /* let the decoder drain before the module counts what it rendered */
        SS4S_PlayerVideoClose(player);
    }
    if (player != NULL) {
        SS4S_PlayerClose(player);
    }
    free(data);
    commons_log_info("SELFTEST", "SELFTEST video codec=%s size=%dx%d open=%d aus=%u ok=%u notready=%u reqkey=%u err=%u "
                                 "result=%s", SS4S_VideoCodecName(selftest.codec), selftest.width, selftest.height,
                     (int) opened, aus, ok, notready, reqkey, err,
                     opened == SS4S_VIDEO_OPEN_OK && aus > 0 && ok >= aus * 9 / 10 ? "PASS" : "FAIL");
}

static int selftest_thread(void *arg) {
    (void) arg;
    SDL_Delay(3000);   /* the UI and the video surface are up */
    if (selftest.peer) {
        selftest_peer();
    }
    if (selftest.video[0] != '\0') {
        selftest_video();
    }
    commons_log_info("SELFTEST", "SELFTEST done");
    SDL_Event quit = {.type = SDL_QUIT};
    SDL_PushEvent(&quit);
    return 0;
}

void android_selftest_start(void) {
    if (!selftest.peer && selftest.video[0] == '\0') {
        return;
    }
    SDL_Thread *thread = SDL_CreateThread(selftest_thread, "selftest", NULL);
    if (thread != NULL) {
        SDL_DetachThread(thread);
    }
}
