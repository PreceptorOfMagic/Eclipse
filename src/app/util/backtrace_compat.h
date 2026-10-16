#pragma once
/*
 * backtrace() for the crash records on Android.
 *
 * glibc's <execinfo.h> backtrace() exists in bionic only from Android 13 (API 33); the Android client runs on
 * Android 9 and later (the NVIDIA Shield is on Android 11). The unwinder every NDK binary already links does
 * the same walk. Frames are return addresses, resolved against /proc/self/maps or by dladdr() below.
 */
#if defined(__ANDROID__)

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unwind.h>

typedef struct eclipse_backtrace_state {
    void **frames;
    int max;
    int count;
} eclipse_backtrace_state;

static _Unwind_Reason_Code eclipse_backtrace_step(struct _Unwind_Context *context, void *arg) {
    eclipse_backtrace_state *state = (eclipse_backtrace_state *) arg;
    const uintptr_t pc = (uintptr_t) _Unwind_GetIP(context);
    if (pc != 0) {
        if (state->count >= state->max) {
            return _URC_END_OF_STACK;
        }
        state->frames[state->count++] = (void *) pc;
    }
    return _URC_NO_REASON;
}

static inline int eclipse_backtrace(void **frames, int max) {
    eclipse_backtrace_state state = {frames, max, 0};
    _Unwind_Backtrace(eclipse_backtrace_step, &state);
    return state.count;
}

/* glibc's backtrace_symbols() contract: one malloc'd block the caller frees, "file(symbol+0xoff) [pc]". */
static inline char **eclipse_backtrace_symbols(void *const *frames, int count) {
    enum { LINE = 192 };
    if (count <= 0) {
        return NULL;
    }
    char **lines = (char **) malloc((size_t) count * (sizeof(char *) + LINE));
    if (lines == NULL) {
        return NULL;
    }
    char *text = (char *) (lines + count);
    for (int i = 0; i < count; ++i) {
        Dl_info info;
        const uintptr_t pc = (uintptr_t) frames[i];
        const char *file = "?", *symbol = "";
        uintptr_t offset = pc;
        if (dladdr(frames[i], &info) != 0) {
            file = info.dli_fname != NULL ? info.dli_fname : "?";
            if (info.dli_sname != NULL && info.dli_saddr != NULL) {
                symbol = info.dli_sname;
                offset = pc - (uintptr_t) info.dli_saddr;
            } else if (info.dli_fbase != NULL) {
                offset = pc - (uintptr_t) info.dli_fbase;
            }
        }
        lines[i] = text + (size_t) i * LINE;
        snprintf(lines[i], LINE, "%s(%s+0x%lx) [%p]", file, symbol, (unsigned long) offset, frames[i]);
    }
    return lines;
}

#else

#include <execinfo.h>

#endif
