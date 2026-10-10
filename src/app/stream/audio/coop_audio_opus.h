#pragma once

#include <stddef.h>
#include <opus_multistream.h>

/* Opus frame_size is output capacity for a packet, but the exact missing
 * duration for NULL-input packet-loss concealment. Keep those two meanings
 * separate: using a larger output buffer must not invent extra audio time.
 * Owner contract: opus-codec.org/docs/opus_api-1.5/group__opus__multistream.html */
static inline int coop_audio_decode_opus(OpusMSDecoder *decoder, const unsigned char *data,
        int bytes, opus_int16 *pcm, int capacity_frames, int packet_frames) {
    const int decode_frames = data == NULL ? packet_frames : capacity_frames;
    return opus_multistream_decode(decoder, data, bytes, pcm, decode_frames, 0);
}
