/* Co-op audio duplicate suppression -- see coop_audio_dedupe.h for what and why. */
#include "coop_audio_dedupe.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define DD_PI 3.14159265358979323846
#define DD_N 512                 /* STFT size (samples) */
#define DD_H 256                 /* hop: 50% overlap, sqrt-Hann analysis + synthesis = perfect reconstruction */
#define DD_BINS (DD_N / 2 + 1)
#define DD_DEC 4                 /* the delay search runs on a 4x decimated mono copy */
#define DD_WIN_D 2048            /* delay-search window, decimated samples (~170 ms at 48 kHz) */
#define DD_SMOOTH 0.6f           /* per-hop smoothing of the cross/auto spectra (~6 frames) */
#define DD_RHO_ON 0.45f          /* rho at or above: suppress */
#define DD_RHO_OFF 0.25f         /* rho at or below: pass everything */
#define DD_COH_LO 0.35f          /* coherence below this is treated as chance (unrelated sounds) */
#define DD_COH_SPAN 0.45f
#define DD_FLOOR 0.05f           /* deepest cut: -26 dB, so a misjudged bin is never silenced outright */
#define DD_SWITCH_MARGIN 0.08f   /* a new delay must beat the current one by this much in rho */
#define DD_ATTACK 0.10f          /* per hop (5.3 ms): suppression engages in ~50 ms */
#define DD_RELEASE 0.01f         /* per hop: and lets go over ~0.5 s, so one gunshot does not drop it */

typedef struct {
    int n;
    float *cos_t, *sin_t;
    int *rev;
} dd_fft_t;

struct coop_dedupe {
    int rate, ch;
    int max_lag;                 /* samples */
    int rho_win;                 /* full-rate correlation window (samples) */
    int est_every;               /* samples between delay estimates */
    /* full-rate history: per-channel interleaved and mono, both streams */
    unsigned rlen, rmask;
    float *raw[2];               /* [stream] interleaved, rlen * ch */
    float *mono[2];              /* [stream] rlen */
    unsigned long long pos;      /* samples written so far */
    /* decimated mono history for the delay search */
    unsigned dlen, dmask;
    float *dec[2];
    unsigned long long dpos;
    float dec_acc[2];
    int dec_n;
    int ld;                      /* max lag, decimated */
    /* delay search FFT */
    dd_fft_t big;
    float *bx_re, *bx_im, *by_re, *by_im;
    unsigned long long next_est;
    /* STFT */
    dd_fft_t small;
    float win[DD_N];
    unsigned long long next_hop; /* absolute index of the end of the next frame to process */
    unsigned olen, omask;
    float *ola[2];               /* [stream] interleaved output accumulators, olen * ch */
    float s_xy_re[DD_BINS], s_xy_im[DD_BINS], s_xx[DD_BINS], s_yy[DD_BINS];
    bool spectra_valid;
    unsigned long long out_pos;  /* next output sample index handed back (input index - DD_N) */
    /* decision state */
    int lag;                     /* > 0 peer later, < 0 self later */
    float rho, rho_s;
    float target, amount;
    bool have_lag;
    coop_dedupe_stats_t stats;
};

/* ---------------------------------------------------------------- FFT (radix 2, in place) */

static bool fft_init(dd_fft_t *f, int n) {
    f->n = n;
    f->cos_t = malloc(sizeof(float) * (size_t) (n / 2));
    f->sin_t = malloc(sizeof(float) * (size_t) (n / 2));
    f->rev = malloc(sizeof(int) * (size_t) n);
    if (f->cos_t == NULL || f->sin_t == NULL || f->rev == NULL) {
        return false;
    }
    for (int i = 0; i < n / 2; ++i) {
        f->cos_t[i] = (float) cos(2.0 * DD_PI * i / n);
        f->sin_t[i] = (float) -sin(2.0 * DD_PI * i / n);
    }
    int bits = 0;
    while ((1 << bits) < n) {
        bits++;
    }
    for (int i = 0; i < n; ++i) {
        int r = 0;
        for (int b = 0; b < bits; ++b) {
            r |= ((i >> b) & 1) << (bits - 1 - b);
        }
        f->rev[i] = r;
    }
    return true;
}

static void fft_free(dd_fft_t *f) {
    free(f->cos_t);
    free(f->sin_t);
    free(f->rev);
}

