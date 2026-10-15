/*
 * TEST BUILDS ONLY (cmake -DECLIPSE_ANDROID_SELFTEST=ON, scripts/android/build_apk.sh --selftest): the
 * emulator smoke tests in .github/workflows/build-android.yml. A release build never contains this file.
 *
 * Started by launch arguments (EclipseActivity passes the "selftest_*" Intent extras through):
 *
 *   --selftest-video <file>   play an Annex B H.264/H.265 file through SS4S, i.e. the MediaCodec module and
 *   --selftest-codec <c>      the video surface under the UI, exactly as a stream's pictures go (h264|hevc)
 *   --selftest-size <WxH>
 *   --selftest-peer           the co-op peer, in two steps:
 *                             1. exec it against a socket nothing listens on: it must start, fail to reach the
 *                                coordinator and exit 1 saying so (main.c) -- the package manager extracted it
 *                                executable and its libraries resolve beside it. The message is required:
 *                                the dynamic linker exits 1 too when a library is missing
 *                             2. start a real coordinator and spawn the peer exactly as a co-op stream does,
 *                                pointed at a host that is not paired: it must connect, start up (settings,
 *                                certificates, SS4S modules -- all without a Java VM) and report "host not
 *                                paired" over the co-op control channel
 *   --selftest-display        the display's refresh rates, the rate a 60 FPS stream asks its host for, and a
 *                             requested mode taking effect (the TV following a stream, as the webOS panel does)
 *   --selftest-keys           every input device and whether SDL takes it for a joystick (a TV remote must be a
 *                             keyboard to it); then the SDL events of keys device_smoke.sh sends through the
 *                             emulator's own keyboard-with-a-d-pad, as a remote's arrive
 *   --selftest-audio          the audio outputs the settings offer, by name; then the path a chosen output takes:
 *                             SDL on AAudio, the output opened by its current number, half a second of silence
 *
 * Results are "SELFTEST ..." lines in the log (logcat), then the app quits.
 */
#include "android_platform.h"

#include "app.h"
#include "logging.h"
#include "util/path.h"

#include "ss4s.h"
#include "stream/video/coop_coord.h"

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
    bool peer, audio, display, keys;
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
        } else if (strcmp(argv[i], "--selftest-audio") == 0) {
            selftest.audio = true;
        } else if (strcmp(argv[i], "--selftest-display") == 0) {
            selftest.display = true;
        } else if (strcmp(argv[i], "--selftest-keys") == 0) {
            selftest.keys = true;
        }
    }
}

static void selftest_peer(void) {
    char exe[PATH_MAX];
    if (!path_self_executable(exe, sizeof(exe))) {
        commons_log_error("SELFTEST", "SELFTEST peer exe=UNRESOLVED result=FAIL");
        return;
    }
    int err[2];
    if (pipe(err) != 0) {
        commons_log_error("SELFTEST", "SELFTEST peer exe=%s pipe failed: %s result=FAIL", exe, strerror(errno));
        return;
    }
    const pid_t pid = fork();
    if (pid == 0) {
        sigset_t all;
        sigfillset(&all);
        sigprocmask(SIG_UNBLOCK, &all, NULL);
        close(err[0]);
        dup2(err[1], STDERR_FILENO);
        close(err[1]);
        execl(exe, exe, "--coop-secondary", "/proc/self/fd/-1/selftest.sock", (char *) NULL);
        _exit(127);
    }
    close(err[1]);
    char said[512];
    size_t len = 0;
    for (ssize_t n; len < sizeof(said) - 1 && (n = read(err[0], said + len, sizeof(said) - 1 - len)) != 0;) {
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        len += (size_t) n;
    }
    said[len] = '\0';
    close(err[0]);
    for (char *c = said; *c != '\0'; c++) {
        if (*c == '\n' || *c == '"') {
            *c = ' ';
        }
    }
    int status = 0;
    if (pid < 0 || waitpid(pid, &status, 0) != pid) {
        commons_log_error("SELFTEST", "SELFTEST peer exe=%s fork/wait failed: %s result=FAIL", exe, strerror(errno));
        return;
    }
    const bool exited = WIFEXITED(status);
    const int code = exited ? WEXITSTATUS(status) : WTERMSIG(status);
    /* 1 and main.c's message = it ran, and refused the unreachable coordinator; 1 without it = the dynamic
     * linker could not load it (CANNOT LINK EXECUTABLE); 127 = exec failed; a signal = crashed. */
    const bool refused = strstr(said, "cannot reach coordinator") != NULL;
    commons_log_info("SELFTEST", "SELFTEST peer exe=%s %s=%d said=\"%s\" result=%s", exe, exited ? "exit" : "signal",
                     code, said, exited && code == 1 && refused ? "PASS" : "FAIL");
}

