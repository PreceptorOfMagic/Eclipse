/*
 * test_coop_audio_dedupe.c -- the duplicate-suppression filter, offline.
 *
 *   ./test_coop_audio_dedupe                       synthetic cases with pass/fail
 *   ./test_coop_audio_dedupe SELF.wav PEER.wav OUT  real capture (coop-audio-self/peer.wav from /tmp/coop-audio-dump):
 *                                                   writes OUT-before.wav (plain 0.75/0.75 mix), OUT-after.wav, and
 *                                                   the processed OUT-self.wav / OUT-peer.wav (both 512 frames late)
 *
 * Synthetic signal per case (48 kHz stereo, 9 s): a shared "narration" (syllable-modulated harmonics + band noise)
 * present in both streams, the second copy offset by `delay`; each stream also has its OWN sounds (decaying noise
 * bursts, different seeds) and its own -40 dB noise floor standing in for the codec. Three 3 s sections:
 *   A shared only   -> the LATER copy must drop >= 12 dB, the earlier must pass (+-0.2 dB)
 *   B own only      -> both must pass (+-1 dB)
 *   C both          -> the later stream must keep its own sounds (energy within -2..+3 dB of them alone)
 * Plus a case with nothing shared at all: both streams must pass (+-0.5 dB).
 * Build: gcc -std=c99 -O2 -Isrc/app/stream/audio tests/coop/test_coop_audio_dedupe.c
 *        src/app/stream/audio/coop_audio_dedupe.c -lm -o /tmp/test_coop_audio_dedupe
 */
#include "coop_audio_dedupe.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE 48000
#define CH 2
#define SECS 9
#define TOTAL (RATE * SECS)
#define BLOCK 480
#define TWO_PI 6.28318530717958647692

static unsigned rng = 1;
static float frand(void) {   /* -1..1 */
    rng = rng * 1664525u + 1013904223u;
    return (float) ((rng >> 8) & 0xFFFFFF) / 8388608.0f - 1.0f;
}

static void make_narration(float *out, int n) {
    double ph1 = 0, ph2 = 0, ph3 = 0;
    float lp = 0;
    rng = 7;
    for (int i = 0; i < n; ++i) {
        double t = (double) i / RATE;
        double syl = 0.5 + 0.5 * sin(TWO_PI * 3.7 * t + 1.3 * sin(TWO_PI * 0.6 * t));   /* ~4 syllables/s */
        double env = syl * syl * (0.6 + 0.4 * sin(TWO_PI * 0.23 * t));
        double f0 = 140 + 30 * sin(TWO_PI * 0.9 * t);
        ph1 += TWO_PI * f0 / RATE;
        ph2 += TWO_PI * 2 * f0 / RATE;
        ph3 += TWO_PI * 3.1 * f0 / RATE;
        lp += 0.15f * (frand() - lp);
        out[i] = (float) (env * (0.5 * sin(ph1) + 0.3 * sin(ph2) + 0.2 * sin(ph3) + 0.6 * lp));
    }
}

static void make_own(float *out, int n, unsigned seed) {
    rng = seed;
    memset(out, 0, sizeof(float) * (size_t) n);
    int next = RATE / 5;
    while (next < n) {
        float amp = 0.6f + 0.4f * frand();
        int len = RATE / 4;
        float lp = 0, k = seed == 2 ? 0.6f : 0.25f;   /* different colour per stream */
        for (int i = 0; i < len && next + i < n; ++i) {
            lp += k * (frand() - lp);
            out[next + i] += amp * lp * expf(-(float) i / (RATE * 0.06f));
        }
        next += RATE / 3 + (int) ((frand() + 1) * RATE / 6);
    }
}

static int16_t to16(float v) {
    long r = lrintf(v * 9000.0f);
    return (int16_t) (r > 32767 ? 32767 : (r < -32768 ? -32768 : r));
}

static double energy(const int16_t *x, int from, int to) {
    double e = 0;
    for (int i = from * CH; i < to * CH; ++i) {
        e += (double) x[i] * x[i];
    }
    return e + 1e-9;
}

static double db(double ratio) { return 10.0 * log10(ratio); }

static void run(coop_dedupe_t *dd, int16_t *self, int16_t *peer, int total) {
    for (int off = 0; off < total; off += BLOCK) {
        int n = total - off < BLOCK ? total - off : BLOCK;
        coop_dedupe_process(dd, self + off * CH, peer + off * CH, n);
    }
}