/* inverse = conjugate twiddles; no 1/n scaling (callers scale) */
static void fft_run(const dd_fft_t *f, float *re, float *im, bool inverse) {
    const int n = f->n;
    for (int i = 0; i < n; ++i) {
        int j = f->rev[i];
        if (j > i) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const int half = len / 2, step = n / len;
        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < half; ++k) {
                float wr = f->cos_t[k * step], wi = inverse ? -f->sin_t[k * step] : f->sin_t[k * step];
                float xr = re[i + k + half], xi = im[i + k + half];
                float tr = xr * wr - xi * wi, ti = xr * wi + xi * wr;
                re[i + k + half] = re[i + k] - tr;
                im[i + k + half] = im[i + k] - ti;
                re[i + k] += tr;
                im[i + k] += ti;
            }
        }
    }
}

static unsigned pow2_at_least(unsigned v) {
    unsigned p = 1;
    while (p < v) {
        p <<= 1;
    }
    return p;
}

/* ---------------------------------------------------------------- lifecycle */

coop_dedupe_t *coop_dedupe_create(int rate, int channels, int max_lag_ms) {
    if (rate < 8000 || rate > 96000 || channels < 1 || channels > 2 || max_lag_ms < 5 || max_lag_ms > 1000) {
        return NULL;
    }
    coop_dedupe_t *dd = calloc(1, sizeof(*dd));
    if (dd == NULL) {
        return NULL;
    }
    dd->rate = rate;
    dd->ch = channels;
    dd->max_lag = rate / 1000 * max_lag_ms;
    dd->rho_win = rate / 10;
    dd->est_every = rate / 10;
    dd->ld = dd->max_lag / DD_DEC + 1;
    dd->rlen = pow2_at_least((unsigned) (dd->max_lag + dd->rho_win + 2 * DD_N + 4096));
    dd->rmask = dd->rlen - 1;
    dd->dlen = pow2_at_least((unsigned) (DD_WIN_D + dd->ld + 64));
    dd->dmask = dd->dlen - 1;
    dd->olen = 4096;
    dd->omask = dd->olen - 1;
    unsigned nf = pow2_at_least((unsigned) (DD_WIN_D + dd->ld));
    bool ok = fft_init(&dd->big, (int) nf) && fft_init(&dd->small, DD_N);
    for (int s = 0; s < 2 && ok; ++s) {
        dd->raw[s] = calloc((size_t) dd->rlen * (size_t) channels, sizeof(float));
        dd->mono[s] = calloc(dd->rlen, sizeof(float));
        dd->dec[s] = calloc(dd->dlen, sizeof(float));
        dd->ola[s] = calloc((size_t) dd->olen * (size_t) channels, sizeof(float));
        ok = dd->raw[s] && dd->mono[s] && dd->dec[s] && dd->ola[s];
    }
    dd->bx_re = calloc(nf, sizeof(float));
    dd->bx_im = calloc(nf, sizeof(float));
    dd->by_re = calloc(nf, sizeof(float));
    dd->by_im = calloc(nf, sizeof(float));
    if (!ok || !dd->bx_re || !dd->bx_im || !dd->by_re || !dd->by_im) {
        coop_dedupe_destroy(dd);
        return NULL;
    }
    for (int i = 0; i < DD_N; ++i) {
        dd->win[i] = (float) sqrt(0.5 - 0.5 * cos(2.0 * DD_PI * i / DD_N));   /* sqrt of periodic Hann */
    }
    dd->next_hop = DD_N;
    dd->next_est = (unsigned long long) (DD_WIN_D + dd->ld) * DD_DEC;
    return dd;
}

void coop_dedupe_destroy(coop_dedupe_t *dd) {
    if (dd == NULL) {
        return;
    }
    for (int s = 0; s < 2; ++s) {
        free(dd->raw[s]);
        free(dd->mono[s]);
        free(dd->dec[s]);
        free(dd->ola[s]);
    }
    free(dd->bx_re);
    free(dd->bx_im);
    free(dd->by_re);
    free(dd->by_im);
    fft_free(&dd->big);
    fft_free(&dd->small);
    free(dd);
}

int coop_dedupe_latency_frames(void) { return DD_N; }

void coop_dedupe_get_stats(const coop_dedupe_t *dd, coop_dedupe_stats_t *out) {
    *out = dd->stats;
    out->lag_samples = dd->lag;
    out->rho = dd->rho;
    out->amount = dd->amount;
    out->later = dd->amount < 0.01f ? 0 : (dd->lag >= 0 ? 2 : 1);
}

/* ---------------------------------------------------------------- delay estimation */

