/* The audio outputs offered in Settings -> Audio on Android (ui/settings/panes/audio.pane.c). The activity
 * names them (deploy/android/.../AudioOutputs.java) because Android only numbers them; this reads its list. */
#include "android_platform.h"

#include <stdlib.h>
#include <string.h>

int android_audio_outputs_parse(const char *text, char **names, int max) {
    int count = 0;
    for (const char *line = text; line != NULL && *line != '\0' && count < max;) {
        const char *end = strchr(line, '\n');
        const size_t len = end != NULL ? (size_t) (end - line) : strlen(line);
        const char *bar = memchr(line, '|', len);
        /* "id|name": a positive id, a non-empty name */
        if (bar != NULL && bar > line && (size_t) (bar - line) + 1 < len && strtol(line, NULL, 10) > 0) {
            const size_t name_len = len - (size_t) (bar - line) - 1;
            char *name = malloc(name_len + 1);
            if (name == NULL) {
                break;
            }
            memcpy(name, bar + 1, name_len);
            name[name_len] = '\0';
            names[count++] = name;
        }
        line = end != NULL ? end + 1 : NULL;
    }
    return count;
}