/* returns failures; delay_ms > 0: peer later */
static int run_case(const char *name, int delay_ms, int shared) {
    const int n = TOTAL, sec = 3 * RATE, d = RATE / 1000 * delay_ms, lat = coop_dedupe_latency_frames();
    float *nar = calloc((size_t) n + 2u * RATE, sizeof(float)), *u1 = calloc((size_t) n, sizeof(float)),
          *u2 = calloc((size_t) n, sizeof(float));
    make_narration(nar, n + 2 * RATE);
    make_own(u1, n, 1);
    make_own(u2, n, 2);
    int16_t *self = calloc((size_t) n * CH, 2), *peer = calloc((size_t) n * CH, 2);
    int16_t *self_in = calloc((size_t) n * CH, 2), *peer_in = calloc((size_t) n * CH, 2);
    int16_t *u_self = calloc((size_t) n * CH, 2), *u_peer = calloc((size_t) n * CH, 2);
    rng = 99;
    for (int i = 0; i < n; ++i) {
        const int s = i / sec;                    /* 0 = A shared only, 1 = B own only, 2 = C both */
        const float sh = shared && s != 1 ? 1.0f : 0.0f, own = s != 0 ? 1.0f : 0.0f;
        const int is = RATE + i, ip = RATE + i - d;   /* narration index: the peer copy is d later */
        for (int c = 0; c < CH; ++c) {
            float sv = sh * nar[is] + own * u1[i] + 0.01f * frand();
            float pv = sh * nar[ip] + own * u2[i] + 0.01f * frand();
            self[i * CH + c] = self_in[i * CH + c] = to16(sv);
            peer[i * CH + c] = peer_in[i * CH + c] = to16(pv);
            u_self[i * CH + c] = to16(own * u1[i]);
            u_peer[i * CH + c] = to16(own * u2[i]);
        }
    }
    coop_dedupe_t *dd = coop_dedupe_create(RATE, CH, 400);
    run(dd, self, peer, n);
    coop_dedupe_stats_t st;
    coop_dedupe_get_stats(dd, &st);
    /* compare output[t + lat] with input[t]; measure the last 1.5 s of each section (after settling) */
    int fails = 0;
    const int16_t *later_out = delay_ms >= 0 ? peer : self, *later_in = delay_ms >= 0 ? peer_in : self_in;
    const int16_t *early_out = delay_ms >= 0 ? self : peer, *early_in = delay_ms >= 0 ? self_in : peer_in;
    const int16_t *later_own = delay_ms >= 0 ? u_peer : u_self;
    double r[3][3];
    for (int s = 0; s < 3; ++s) {
        int a = s * sec + sec / 2, b = (s + 1) * sec - lat;
        double eo = 0, ei = 0, eeo = 0, eei = 0;
        for (int i = a; i < b; ++i) {
            for (int c = 0; c < CH; ++c) {
                double x = later_out[(i + lat) * CH + c], y = later_in[i * CH + c];
                double xe = early_out[(i + lat) * CH + c], ye = early_in[i * CH + c];
                eo += x * x; ei += y * y; eeo += xe * xe; eei += ye * ye;
            }
        }
        r[s][0] = db((eo + 1e-9) / (ei + 1e-9));
        r[s][1] = db((eeo + 1e-9) / (eei + 1e-9));
        r[s][2] = db((eo + 1e-9) / (energy(later_own, a, b) + 1e-9));
    }
    printf("%-22s lag=%d rho=%.2f est=%u changes=%u | A later %+6.1f dB early %+5.2f | B later %+5.2f early %+5.2f | C later-vs-own %+5.2f early %+5.2f\n",
           name, st.lag_samples, st.rho, st.estimates, st.lag_changes, r[0][0], r[0][1], r[1][0], r[1][1], r[2][2], r[2][1]);
    if (shared) {
        if (r[0][0] > -12.0) { printf("  FAIL: duplicate only %.1f dB down (want <= -12)\n", r[0][0]); fails++; }
        if (fabs(r[0][1]) > 0.2) { printf("  FAIL: earlier stream changed %.2f dB in A\n", r[0][1]); fails++; }
        if (r[2][2] < -2.0 || r[2][2] > 3.0) { printf("  FAIL: later stream's own sounds %.2f dB vs alone\n", r[2][2]); fails++; }
        if (abs(st.lag_samples - d) > 8) { printf("  FAIL: lag %d, true %d\n", st.lag_samples, d); fails++; }
    } else {
        for (int s = 0; s < 3; ++s) {
            if (s == 0) continue;   /* A is silence+floor only without sharing */
            if (fabs(r[s][0]) > 0.5 || fabs(r[s][1]) > 0.5) { printf("  FAIL: unrelated audio changed (%.2f / %.2f dB)\n", r[s][0], r[s][1]); fails++; }
        }
    }
    if (fabs(r[1][0]) > 1.0 || fabs(r[1][1]) > 1.0) { printf("  FAIL: own-only section changed (%.2f / %.2f dB)\n", r[1][0], r[1][1]); fails++; }
    coop_dedupe_destroy(dd);
    free(nar); free(u1); free(u2); free(self); free(peer); free(self_in); free(peer_in); free(u_self); free(u_peer);
    return fails;
}