/* Normalised correlation of the later stream against the earlier one shifted by |lag|, over the last rho_win
 * samples. lag > 0: peer later. */
static float rho_at(const coop_dedupe_t *dd, int lag) {
    const float *later = dd->mono[lag >= 0 ? 1 : 0], *earlier = dd->mono[lag >= 0 ? 0 : 1];
    const unsigned long long a = (unsigned long long) (lag >= 0 ? lag : -lag);
    double sab = 0, saa = 0, sbb = 0;
    for (unsigned long long t = dd->pos - (unsigned long long) dd->rho_win; t < dd->pos; ++t) {
        float x = later[t & dd->rmask], y = earlier[(t - a) & dd->rmask];
        sab += (double) x * y;
        saa += (double) x * x;
        sbb += (double) y * y;
    }
    if (saa < 1e-3 || sbb < 1e-3) {
        return 0.0f;
    }
    double r = sab / sqrt(saa * sbb);
    return r > 0 ? (float) r : 0.0f;
}

/* One-sided GCC-PHAT on the decimated copies, using the NEWEST audio: how far behind `earlier` is `later`?
 * x = later's newest DD_WIN_D samples, y = earlier's history reaching ld further back. Returns the decimated delay
 * (0..ld) and its PHAT peak height; -1 when either side is silent. Searching each direction on the newest audio (rather
 * than one two-sided search, which must look ld into the past to cover both signs) is what lets the filter lock within
 * ~one window after shared sound starts -- measured: the two-sided version took ~0.75 s and missed narration onsets. */
static int phat_delay(coop_dedupe_t *dd, int later, float *peak) {
    const int ld = dd->ld, n = dd->big.n;
    const unsigned long long dnow = dd->dpos;
    memset(dd->bx_re, 0, sizeof(float) * (size_t) n);
    memset(dd->bx_im, 0, sizeof(float) * (size_t) n);
    memset(dd->by_re, 0, sizeof(float) * (size_t) n);
    memset(dd->by_im, 0, sizeof(float) * (size_t) n);
    const unsigned long long x0 = dnow - DD_WIN_D, y0 = dnow - DD_WIN_D - (unsigned long long) ld;
    double ex = 0, ey = 0;
    for (int i = 0; i < DD_WIN_D; ++i) {
        dd->bx_re[i] = dd->dec[later][(x0 + (unsigned) i) & dd->dmask];
        ex += (double) dd->bx_re[i] * dd->bx_re[i];
    }
    for (int i = 0; i < DD_WIN_D + ld; ++i) {
        dd->by_re[i] = dd->dec[1 - later][(y0 + (unsigned) i) & dd->dmask];
        ey += (double) dd->by_re[i] * dd->by_re[i];
    }
    if (ex < 1.0 || ey < 1.0) {
        return -1;
    }
    fft_run(&dd->big, dd->bx_re, dd->bx_im, false);
    fft_run(&dd->big, dd->by_re, dd->by_im, false);
    for (int k = 0; k < n; ++k) {   /* conj(X) * Y with the phase transform */
        float re = dd->bx_re[k] * dd->by_re[k] + dd->bx_im[k] * dd->by_im[k];
        float im = dd->bx_re[k] * dd->by_im[k] - dd->bx_im[k] * dd->by_re[k];
        float mag = sqrtf(re * re + im * im) + 1e-9f;
        dd->bx_re[k] = re / mag;
        dd->bx_im[k] = im / mag;
    }
    fft_run(&dd->big, dd->bx_re, dd->bx_im, true);
    int best_s = 0;
    float best = -1e30f;
    for (int s = 0; s <= ld; ++s) {   /* r[s] = sum x[i] y[i+s]; y[i+s] is earlier at (x0+i) - (ld-s) */
        if (dd->bx_re[s] > best) {
            best = dd->bx_re[s];
            best_s = s;
        }
    }
    *peak = best;
    return ld - best_s;
}

static int refine(const coop_dedupe_t *dd, int coarse_full, float *rho_out) {
    int best = coarse_full;
    float best_rho = -1.0f;
    for (int l = coarse_full - DD_DEC; l <= coarse_full + DD_DEC; ++l) {
        if (l > dd->max_lag || l < -dd->max_lag) {
            continue;
        }
        float r = rho_at(dd, l);
        if (r > best_rho) {
            best_rho = r;
            best = l;
        }
    }
    *rho_out = best_rho;
    return best;
}

