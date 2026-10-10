/*
 * eval_coop_audio_gate.c -- the multi-offset echo gate vs the single-offset dedupe filter, on a real capture.
 *
 *   ./eval_coop_audio_gate SELF.wav PEER.wav OUT
 *
 * Writes OUT-before.wav (plain 0.75/0.75 mix), OUT-dedupe.wav, OUT-gate.wav, and prints one row per 0.5 s:
 *   echo   = how much of the later stream still REPEATS the earlier one: best normalised correlation of 1 ms log-energy
 *            envelopes over +-500 ms (the measure that found the layers in the first place), raw / dedupe / gate;
 *   peer, self = energy change of each stream through each filter (dB);
 *   bank   = the gate's offsets and how hard each is applied.
 * Build: gcc -std=c99 -O2 -Isrc/app/stream/audio tests/coop/eval_coop_audio_gate.c
 *        src/app/stream/audio/coop_audio_gate.c src/app/stream/audio/coop_audio_dedupe.c -lm -o /tmp/eval_gate
 */
#include "coop_audio_dedupe.h"
#include "coop_audio_gate.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK 480
#define HOP_MS 1
#define WIN_MS 1000
#define STEP_MS 500
#define LAG_MS 500

static int16_t *read_wav(const char *path, int *frames, int *channels, int *rate) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    unsigned char h[44];
    if (fread(h, 1, 44, f) != 44) {
        fclose(f);
        return NULL;
    }
    *channels = h[22] | (h[23] << 8);
    *rate = h[24] | (h[25] << 8) | (h[26] << 16) | (h[27] << 24);
    fseek(f, 0, SEEK_END);
    long bytes = ftell(f) - 44;
    fseek(f, 44, SEEK_SET);
    *frames = (int) (bytes / (2 * *channels));
    int16_t *d = malloc((size_t) bytes);
    if (d == NULL || fread(d, 2, (size_t) *frames * (size_t) *channels, f) != (size_t) *frames * (size_t) *channels) {
        free(d);
        fclose(f);
        return NULL;
    }
    fclose(f);
    return d;
}

static void write_wav(const char *path, const int16_t *d, int frames, int ch, int rate) {
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return;
    }
    unsigned data = (unsigned) frames * (unsigned) ch * 2u, riff = 36u + data, fmt = 16u, br = (unsigned) rate * ch * 2u;
    unsigned short pcm = 1, c = (unsigned short) ch, al = (unsigned short) (ch * 2), bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmt, 4, 1, f);
    fwrite(&pcm, 2, 1, f); fwrite(&c, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&br, 4, 1, f);
    fwrite(&al, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    fwrite(d, 2, (size_t) frames * (size_t) ch, f);
    fclose(f);
}

static void mix(const int16_t *a, const int16_t *b, int16_t *out, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        long v = lrintf(0.75f * a[i] + 0.75f * b[i]);
        out[i] = (int16_t) (v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
    }
}

/* 1 ms log-energy envelope of x starting `skip` frames in */
static float *envelope(const int16_t *x, int frames, int ch, int rate, int skip, int *n_out) {
    const int hop = rate / 1000 * HOP_MS;
    const int n = (frames - skip) / hop;
    float *e = malloc(sizeof(float) * (size_t) (n > 0 ? n : 1));
    for (int i = 0; i < n; ++i) {
        double s = 0;
        for (int j = 0; j < hop; ++j) {
            const int16_t *p = x + (size_t) (skip + i * hop + j) * (size_t) ch;
            double m = 0;
            for (int c = 0; c < ch; ++c) {
                m += p[c];
            }
            m /= ch;
            s += m * m;
        }
        e[i] = (float) log10(s + 1.0);
    }
    *n_out = n;
    return e;
}

static double ncc(const float *a, const float *b, int n) {
    double ma = 0, mb = 0;
    for (int i = 0; i < n; ++i) {
        ma += a[i];
        mb += b[i];
    }
    ma /= n;
    mb /= n;
    double sab = 0, saa = 0, sbb = 0;
    for (int i = 0; i < n; ++i) {
        double x = a[i] - ma, y = b[i] - mb;
        sab += x * y;
        saa += x * x;
        sbb += y * y;
    }
    return saa > 0 && sbb > 0 ? sab / sqrt(saa * sbb) : 0;
}

/* best repeat of `later` against `earlier` over lags 1..LAG_MS (later is later by lag), window at w */
static double best_echo(const float *later, const float *earlier, int n, int w, int *lag_out) {
    double best = 0;
    *lag_out = 0;
    const int W = WIN_MS / HOP_MS;
    for (int l = 2; l <= LAG_MS / HOP_MS; l += 2) {
        if (w - l < 0 || w + W > n) {
            continue;
        }
        double r = ncc(later + w, earlier + w - l, W);
        if (r > best) {
            best = r;
            *lag_out = l;
        }
    }
    return best;
}


