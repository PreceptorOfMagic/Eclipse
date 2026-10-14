/* The audio outputs list the activity hands the settings ("id|name" lines, AudioOutputs.java). */
#include "android_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

int main(void) {
    char *names[8];
    int n = android_audio_outputs_parse("3|HDMI: LG TV\n9|Bluetooth: WH-1000XM5\n21|USB: DAC/X 2\n", names, 8);
    CHECK(n == 3);
    CHECK(n == 3 && strcmp(names[0], "HDMI: LG TV") == 0 && strcmp(names[1], "Bluetooth: WH-1000XM5") == 0 &&
          strcmp(names[2], "USB: DAC/X 2") == 0);
    for (int i = 0; i < n; ++i) free(names[i]);

    /* Malformed lines are skipped: no separator, no id, id 0, empty name, blank line; the last line may lack
     * its newline; a name may contain ": " and digits. */
    n = android_audio_outputs_parse("nonsense\n|Nameless id\n0|Zero\n5|\n\n7|Aux: 2 ch\n12|Speaker", names, 8);
    CHECK(n == 2);
    CHECK(n == 2 && strcmp(names[0], "Aux: 2 ch") == 0 && strcmp(names[1], "Speaker") == 0);
    for (int i = 0; i < n; ++i) free(names[i]);

    /* At most max; an empty or missing list is none. */
    n = android_audio_outputs_parse("1|A\n2|B\n3|C\n", names, 2);
    CHECK(n == 2);
    for (int i = 0; i < n; ++i) free(names[i]);
    CHECK(android_audio_outputs_parse("", names, 8) == 0);
    CHECK(android_audio_outputs_parse(NULL, names, 8) == 0);

    if (failures == 0) printf("audio outputs parse: OK\n");
    return failures == 0 ? 0 : 1;
}