static void estimate_lag(coop_dedupe_t *dd) {
    dd->stats.estimates++;
    float pk_peer = 0, pk_self = 0, rho_p = -1, rho_s = -1;
    int dp = phat_delay(dd, 1, &pk_peer);   /* peer later by dp */
    int ds = phat_delay(dd, 0, &pk_self);   /* self later by ds */
    int cand_p = 0, cand_s = 0;
    if (dp >= 0) {
        cand_p = refine(dd, dp * DD_DEC, &rho_p);
    }
    if (ds >= 0) {
        cand_s = refine(dd, -ds * DD_DEC, &rho_s);
    }
    int cand;
    float cand_rho;
    if (rho_p >= rho_s) {
        cand = cand_p;
        cand_rho = rho_p;
    } else {
        cand = cand_s;
        cand_rho = rho_s;
    }
    const float cur_rho = dd->have_lag ? rho_at(dd, dd->lag) : 0.0f;
    /* Only a CONVINCING new delay replaces the current one: while nothing is shared the search returns noise, and
     * keeping the last good delay means the filter is already right when the narration comes back. */
    if (cand_rho >= DD_RHO_ON && (!dd->have_lag || cand_rho > cur_rho + DD_SWITCH_MARGIN)) {
        if (dd->have_lag && cand != dd->lag) {
            dd->stats.lag_changes++;
            dd->spectra_valid = false;
        }
        dd->lag = cand;
        dd->rho = cand_rho;
        dd->have_lag = true;
    } else {
        dd->rho = cur_rho;
    }
    dd->rho_s = 0.5f * dd->rho_s + 0.5f * dd->rho;
    if (dd->rho_s >= DD_RHO_ON || dd->rho >= DD_RHO_ON) {
        dd->target = 1.0f;
    } else if (dd->rho_s <= DD_RHO_OFF) {
        dd->target = 0.0f;
    }
}

/* ---------------------------------------------------------------- STFT frame */

static void process_frame(coop_dedupe_t *dd, unsigned long long end) {
    const int ch = dd->ch;
    const unsigned long long start = end - DD_N;
    const int later = dd->lag >= 0 ? 1 : 0, earlier = 1 - later;
    const unsigned long long a = (unsigned long long) (dd->lag >= 0 ? dd->lag : -dd->lag);
    float gain[DD_BINS];
    for (int k = 0; k < DD_BINS; ++k) {
        gain[k] = 1.0f;
    }
    dd->amount += (dd->target - dd->amount) * (dd->target > dd->amount ? DD_ATTACK : DD_RELEASE);
    if (dd->have_lag && dd->amount > 0.01f && start >= a) {
        float xr[DD_N], xi[DD_N], yr[DD_N], yi[DD_N];
        for (int i = 0; i < DD_N; ++i) {
            xr[i] = dd->mono[later][(start + (unsigned) i) & dd->rmask] * dd->win[i];
            yr[i] = dd->mono[earlier][(start + (unsigned) i - a) & dd->rmask] * dd->win[i];
            xi[i] = yi[i] = 0.0f;
        }
        fft_run(&dd->small, xr, xi, false);
        fft_run(&dd->small, yr, yi, false);
        const float al = dd->spectra_valid ? DD_SMOOTH : 0.0f;
        float q[DD_BINS], coh[DD_BINS];
        for (int k = 0; k < DD_BINS; ++k) {
            float cre = xr[k] * yr[k] + xi[k] * yi[k], cim = xi[k] * yr[k] - xr[k] * yi[k];
            dd->s_xy_re[k] = al * dd->s_xy_re[k] + (1 - al) * cre;
            dd->s_xy_im[k] = al * dd->s_xy_im[k] + (1 - al) * cim;
            dd->s_xx[k] = al * dd->s_xx[k] + (1 - al) * (xr[k] * xr[k] + xi[k] * xi[k]);
            dd->s_yy[k] = al * dd->s_yy[k] + (1 - al) * (yr[k] * yr[k] + yi[k] * yi[k]);
            float num = dd->s_xy_re[k] * dd->s_xy_re[k] + dd->s_xy_im[k] * dd->s_xy_im[k];
            float den = dd->s_xx[k] * dd->s_yy[k];
            coh[k] = den > 1e-6f ? num / den : 0.0f;
            if (coh[k] > 1.0f) {
                coh[k] = 1.0f;
            }
            float v = (coh[k] - DD_COH_LO) / DD_COH_SPAN;
            q[k] = v < 0 ? 0 : (v > 1 ? 1 : v);
        }
        dd->spectra_valid = true;
        for (int k = 0; k < DD_BINS; ++k) {   /* light smoothing across frequency against isolated bins */
            float qs = q[k];
            if (k > 0 && k < DD_BINS - 1) {
                qs = 0.25f * q[k - 1] + 0.5f * q[k] + 0.25f * q[k + 1];
            }
            /* q already says how surely this bin is a copy (coherence 0.35 -> 0, 0.80 -> 1); a sure copy goes all the
             * way to the floor rather than by its coherent share -- measured 10-03: the share rule left real Opus-coded
             * Halo audio only 11.8 dB down, because coding noise holds coherence near 0.8 even for identical sound. */
            float g2 = 1.0f - dd->amount * qs;
            if (g2 < DD_FLOOR * DD_FLOOR) {
                g2 = DD_FLOOR * DD_FLOOR;
            }
            gain[k] = sqrtf(g2);
        }
    } else {
        dd->spectra_valid = false;
    }
    /* both streams, every channel: analysis -> gain (later stream only) -> synthesis -> overlap-add */
    for (int s = 0; s < 2; ++s) {
        const bool apply = s == later && dd->amount > 0.01f;
        for (int c = 0; c < ch; ++c) {
            float re[DD_N], im[DD_N];
            for (int i = 0; i < DD_N; ++i) {
                re[i] = dd->raw[s][((start + (unsigned) i) & dd->rmask) * (unsigned) ch + (unsigned) c] * dd->win[i];
                im[i] = 0.0f;
            }
            if (apply) {
                fft_run(&dd->small, re, im, false);
                for (int k = 0; k < DD_BINS; ++k) {
                    re[k] *= gain[k];
                    im[k] *= gain[k];
                    if (k > 0 && k < DD_N / 2) {
                        re[DD_N - k] = re[k];
                        im[DD_N - k] = -im[k];
                    }
                }
                fft_run(&dd->small, re, im, true);
                for (int i = 0; i < DD_N; ++i) {
                    re[i] /= (float) DD_N;
                }
            }
            for (int i = 0; i < DD_N; ++i) {
                dd->ola[s][((start + (unsigned) i) & dd->omask) * (unsigned) ch + (unsigned) c] += re[i] * dd->win[i];
            }
        }
    }
}

