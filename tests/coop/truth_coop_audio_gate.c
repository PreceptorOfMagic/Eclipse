/*
 * truth_coop_audio_gate.c -- the echo gate against KNOWN echo, built from a real capture.
 *
 *   ./truth_coop_audio_gate SELF.wav PEER.wav
 *
 * WHY. On a real two-host capture nobody knows which part of the later stream is the copy, so every "how much echo is
 * left" number is an estimate with its own floor. This builds a peer stream whose parts are known:
 *   self  = the real self capture (game audio as the earlier host sent it);
 *   echo  = self's LOW band (< 400 Hz) delayed by a "stream" offset (60 ms, later 54 ms: drift) plus self's HIGH band
 *           delayed by a "cutscene" offset that switches every 6 s between 272 / 294 / 353 ms, sometimes absent --
 *           the layered, switching pattern measured on 2026-10-04; scaled +5 dB (the peer host was louder);
 *   own   = the real PEER capture from its late-gameplay stretches (86-100 s, 150-190 s), looped: sounds the earlier
 *           host never made;
 *   peer  = echo + own.
 * The gate's per-bin gains (coop_gate_set_gain_hook) are applied to echo and own SEPARATELY, so the printout is exact:
 *   echo dB = how far the echo was turned down; own dB = what the peer's own sounds lost (want ~0).
 * Build: gcc -std=c99 -O2 -Isrc/app/stream/audio tests/coop/truth_coop_audio_gate.c
 *        src/app/stream/audio/coop_audio_gate.c -lm -o /tmp/truth_gate
 */
#include "coop_audio_gate.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 512
#define H 256
#define SEG_S 6
#define BLOCK 480
#define PI 3.14159265358979323846

static int16_t *read_wav(const char *path, int *frames, int *channels, int *rate) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;
    unsigned char h[44];
    if (fread(h, 1, 44, f) != 44) { fclose(f); return NULL; }
    *channels = h[22] | (h[23] << 8);
    *rate = h[24] | (h[25] << 8) | (h[26] << 16) | (h[27] << 24);
    fseek(f, 0, SEEK_END);
    long bytes = ftell(f) - 44;
    fseek(f, 44, SEEK_SET);
    *frames = (int) (bytes / (2 * *channels));
    int16_t *d = malloc((size_t) bytes);
    if (d == NULL || fread(d, 2, (size_t) *frames * (size_t) *channels, f) != (size_t) *frames * (size_t) *channels) {
        free(d); fclose(f); return NULL;
    }
    fclose(f);
    return d;
}

/* ---- the schedule: per 6 s segment, low-band offset (ms, 0 = none) and high-band offset (ms, 0 = none) */
static const int SCHED[][2] = {
    {60, 272}, {60, 294}, {0, 0}, {60, 353}, {60, 272}, {0, 294}, {60, 0}, {60, 353},
};
#define NSCHED (int) (sizeof(SCHED) / sizeof(SCHED[0]))
static void sched_at(int seg, int *lo, int *hi) {
    *lo = SCHED[seg % NSCHED][0];
    *hi = SCHED[seg % NSCHED][1];
    if (*lo && seg >= 2 * NSCHED) *lo = 54;   /* third cycle on: the stream offset has drifted */
}

/* ---- applying the gate's gains to a known component, same STFT as the gate */
typedef struct {
    const float *in[2];   /* echo, own (mono) */
    float *out[2];
    int n;
    float win[N];
    float cr[N], ci[N];
} aux_t;

static void fft(float *re, float *im, int n, int inv) {
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = 2 * PI / len * (inv ? 1 : -1);
        float wr = (float) cos(ang), wi = (float) sin(ang);
        for (int i = 0; i < n; i += len) {
            float cr = 1, ci = 0;
            for (int k = 0; k < len / 2; ++k) {
                float ur = re[i + k], ui = im[i + k];
                float vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
                float vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
                re[i + k] = ur + vr; im[i + k] = ui + vi;
                re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
                float nr = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = nr;
            }
        }
    }
}

static void hook(void *ctx, unsigned long long start, const float *gs, const float *gp, int bins) {
    (void) gs;
    aux_t *a = ctx;
    if (start + N > (unsigned long long) a->n) return;
    for (int c = 0; c < 2; ++c) {
        for (int i = 0; i < N; ++i) { a->cr[i] = a->in[c][start + i] * a->win[i]; a->ci[i] = 0; }
        fft(a->cr, a->ci, N, 0);
        for (int k = 0; k < bins; ++k) {
            a->cr[k] *= gp[k]; a->ci[k] *= gp[k];
            if (k > 0 && k < N / 2) { a->cr[N - k] = a->cr[k]; a->ci[N - k] = -a->ci[k]; }
        }
        fft(a->cr, a->ci, N, 1);
        for (int i = 0; i < N; ++i) a->out[c][start + i] += a->cr[i] / N * a->win[i];
    }
}