/* The co-op start-up handshake, through the same calls as session_video.c's co-op spawn. */
static void selftest_coop_handshake(void) {
    static const char unpaired_host[] = "00000000-0000-4000-8000-00000000e5c1";
    char exe[PATH_MAX];
    if (!path_self_executable(exe, sizeof(exe))) {
        commons_log_error("SELFTEST", "SELFTEST coop-handshake exe=UNRESOLVED result=FAIL");
        return;
    }
    if (!coop_coord_start(COOP_COORD_SOCKET)) {
        commons_log_error("SELFTEST", "SELFTEST coop-handshake coordinator did not start: %s result=FAIL",
                          strerror(errno));
        return;
    }
    const uint64_t started = SDL_GetTicks64();
    const int pid = coop_coord_spawn_secondary_for(exe, COOP_COORD_SOCKET, unpaired_host, 1, 1280, 720);
    const bool ready = pid > 0 && coop_coord_wait_secondary_ready(30000);
    const char *why = coop_coord_start_error_message();
    const unsigned elapsed = (unsigned) (SDL_GetTicks64() - started);
    coop_coord_request_peer_shutdown();
    coop_coord_reap_secondary();
    coop_coord_stop();
    /* "No longer paired" is the one answer that needs the peer to have connected, started up completely and
     * looked the host up; a peer that died or never connected reads "did not begin streaming in time". */
    const bool reported_host = !ready && strstr(why, "no longer paired") != NULL;
    commons_log_info("SELFTEST", "SELFTEST coop-handshake pid=%d after=%ums peer-said=\"%s\" result=%s",
                     pid, elapsed, ready ? "ready" : why,
                     pid > 0 && reported_host ? "PASS" : "FAIL");
}

/* InputDevice sources: SOURCE_DPAD, SOURCE_GAMEPAD, SOURCE_CLASS_JOYSTICK. */
#define SOURCE_DPAD 0x201u
#define SOURCE_GAMEPAD 0x401u
#define SOURCE_CLASS_JOYSTICK 0x10u

static int key_watch(void *userdata, SDL_Event *event) {
    (void) userdata;
    switch (event->type) {
        case SDL_KEYDOWN:
            if (!event->key.repeat) {
                commons_log_info("SELFTEST", "SELFTEST key down scancode=%s(%d)",
                                 SDL_GetScancodeName(event->key.keysym.scancode), (int) event->key.keysym.scancode);
            }
            break;
        case SDL_JOYBUTTONDOWN:
            commons_log_info("SELFTEST", "SELFTEST key joystick-button=%d", (int) event->jbutton.button);
            break;
        case SDL_CONTROLLERBUTTONDOWN:
            commons_log_info("SELFTEST", "SELFTEST key controller-button=%s",
                             SDL_GameControllerGetStringForButton((SDL_GameControllerButton) event->cbutton.button));
            break;
        default:
            break;
    }
    return 0;
}

static void selftest_keys(void) {
    char *devices = android_input_devices();
    int count = 0, remotes_as_joysticks = 0;
    for (char *line = devices, *next; line != NULL && *line != '\0'; line = next) {
        next = strchr(line, '\n');
        if (next != NULL) {
            *next++ = '\0';
        }
        char *sources_text = strchr(line, '|');
        char *joystick_text = sources_text != NULL ? strchr(sources_text + 1, '|') : NULL;
        if (joystick_text == NULL) {
            continue;
        }
        *sources_text++ = '\0';
        *joystick_text++ = '\0';
        const unsigned sources = (unsigned) strtoul(sources_text, NULL, 16);
        const bool joystick = strcmp(joystick_text, "1") == 0;
        const bool remote_like = (sources & SOURCE_DPAD) == SOURCE_DPAD && (sources & SOURCE_GAMEPAD) != SOURCE_GAMEPAD &&
                                 (sources & SOURCE_CLASS_JOYSTICK) == 0;
        remotes_as_joysticks += remote_like && joystick;
        count++;
        commons_log_info("SELFTEST", "SELFTEST input-device name=\"%s\" sources=0x%x sdl-joystick=%d%s", line, sources,
                         (int) joystick, remote_like ? " (a keyboard with a d-pad, as a TV remote)" : "");
    }
    free(devices);
    commons_log_info("SELFTEST", "SELFTEST input-devices count=%d remote-like-as-joystick=%d result=%s", count,
                     remotes_as_joysticks, count > 0 && remotes_as_joysticks == 0 ? "PASS" : "FAIL");
    SDL_AddEventWatch(key_watch, NULL);
    commons_log_info("SELFTEST", "SELFTEST keys ready");
    SDL_Delay(10000);   /* device_smoke.sh sends its keys now */
    SDL_DelEventWatch(key_watch, NULL);
    commons_log_info("SELFTEST", "SELFTEST keys end");
}