/* ---------------------------------------------------------------- per call */

void coop_dedupe_process(coop_dedupe_t *dd, int16_t *self, int16_t *peer, int frames) {
    const int ch = dd->ch;
    int16_t *io[2] = {self, peer};
    for (int i = 0; i < frames; ++i) {
        const unsigned long long t = dd->pos;
        for (int s = 0; s < 2; ++s) {
            float m = 0;
            for (int c = 0; c < ch; ++c) {
                float v = (float) io[s][i * ch + c];
                dd->raw[s][(t & dd->rmask) * (unsigned) ch + (unsigned) c] = v;
                m += v;
            }
            m /= (float) ch;
            dd->mono[s][t & dd->rmask] = m;
            dd->dec_acc[s] += m;
        }
        dd->pos++;
        if (++dd->dec_n == DD_DEC) {
            for (int s = 0; s < 2; ++s) {
                dd->dec[s][dd->dpos & dd->dmask] = dd->dec_acc[s] / DD_DEC;
                dd->dec_acc[s] = 0;
            }
            dd->dpos++;
            dd->dec_n = 0;
        }
        if (dd->pos >= dd->next_est) {
            estimate_lag(dd);
            dd->next_est += (unsigned long long) dd->est_every;
        }
        if (dd->pos >= dd->next_hop) {
            process_frame(dd, dd->next_hop);
            dd->next_hop += DD_H;
        }
    }
    /* hand back `frames` finished samples per stream, DD_N behind the input; clear what was handed back */
    for (int i = 0; i < frames; ++i) {
        const unsigned long long t = dd->out_pos;
        for (int s = 0; s < 2; ++s) {
            for (int c = 0; c < ch; ++c) {
                float v = 0.0f;
                if (t >= DD_N) {
                    float *cell = &dd->ola[s][(((t - DD_N) & dd->omask) * (unsigned) ch) + (unsigned) c];
                    v = *cell;
                    *cell = 0.0f;
                }
                long r = lrintf(v);
                io[s][i * ch + c] = (int16_t) (r > 32767 ? 32767 : (r < -32768 ? -32768 : r));
            }
        }
        dd->out_pos++;
    }
}