/* ------------------------------------------------------------- WAV mode */

static int16_t *read_wav(const char *path, int *frames, int *channels, int *rate) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;
    unsigned char h[44];
    if (fread(h, 1, 44, f) != 44) { fclose(f); return NULL; }
    *channels = h[22] | (h[23] << 8);
    *rate = (int) (h[24] | (h[25] << 8) | (h[26] << 16) | ((unsigned) h[27] << 24));
    fseek(f, 0, SEEK_END);
    long bytes = ftell(f) - 44;
    fseek(f, 44, SEEK_SET);
    *frames = (int) (bytes / (2 * *channels));
    int16_t *d = malloc((size_t) bytes);
    if (d == NULL || fread(d, 1, (size_t) bytes, f) != (size_t) bytes) { fclose(f); free(d); return NULL; }
    fclose(f);
    return d;
}

static void write_wav(const char *path, const int16_t *d, int frames, int ch, int rate) {
    FILE *f = fopen(path, "wb");
    if (f == NULL) return;
    unsigned bytes = (unsigned) frames * (unsigned) ch * 2u, riff = 36u + bytes, fl = 16u, br = (unsigned) rate * (unsigned) ch * 2u;
    unsigned short tag = 1, c = (unsigned short) ch, al = (unsigned short) (ch * 2), bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fl, 4, 1, f);
    fwrite(&tag, 2, 1, f); fwrite(&c, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&br, 4, 1, f);
    fwrite(&al, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
    fwrite(d, 2, (size_t) frames * (size_t) ch, f);
    fclose(f);
}

static int wav_mode(const char *sp, const char *pp, const char *out) {
    int fs, cs, rs, fp, cp, rp;
    int16_t *s = read_wav(sp, &fs, &cs, &rs), *p = read_wav(pp, &fp, &cp, &rp);
    if (s == NULL || p == NULL || cs != cp || rs != rp) { fprintf(stderr, "cannot read matching WAVs\n"); return 2; }
    int n = fs < fp ? fs : fp;
    int16_t *mix = malloc((size_t) n * (size_t) cs * 2u);
    for (int i = 0; i < n * cs; ++i) {
        int v = (int) lrintf(0.75f * s[i] + 0.75f * p[i]);
        mix[i] = (int16_t) (v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
    }
    char path[1024];
    snprintf(path, sizeof(path), "%s-before.wav", out);
    write_wav(path, mix, n, cs, rs);
    coop_dedupe_t *dd = coop_dedupe_create(rs, cs, 400);
    coop_dedupe_stats_t st;
    unsigned long long active = 0, blocks = 0;
    for (int off = 0; off < n; off += BLOCK) {
        int k = n - off < BLOCK ? n - off : BLOCK;
        coop_dedupe_process(dd, s + off * cs, p + off * cs, k);
        coop_dedupe_get_stats(dd, &st);
        blocks++;
        if (st.later) active++;
        if (blocks % 500 == 0) {
            printf("t=%6.1fs lag=%+6d (%+6.1f ms) rho=%.2f amount=%.2f suppressing=%s\n", (double) off / rs, st.lag_samples,
                   1000.0 * st.lag_samples / rs, st.rho, st.amount, st.later == 2 ? "peer" : st.later == 1 ? "self" : "-");
        }
    }
    snprintf(path, sizeof(path), "%s-self.wav", out);
    write_wav(path, s, n, cs, rs);
    snprintf(path, sizeof(path), "%s-peer.wav", out);
    write_wav(path, p, n, cs, rs);
    for (int i = 0; i < n * cs; ++i) {
        int v = (int) lrintf(0.75f * s[i] + 0.75f * p[i]);
        mix[i] = (int16_t) (v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
    }
    snprintf(path, sizeof(path), "%s-after.wav", out);
    write_wav(path, mix, n, cs, rs);
    printf("suppression active %.0f%% of the time; %u delay estimates, %u lag changes\n", 100.0 * active / (blocks ? blocks : 1),
           st.estimates, st.lag_changes);
    coop_dedupe_destroy(dd);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 4) {
        return wav_mode(argv[1], argv[2], argv[3]);
    }
    int fails = 0;
    fails += run_case("peer later +80 ms", 80, 1);
    fails += run_case("peer later +250 ms", 250, 1);
    fails += run_case("self later -120 ms", -120, 1);
    fails += run_case("nothing shared", 80, 0);
    printf(fails ? "FAIL (%d)\n" : "PASS\n", fails);
    return fails ? 1 : 0;
}