static void selftest_display(void) {
    int rates[32];
    const int count = android_display_refresh_rates(rates, 32);
    char list[256] = "";
    for (int i = 0; i < count; ++i) {
        const size_t used = strlen(list);
        snprintf(list + used, sizeof(list) - used, "%s%d", i > 0 ? "," : "", rates[i]);
    }
    const int asks = android_stream_refresh_x100(60, 0, rates, count);
    int before = 0, after = 0, width = 0, height = 0;
    android_display_mode(&width, &height, &before, NULL, NULL);
    /* As a stream does (session_worker.c): the mode for 60 FPS, then back to the system's choice. */
    android_display_request_refresh(asks > 0 ? asks : 6000);
    SDL_Delay(2000);   /* an HDMI mode change takes a moment */
    android_display_mode(NULL, NULL, &after, NULL, NULL);
    android_display_request_refresh(0);
    /* 60 FPS shows at the asked rate or an even multiple of it; any rate when the display has only one. */
    const int want = asks > 0 ? asks : 6000;
    const int n = want > 0 ? (after + want / 2) / want : 0;
    const bool even = n >= 1 && (after - n * want <= n && n * want - after <= n);
    commons_log_info("SELFTEST", "SELFTEST display %dx%d rates=%s stream60-asks=%d refresh-before=%d after=%d "
                                 "result=%s", width, height, list, asks, before, after,
                     count > 0 && (even || count == 1) ? "PASS" : "FAIL");
}

static void silence(void *userdata, Uint8 *stream, int len) {
    (void) userdata;
    memset(stream, 0, (size_t) len);
}

static void selftest_audio(void) {
    char *names[32];
    const int count = android_audio_output_names(names, 32);
    char list[1024] = "";
    for (int i = 0; i < count; ++i) {
        const size_t used = strlen(list);
        snprintf(list + used, sizeof(list) - used, "%s\"%s\"", i > 0 ? ", " : "", names[i]);
    }
    commons_log_info("SELFTEST", "SELFTEST audio-outputs count=%d %s", count, list);
    /* The first output by name, as a stream opens a chosen one: its current number, on SDL's AAudio driver
     * (third_party/ss4s/modules/sdl/sdl.c). SDL_AudioInit restarts SDL's audio on that driver. */
    const int id = count > 0 ? android_audio_output_id(names[0]) : 0;
    bool aaudio = false, opened = false;
    const char *error = "";
    if (id > 0) {
        aaudio = SDL_AudioInit("AAudio") == 0;
        if (aaudio) {
            char id_name[16];
            snprintf(id_name, sizeof(id_name), "%d", id);
            SDL_AudioSpec want = {.freq = 48000, .format = AUDIO_S16LSB, .channels = 2, .samples = 240,
                    .callback = silence}, have;
            const SDL_AudioDeviceID device = SDL_OpenAudioDevice(id_name, 0, &want, &have, 0);
            opened = device != 0;
            if (opened) {
                SDL_PauseAudioDevice(device, 0);
                SDL_Delay(500);
                SDL_CloseAudioDevice(device);
            }
        }
        if (!opened) {
            error = SDL_GetError();
        }
        SDL_AudioInit(NULL);   /* back to SDL's own choice */
    }
    commons_log_info("SELFTEST", "SELFTEST audio output=\"%s\" id=%d aaudio=%d opened=%d %s result=%s",
                     count > 0 ? names[0] : "", id, (int) aaudio, (int) opened, error,
                     opened ? "PASS" : "FAIL");
    for (int i = 0; i < count; ++i) {
        free(names[i]);
    }
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
        selftest_coop_handshake();
    }
    if (selftest.video[0] != '\0') {
        selftest_video();
    }
    if (selftest.keys) {
        selftest_keys();
    }
    if (selftest.display) {
        selftest_display();
    }
    if (selftest.audio) {
        selftest_audio();
    }
    commons_log_info("SELFTEST", "SELFTEST done");
    SDL_Event quit = {.type = SDL_QUIT};
    SDL_PushEvent(&quit);
    return 0;
}

void android_selftest_start(void) {
    if (!selftest.peer && !selftest.audio && !selftest.display && !selftest.keys && selftest.video[0] == '\0') {
        return;
    }
    SDL_Thread *thread = SDL_CreateThread(selftest_thread, "selftest", NULL);
    if (thread != NULL) {
        SDL_DetachThread(thread);
    }
}