static double en(const float *x, int a, int b) {
    double e = 0;
    for (int i = a; i < b; ++i) e += (double) x[i] * x[i];
    return e + 1e-3;
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s SELF.wav PEER.wav\n", argv[0]); return 2; }
    int fs, fp, ch, ch2, rate, rate2;
    int16_t *S = read_wav(argv[1], &fs, &ch, &rate), *P = read_wav(argv[2], &fp, &ch2, &rate2);
    if (!S || !P || ch != ch2 || rate != rate2) { fprintf(stderr, "bad input\n"); return 2; }
    const int n = (fs < fp ? fs : fp) - rate;   /* leave room for the delays */
    const int pre = rate;                        /* echo reads up to 1 s back */
    /* mono self, split into bands by two one-pole lowpasses at 400 Hz */
    float *sm = malloc(sizeof(float) * (size_t) fs), *lo = malloc(sizeof(float) * (size_t) fs), *hi = malloc(sizeof(float) * (size_t) fs);
    const float k = 1.0f - expf(-2.0f * (float) PI * 400.0f / rate);
    float z1 = 0, z2 = 0;
    for (int i = 0; i < fs; ++i) {
        float m = 0;
        for (int c = 0; c < ch; ++c) m += S[(size_t) i * ch + c];
        sm[i] = m / ch;
        z1 += k * (sm[i] - z1); z2 += k * (z1 - z2);
        lo[i] = z2; hi[i] = sm[i] - z2;
    }
    /* own: peer's late stretches, looped */
    const int o_ranges[2][2] = {{86, 100}, {150, 190}};
    int olen = 0;
    float *own_src = malloc(sizeof(float) * (size_t) (60 * rate));
    for (int r = 0; r < 2; ++r)
        for (int i = o_ranges[r][0] * rate; i < o_ranges[r][1] * rate && i < fp; ++i) {
            float m = 0;
            for (int c = 0; c < ch; ++c) m += P[(size_t) i * ch + c];
            own_src[olen++] = (getenv("TRUTH_OWN") ? (float) atof(getenv("TRUTH_OWN")) : 0.7f) * m / ch;
        }
    float *echo = calloc((size_t) n, sizeof(float)), *own = calloc((size_t) n, sizeof(float));
    const float G = 1.78f;   /* +5 dB */
    const int fade = rate / 200;
    for (int i = 0; i < n; ++i) {
        const int t = i + pre;   /* index into self */
        const int seg = i / (SEG_S * rate), pos = i % (SEG_S * rate);
        int l, h, pl, ph;
        sched_at(seg, &l, &h);
        sched_at(seg > 0 ? seg - 1 : 0, &pl, &ph);
        float wnew = pos < fade && seg > 0 ? (float) pos / fade : 1.0f;   /* 5 ms crossfade at a switch */
        float vn = (l ? lo[t - l * rate / 1000] : 0) + (h ? hi[t - h * rate / 1000] : 0);
        float vo = (pl ? lo[t - pl * rate / 1000] : 0) + (ph ? hi[t - ph * rate / 1000] : 0);
        echo[i] = G * (wnew * vn + (1 - wnew) * vo);
        own[i] = own_src[i % olen];
        if (getenv("TRUTH_SIMPLE") != NULL) {   /* sanity case: one whole-signal copy at 272 ms, no own sound */
            echo[i] = G * sm[t - 272 * rate / 1000];
            own[i] = 0.001f * (float) ((i * 7919) % 200 - 100);
        }
    }
    /* streams the gate sees: self from `pre` on, peer = echo + own (mono duplicated to the channel count) */
    int16_t *gs = malloc(sizeof(int16_t) * (size_t) n * ch), *gp = malloc(sizeof(int16_t) * (size_t) n * ch);
    for (int i = 0; i < n; ++i)
        for (int c = 0; c < ch; ++c) {
            gs[(size_t) i * ch + c] = S[(size_t) (i + pre) * ch + c];
            float v = echo[i] + own[i];
            long r = lrintf(v);
            gp[(size_t) i * ch + c] = (int16_t) (r > 32767 ? 32767 : (r < -32768 ? -32768 : r));
        }
    aux_t a;
    a.in[0] = echo; a.in[1] = own; a.n = n;
    a.out[0] = calloc((size_t) n, sizeof(float)); a.out[1] = calloc((size_t) n, sizeof(float));
    for (int i = 0; i < N; ++i) a.win[i] = (float) sqrt(0.5 - 0.5 * cos(2.0 * PI * i / N));
    coop_gate_t *g = coop_gate_create(rate, ch, 1000);
    coop_gate_set_gain_hook(g, hook, &a);
    for (int off = 0; off < n; off += BLOCK) {
        int b = n - off < BLOCK ? n - off : BLOCK;
        coop_gate_process(g, gs + (size_t) off * ch, gp + (size_t) off * ch, b);
    }
    /* ORACLE: the ideal binary mask from the TRUE components (cut a bin when the echo outweighs own sound in it), same
     * STFT -- the most any gain-based method could do at this resolution. */
    float *oe = calloc((size_t) n, sizeof(float)), *oo = calloc((size_t) n, sizeof(float));
    {
        static float er[N], ei[N], orr[N], oi[N];
        for (int st = 0; st + N <= n; st += H) {
            for (int i = 0; i < N; ++i) {
                er[i] = echo[st + i] * a.win[i]; ei[i] = 0; orr[i] = own[st + i] * a.win[i]; oi[i] = 0;
            }
            fft(er, ei, N, 0); fft(orr, oi, N, 0);
            for (int k2 = 0; k2 <= N / 2; ++k2) {
                float pe = er[k2] * er[k2] + ei[k2] * ei[k2], po = orr[k2] * orr[k2] + oi[k2] * oi[k2];
                float gk = pe > po ? 0.01f : 1.0f;
                er[k2] *= gk; ei[k2] *= gk; orr[k2] *= gk; oi[k2] *= gk;
                if (k2 > 0 && k2 < N / 2) { er[N - k2] = er[k2]; ei[N - k2] = -ei[k2]; orr[N - k2] = orr[k2]; oi[N - k2] = -oi[k2]; }
            }
            fft(er, ei, N, 1); fft(orr, oi, N, 1);
            for (int i = 0; i < N; ++i) { oe[st + i] += er[i] / N * a.win[i]; oo[st + i] += orr[i] / N * a.win[i]; }
        }
    }
    /* per segment: steady state (from 1 s in) and the onset leak (time until echo is 20 dB down, 10 ms steps) */
    printf("seg  t0   low  high | echo dB steady | own dB | onset: ms until echo -20 dB / -30 dB\n");
    double se = 0, so = 0, n_e = 0, n_o = 0;
    const int nseg = n / (SEG_S * rate);
    for (int s = 0; s < nseg; ++s) {
        int l, h;
        sched_at(s, &l, &h);
        const int a0 = s * SEG_S * rate, a1 = a0 + rate, b = a0 + SEG_S * rate;
        const double e_db = 10 * log10(en(a.out[0], a1, b) / en(echo, a1, b));
        const double o_db = 10 * log10(en(a.out[1], a1, b) / en(own, a1, b));
        const double oe_db = 10 * log10(en(oe, a1, b) / en(echo, a1, b)), oo_db = 10 * log10(en(oo, a1, b) / en(own, a1, b));
        const double eo_ratio = 10 * log10(en(echo, a1, b) / en(own, a1, b));
        int t20 = -1, t30 = -1;
        for (int t = a0; t + rate / 100 <= b && (t20 < 0 || t30 < 0); t += rate / 100) {
            const double d = 10 * log10(en(a.out[0], t, t + rate / 50 < b ? t + rate / 50 : b) / en(echo, t, t + rate / 50 < b ? t + rate / 50 : b));
            if (t20 < 0 && d <= -20) t20 = (t - a0) * 1000 / rate;
            if (t30 < 0 && d <= -30) t30 = (t - a0) * 1000 / rate;
        }
        const int has_echo = l || h;
        if (has_echo) {
            printf("%3d %5d %4d %4d |   %7.1f      | %6.2f | %5d / %5d | oracle %6.1f %6.2f | echo/own %+5.1f dB\n", s, s * SEG_S, l, h, e_db, o_db, t20, t30, oe_db, oo_db, eo_ratio);
            se += e_db; n_e++;
        } else {
            printf("%3d %5d %4d %4d |   (no echo)    | %6.2f |\n", s, s * SEG_S, l, h, o_db);
        }
        so += o_db; n_o++;
    }
    printf("MEAN echo %.1f dB (segments with echo), own %.2f dB\n", se / (n_e ? n_e : 1), so / (n_o ? n_o : 1));
    return 0;
}
