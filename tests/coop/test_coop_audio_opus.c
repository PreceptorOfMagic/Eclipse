/* Link against the real Opus library. Exercises the production decode helper,
 * including subsequent decoder state after consecutive missing packets. */
#include "coop_audio_opus.h"

#include <assert.h>
#include <stdbool.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void run_duration(int packet_frames) {
    unsigned char mapping[2] = {0, 1};
    int error;
    OpusMSEncoder *encoder = opus_multistream_encoder_create(48000, 2, 1, 1, mapping,
                                                            OPUS_APPLICATION_AUDIO, &error);
    assert(encoder && error == OPUS_OK);
    OpusMSDecoder *reference = opus_multistream_decoder_create(48000, 2, 1, 1, mapping, &error);
    assert(reference && error == OPUS_OK);
    OpusMSDecoder *fixed = opus_multistream_decoder_create(48000, 2, 1, 1, mapping, &error);
    assert(fixed && error == OPUS_OK);
    OpusMSDecoder *old = opus_multistream_decoder_create(48000, 2, 1, 1, mapping, &error);
    assert(old && error == OPUS_OK);
    unsigned char packet[4000];
    opus_int16 input[1920], expected[1920], actual[1920], previous[1920];
    int reference_total = 0, fixed_total = 0, old_total = 0, loss_calls = 0;
    for (int k = 0; k < 20; ++k) {
        for (int i = 0; i < packet_frames; ++i) {
            double phase = (k * packet_frames + i) / 48000.0;
            input[2*i] = (opus_int16) (12000 * sin(2 * 3.141592653589793 * 437 * phase));
            input[2*i+1] = (opus_int16) (7000 * sin(2 * 3.141592653589793 * 719 * phase));
        }
        int bytes = opus_multistream_encode(encoder, input, packet_frames, packet, sizeof(packet));
        assert(bytes > 0);
        bool lost = k == 4 || k == 5 || k == 12;
        const unsigned char *data = lost ? NULL : packet;
        if (lost) { bytes = 0; ++loss_calls; }
        int a = opus_multistream_decode(reference, data, bytes, expected, packet_frames, 0);
        int b = coop_audio_decode_opus(fixed, data, bytes, actual, 960, packet_frames);
        int c = opus_multistream_decode(old, data, bytes, previous, 960, 0);
        assert(a == packet_frames && b == a && c > 0);
        assert(memcmp(expected, actual, (size_t) a * 2 * sizeof(opus_int16)) == 0);
        reference_total += a; fixed_total += b; old_total += c;
    }
    assert(fixed_total == reference_total && fixed_total == 20 * packet_frames);
    assert(old_total - fixed_total == loss_calls * (960 - packet_frames));
    printf("packet=%d frames, losses=%d: expected=%d fixed=%d old=%d; extra old audio=%.1fms; fixed PCM matches reference\n",
           packet_frames, loss_calls, reference_total, fixed_total, old_total, (old_total-fixed_total)/48.0);
    opus_multistream_decoder_destroy(reference);
    opus_multistream_decoder_destroy(fixed);
    opus_multistream_decoder_destroy(old);
    opus_multistream_encoder_destroy(encoder);
}

static void normal_packet_capacity(void) {
    unsigned char mapping[2] = {0, 1}, packet[4000];
    int error;
    OpusMSEncoder *encoder = opus_multistream_encoder_create(48000, 2, 1, 1, mapping,
                                                            OPUS_APPLICATION_AUDIO, &error);
    assert(encoder && error == OPUS_OK);
    OpusMSDecoder *decoder = opus_multistream_decoder_create(48000, 2, 1, 1, mapping, &error);
    assert(decoder && error == OPUS_OK);
    opus_int16 pcm[1920] = {0};
    int bytes = opus_multistream_encode(encoder, pcm, 960, packet, sizeof(packet));
    assert(bytes > 0);
    /* A normal 20 ms packet still uses the full output capacity. */
    assert(coop_audio_decode_opus(decoder, packet, bytes, pcm, 960, 240) == 960);
    opus_multistream_decoder_destroy(decoder);
    opus_multistream_encoder_destroy(encoder);
}

int main(void) {
    puts(opus_get_version_string());
    for (int frames = 120; frames <= 960; frames *= 2) run_duration(frames);
    normal_packet_capacity();
    puts("secondary Opus duration: PASS");
    return 0;
}
