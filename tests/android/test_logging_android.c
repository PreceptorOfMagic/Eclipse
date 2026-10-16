/* commons' Android logging backend: every level reaches logcat under one tag with the commons tag in the
 * message, and the same line goes to stderr for a kept diagnostic log. __android_log_print is faked. */
#include "logging.h"

#include <android/log.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int last_priority;
static char last_tag[64], last_msg[1100];
static int calls;

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    last_priority = prio;
    snprintf(last_tag, sizeof(last_tag), "%s", tag);
    vsnprintf(last_msg, sizeof(last_msg), fmt, ap);
    va_end(ap);
    return ++calls;
}

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stdout, "FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

int main(void) {
    char path[] = "/tmp/test_logging_android_XXXXXX";
    const int fd = mkstemp(path);
    CHECK(fd >= 0);
    CHECK(freopen(path, "w", stderr) != NULL);
    commons_logging_init("test");
    commons_log_info("Tag", "hello %d", 42);
    CHECK(last_priority == ANDROID_LOG_INFO && strcmp(last_tag, "Eclipse") == 0 && strcmp(last_msg, "[Tag] hello 42") == 0);
    commons_log_error("Err", "trailing newline\n");
    CHECK(last_priority == ANDROID_LOG_ERROR && strcmp(last_msg, "[Err] trailing newline") == 0);
    commons_log_warn("W", "w");
    CHECK(last_priority == ANDROID_LOG_WARN);
    commons_log_fatal("F", "f");
    CHECK(last_priority == ANDROID_LOG_FATAL);
    commons_log_debug("D", "d");
    CHECK(last_priority == ANDROID_LOG_DEBUG);
    const int before = calls;
    commons_log_verbose("V", "filtered as everywhere");
    CHECK(calls == before);
    char longmsg[2000];
    memset(longmsg, 'x', sizeof(longmsg) - 1);
    longmsg[sizeof(longmsg) - 1] = '\0';
    commons_log_info("Long", "%s", longmsg);
    CHECK(strlen(last_msg) < 1100);
    commons_logging_deinit();
    fflush(stderr);
    FILE *f = fopen(path, "r");
    char line[1200];
    int lines = 0, matched = 0;
    while (f != NULL && fgets(line, sizeof(line), f) != NULL) {
        lines++;
        matched += strstr(line, "[Tag] I hello 42\n") != NULL;
    }
    if (f != NULL) fclose(f);
    unlink(path);
    CHECK(lines == 6 && matched == 1);
    if (failures == 0) printf("logging_android: OK\n");
    return failures == 0 ? 0 : 1;
}