/* ---- echo ENERGY: full-rate cross-correlation of the later stream's 1 s window against the earlier stream reaching
 * LAG_MS further back. r^2 at a peak = the share of the later window's energy that is a copy of the earlier stream
 * at that offset; summed over the top 3 separated peaks (layers) and times the window energy = echo energy. */
#define XN 131072
static float xr_[XN], xi_[XN], yr_[XN], yi_[XN];
static double last_share;
static void fft(float *re, float *im, int n, int inv) {
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = 2 * 3.14159265358979323846 / len * (inv ? 1 : -1);
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
/* later window [a, a+W) of `lat` (frames offset by `lo`), earlier [a-D, a+W) of `ear`; mono */
static double echo_energy(const int16_t *lat, int lo, const int16_t *ear, int ch, int a, int W, int D) {
    memset(xr_, 0, sizeof xr_); memset(xi_, 0, sizeof xi_); memset(yr_, 0, sizeof yr_); memset(yi_, 0, sizeof yi_);
    double ex = 0;
    for (int i = 0; i < W; ++i) {
        float m = 0;
        for (int c = 0; c < ch; ++c) m += lat[(size_t) (a + lo + i) * ch + c];
        xr_[i] = m / ch; ex += (double) xr_[i] * xr_[i];
    }
    double *cum = malloc(sizeof(double) * (size_t) (W + D + 1));
    cum[0] = 0;
    for (int i = 0; i < W + D; ++i) {
        float m = 0;
        for (int c = 0; c < ch; ++c) m += ear[(size_t) (a - D + i) * ch + c];
        yr_[i] = m / ch; cum[i + 1] = cum[i] + (double) yr_[i] * yr_[i];
    }
    fft(xr_, xi_, XN, 0); fft(yr_, yi_, XN, 0);
    for (int k = 0; k < XN; ++k) {   /* conj(X) Y */
        float re = xr_[k] * yr_[k] + xi_[k] * yi_[k], im = xr_[k] * yi_[k] - xi_[k] * yr_[k];
        xr_[k] = re; xi_[k] = im;
    }
    fft(xr_, xi_, XN, 1);
    /* r(s) = sum x[i] y[i+s]; s = D - lag */
    double r2[3] = {0, 0, 0}; int at[3] = {-99999, -99999, -99999};
    const int sep = 48 * 3;
    for (int pk = 0; pk < 3; ++pk) {
        double best = 0; int bs = -1;
        for (int s = 0; s <= D - 48; ++s) {   /* lags >= 1 ms */
            int skip = 0;
            for (int q = 0; q < pk; ++q) if (abs(s - at[q]) < sep) skip = 1;
            if (skip) continue;
            double ey = cum[s + W] - cum[s];
            if (ey <= 0 || ex <= 0) continue;
            double v = xr_[s] / XN; v = v * v / (ex * ey);
            if (v > best) { best = v; bs = s; }
        }
        if (bs < 0) break;
        r2[pk] = best; at[pk] = bs;
    }
    free(cum);
    last_share = r2[0] + r2[1] + r2[2];
    return (r2[0] + r2[1] + r2[2]) * ex;
}

static double energy(const int16_t *x, int ch, int from, int to) {
    double e = 0;
    for (long i = (long) from * ch; i < (long) to * ch; ++i) {
        e += (double) x[i] * x[i];
    }
    return e + 1.0;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s SELF.wav PEER.wav OUT\n", argv[0]);
        return 2;
    }
    int fs, fp, cs, cp, rs, rp;
    int16_t *s0 = read_wav(argv[1], &fs, &cs, &rs), *p0 = read_wav(argv[2], &fp, &cp, &rp);
    if (s0 == NULL || p0 == NULL || cs != cp || rs != rp) {
        fprintf(stderr, "cannot read matching WAVs\n");
        return 2;
    }
    const int ch = cs, rate = rs, n = fs < fp ? fs : fp;
    const size_t bytes = (size_t) n * (size_t) ch * 2u;
    int16_t *sd = malloc(bytes), *pd = malloc(bytes), *sg = malloc(bytes), *pg = malloc(bytes), *mx = malloc(bytes);
    memcpy(sd, s0, bytes); memcpy(pd, p0, bytes); memcpy(sg, s0, bytes); memcpy(pg, p0, bytes);
    coop_dedupe_t *dd = coop_dedupe_create(rate, ch, 400);
    coop_gate_t *g = coop_gate_create(rate, ch, 1000);
    const int L = coop_gate_latency_frames();
    const int steps = n / (rate / 1000 * STEP_MS);
    char (*bank)[160] = calloc((size_t) steps + 2, 160);
    for (int off = 0; off < n; off += BLOCK) {
        const int b = n - off < BLOCK ? n - off : BLOCK;
        coop_dedupe_process(dd, sd + (size_t) off * ch, pd + (size_t) off * ch, b);
        coop_gate_process(g, sg + (size_t) off * ch, pg + (size_t) off * ch, b);
        const int step = (off + b) / (rate / 1000 * STEP_MS);
        if (step <= steps && bank[step][0] == 0 && (off + b) % (rate / 1000 * STEP_MS) < BLOCK) {
            coop_gate_stats_t st;
            coop_gate_get_stats(g, &st);
            int len = 0;
            for (int i = 0; i < st.count && len < 140; ++i) {
                len += snprintf(bank[step] + len, 160 - (size_t) len, "%+d:%.0f%% ",
                                st.lag_samples[i] * 1000 / rate, 100.0 * st.amount[i]);
            }
            if (len == 0) {
                snprintf(bank[step], 160, "-");
            }
        }
    }
    char path[1024];
    mix(s0, p0, mx, (size_t) n * ch);
    snprintf(path, sizeof(path), "%s-before.wav", argv[3]); write_wav(path, mx, n, ch, rate);
    mix(sd, pd, mx, (size_t) n * ch);
    snprintf(path, sizeof(path), "%s-dedupe.wav", argv[3]); write_wav(path, mx, n, ch, rate);
    mix(sg, pg, mx, (size_t) n * ch);
    snprintf(path, sizeof(path), "%s-gate.wav", argv[3]); write_wav(path, mx, n, ch, rate);
    snprintf(path, sizeof(path), "%s-gate-peer.wav", argv[3]); write_wav(path, pg, n, ch, rate);
    snprintf(path, sizeof(path), "%s-dedupe-peer.wav", argv[3]); write_wav(path, pd, n, ch, rate);
    /* envelopes, outputs advanced by L so time lines up with the input */
    int ne, n1;
    float *es = envelope(s0, n, ch, rate, 0, &ne), *ep = envelope(p0, n, ch, rate, 0, &n1);
    float *eds = envelope(sd, n, ch, rate, L, &n1), *edp = envelope(pd, n, ch, rate, L, &n1);
    float *egs = envelope(sg, n, ch, rate, L, &n1), *egp = envelope(pg, n, ch, rate, L, &n1);
    if (n1 < ne) {
        ne = n1;
    }
    printf("   t  | echo raw  dedupe gate | peer dB ded  gate | self dB ded  gate | gate bank (ms:amount)\n");
    double sum[3] = {0, 0, 0};
    int rows = 0;
    for (int w = LAG_MS / HOP_MS; w + WIN_MS / HOP_MS < ne; w += STEP_MS / HOP_MS) {
        int l0, l1, l2, m0, m1, m2;
        /* both directions: peer later (peer vs self) and self later (self vs peer) */
        double r0 = best_echo(ep, es, ne, w, &l0), r0b = best_echo(es, ep, ne, w, &m0);
        double r1 = best_echo(edp, es, ne, w, &l1), r1b = best_echo(eds, ep, ne, w, &m1);
        double r2 = best_echo(egp, es, ne, w, &l2), r2b = best_echo(egs, ep, ne, w, &m2);
        if (r0b > r0) { r0 = r0b; l0 = -m0; }
        if (r1b > r1) { r1 = r1b; }
        if (r2b > r2) { r2 = r2b; }
        const int a = w * (rate / 1000), b = a + WIN_MS * (rate / 1000);
        const double pe = energy(p0, ch, a, b), se = energy(s0, ch, a, b);
        const double pdd = 10 * log10(energy(pd, ch, a + L, b + L) / pe), pgg = 10 * log10(energy(pg, ch, a + L, b + L) / pe);
        const double sdd = 10 * log10(energy(sd, ch, a + L, b + L) / se), sgg = 10 * log10(energy(sg, ch, a + L, b + L) / se);
        const int D = LAG_MS * (rate / 1000), Wf = WIN_MS * (rate / 1000);
        const double x0 = echo_energy(p0, 0, s0, ch, a, Wf, D); const double sh0 = last_share;
        const double x1 = echo_energy(pd, L, s0, ch, a, Wf, D); const double sh1 = last_share;
        const double x2 = echo_energy(pg, L, s0, ch, a, Wf, D); const double sh2 = last_share;
        const int step = (w + WIN_MS / HOP_MS) / (STEP_MS / HOP_MS);
        printf("E %.1f %.6g %.6g %.6g %.4f %.4f %.4f\n", w / 1000.0, x0, x1, x2, sh0, sh1, sh2);
        printf("%6.1f | %.2f@%+4d %.2f %.2f | %6.1f %6.1f | %6.1f %6.1f | %s\n", w / 1000.0, r0, l0, r1, r2, pdd, pgg,
               sdd, sgg, step <= steps ? bank[step] : "");
        sum[0] += r0; sum[1] += r1; sum[2] += r2;
        rows++;
    }
    coop_gate_stats_t st;
    coop_gate_get_stats(g, &st);
    printf("mean echo: raw %.3f dedupe %.3f gate %.3f | gate searches %u admitted %u expired %u\n", sum[0] / rows,
           sum[1] / rows, sum[2] / rows, st.searches, st.admitted, st.expired);
    return 0;
}
