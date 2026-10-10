/*
 * sim_coop_audio_pgate.c -- the passive gate (coop_audio_pgate.c) against real captures and a ground-truth simulation.
 *
 *   ./sim real SELF.wav PEER.wav SECTIONS.txt OUT
 *       per section ("start end label" lines): each stream's change from ECHO REMOVAL ONLY (the gate's own per-band,
 *       per-channel gains applied to the input -- the level balance is reported separately, not mixed in), overall
 *       and per band; gate flips/s. Writes OUT-before.wav / OUT-after.wav (0.75/0.75 mixes; "after" includes the
 *       balance, as the mixer would hear it).
 *   ./sim truth VOICE.wav BG_A.wav,BG_B.wav A_FROM B_FROM SECONDS OUT
 *       a co-op "cutscene": each player's background is that PC's own capture (A from A_FROM s, B from B_FROM s),
 *       narrator "sentences" from VOICE.wav are placed in BOTH streams at an offset that changes per sentence (the
 *       leader flips). The gate's gains are applied to every component separately, so the numbers are exact.
 *       TRUTH_PEER_DB=x puts the second PC's copy x dB louder (a host volume difference).
 *   ./sim truth-unseen VOICE.wav BG_A.wav,BG_B.wav A_FROM B_FROM SECONDS OUT
 *       each scheduled offset occurs once; default class whisper; never substitutes voiced training.
 *   ./sim real-trace SELF.wav PEER.wav SECTIONS.txt OUT TRACE.csv FROM TO
 *       same processing as real, with final hook decisions and bank snapshots in [FROM, TO).
 */
#include "coop_audio_pgate.h"

#ifndef COOP_PGATE_ENV_TUNING
#error "build with -DCOOP_PGATE_ENV_TUNING (and the same flag on coop_audio_pgate.c), or the PGATE_* sweeps are silently ignored"
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE 48000
#define BLOCK_IO 480
#define NB COOP_PGATE_BANDS

static const char *BAND_NAME[NB] = {"<250", "250-600", "600-1.5k", "1.5-3.5k", "3.5-8k", ">8k"};

static int16_t *read_wav(const char *path, long *frames, int *ch) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    unsigned char h[44];
    if (fread(h, 1, 44, f) != 44) { fclose(f); return NULL; }
    *ch = h[22] | (h[23] << 8);
    fseek(f, 0, SEEK_END);
    long bytes = ftell(f) - 44;
    fseek(f, 44, SEEK_SET);
    *frames = bytes / (2 * *ch);
    int16_t *d = malloc((size_t) bytes);
    if (!d || fread(d, 2, (size_t) (*frames * *ch), f) != (size_t) (*frames * *ch)) { free(d); fclose(f); return NULL; }
    fclose(f);
    return d;
}

static void write_wav(const char *path, const int16_t *d, long frames, int ch) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    unsigned data = (unsigned) frames * (unsigned) ch * 2u, riff = 36u + data, fmt = 16u, rate = RATE, br = RATE * ch * 2u;
    unsigned short pcm = 1, c = (unsigned short) ch, al = (unsigned short) (ch * 2), bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmt, 4, 1, f);
    fwrite(&pcm, 2, 1, f); fwrite(&c, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&br, 4, 1, f);
    fwrite(&al, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    fwrite(d, 2, (size_t) (frames * ch), f);
    fclose(f);
}

static int16_t clip16(float v) { long r = lrintf(v); return (int16_t) (r > 32767 ? 32767 : (r < -32768 ? -32768 : r)); }

/* ---- components pushed through the gate's own gains (from the hook) */
#define MAXC 8
typedef struct {
    long n;
    int ncomp;
    int stream[MAXC], chan[MAXC];
    float *bands[MAXC][NB];   /* input component, split */
    float *out[MAXC];         /* after the gate's gains */
    long nblocks, cap;
    unsigned long long *bstart;
    unsigned char (*closed)[2][2];   /* per block, stream, channel: bitmask of closed bands */
    unsigned char (*why)[2][2][NB];
    FILE *trace;
    coop_pgate_t *gate;
    unsigned long long trace_from, trace_to;
} rec_t;

static void rec_add(rec_t *r, int stream, int chan, const float *x) {
    const int k = r->ncomp++;
    r->stream[k] = stream; r->chan[k] = chan;
    coop_pgate_xover_t xo; coop_pgate_xover_init(&xo, RATE);
    for (int b = 0; b < NB; ++b) r->bands[k][b] = malloc(sizeof(float) * (size_t) r->n);
    r->out[k] = calloc((size_t) r->n, sizeof(float));
    float bb[NB];
    for (long i = 0; i < r->n; ++i) { coop_pgate_xover_run(&xo, x[i], bb); for (int b = 0; b < NB; ++b) r->bands[k][b][i] = bb[b]; }
}

static void hook(void *ctx, unsigned long long start, int frames, const float g0[2][2][NB], const float g1[2][2][NB],
                 const unsigned char why[2][2][NB]) {
    rec_t *r = ctx;
    if (r->nblocks == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 65536;
        r->bstart = realloc(r->bstart, sizeof(*r->bstart) * (size_t) r->cap);
        r->closed = realloc(r->closed, sizeof(*r->closed) * (size_t) r->cap);
        r->why = realloc(r->why, sizeof(*r->why) * (size_t) r->cap);
    }
    r->bstart[r->nblocks] = start;
    memcpy(r->why[r->nblocks], why, sizeof(*r->why));
    for (int s = 0; s < 2; ++s) for (int c = 0; c < 2; ++c) {
        unsigned char m = 0;
        for (int b = 0; b < NB; ++b) if (g1[s][c][b] < 0.5f) m |= (unsigned char) (1u << b);
        r->closed[r->nblocks][s][c] = m;
    }
    r->nblocks++;
    if (r->trace && start >= r->trace_from && start < r->trace_to) {
        coop_pgate_stats_t st;
        coop_pgate_get_stats(r->gate, &st);
        fprintf(r->trace, "%llu,%d,", start, frames);
        for (int i = 0; i < st.offsets; ++i) fprintf(r->trace, "%s%d", i ? ";" : "", st.offset_samples[i]);
        fprintf(r->trace, ",%.4f,%d,%u,%u,%u,%u", st.balance_db, st.balance_applied,
                st.admitted, st.recentres, st.pred_confirmed, st.pred_failed);
        for (int s = 0; s < 2; ++s) for (int c = 0; c < 2; ++c) for (int b = 0; b < NB; ++b)
            fprintf(r->trace, ",%.6f,%.6f,%u", g0[s][c][b], g1[s][c][b], (unsigned) why[s][c][b]);
        fputc('\n', r->trace);
    }
    for (int k = 0; k < r->ncomp; ++k) {
        const int s = r->stream[k], c = r->chan[k];
        for (int i = 0; i < frames; ++i) {
            const long t = (long) start + i;
            if (t >= r->n) break;
            const float f = (float) (i + 1) / (float) frames;
            float v = 0;
            for (int b = 0; b < NB; ++b) v += (g0[s][c][b] + (g1[s][c][b] - g0[s][c][b]) * f) * r->bands[k][b][t];
            r->out[k][t] = v;
        }
    }
}

static void run_gate(coop_pgate_t *g, int16_t *s, int16_t *p, long n, int ch) {
    for (long off = 0; off < n; off += BLOCK_IO) {
        int b = (int) (n - off < BLOCK_IO ? n - off : BLOCK_IO);
        coop_pgate_process(g, s + off * ch, p + off * ch, b);
    }
}

/* energy of x (or of band `band` of it, re-split) over [a, b) */
static double en(const float *x, long n, long a, long b, int band) {
    coop_pgate_xover_t xo; coop_pgate_xover_init(&xo, RATE);
    double e = 0; float bb[NB];
    for (long t = (a > 4800 ? a - 4800 : 0); t < b && t < n; ++t) {
        float v = x[t];
        if (band >= 0) { coop_pgate_xover_run(&xo, v, bb); v = bb[band]; }
        if (t >= a) e += (double) v * v;
    }
    return e + 1.0;
}

static void print_stats(coop_pgate_t *g) {
    coop_pgate_stats_t st; coop_pgate_get_stats(g, &st);
    printf("  learned: balance PC2 vs VEGA %+.2f dB (%s, %u clean matches); band ceilings", st.balance_db,
           st.balance_applied ? "applied" : "not applied", st.balance_samples);
    for (int b = 0; b < NB; ++b) printf(" %s:%.2f", BAND_NAME[b], st.ceiling[b]);
    printf("\n  bank:"); for (int i = 0; i < st.offsets; ++i) printf(" %+.1f", st.offset_samples[i] * 1000.0 / RATE);
    printf(" ms | admitted %u (twin splits %u) evicted %u | band conflicts: kept by holder %u, switched %u | fast tracks %u drift steps %u | env lines %u env closes %u\n", st.admitted, st.twin_splits, st.evicted, st.sticky_holds, st.side_switches,
           st.fast_tracks, st.drift_steps, st.env_lines, st.env_forced);
}

/* ================================================================== real */
static int mode_real(int argc, char **argv) {
    const bool trace = strcmp(argv[1], "real-trace") == 0;
    if (argc != (trace ? 9 : 6)) return 2;
    long ns, np; int cs, cp;
    int16_t *S = read_wav(argv[2], &ns, &cs), *P = read_wav(argv[3], &np, &cp);
    if (!S || !P || cs != 2 || cp != 2) { fprintf(stderr, "need two stereo WAVs\n"); return 2; }
    const long n = ns < np ? ns : np;
    rec_t r; memset(&r, 0, sizeof(r)); r.n = n;
    float *in[2][2];
    for (int s = 0; s < 2; ++s) for (int c = 0; c < 2; ++c) {
        in[s][c] = malloc(sizeof(float) * n);
        for (long i = 0; i < n; ++i) in[s][c][i] = (s ? P : S)[2 * i + c];
        rec_add(&r, s, c, in[s][c]);
    }
    int16_t *s = malloc(sizeof(int16_t) * n * 2), *p = malloc(sizeof(int16_t) * n * 2), *mx = malloc(sizeof(int16_t) * n * 2);
    memcpy(s, S, sizeof(int16_t) * n * 2); memcpy(p, P, sizeof(int16_t) * n * 2);
    /* SIM_QUALITY=accurate builds the gate the way the app's "Accurate" setting does */
    coop_pgate_t *g = getenv("SIM_QUALITY") && !strcmp(getenv("SIM_QUALITY"), "accurate")
                      ? coop_pgate_create_quality(RATE, 2, 1000, COOP_PGATE_ACCURATE, COOP_PGATE_ACCURATE_LOOKAHEAD_MS) : coop_pgate_create(RATE, 2, 1000);
    if (trace) {
        char *ea, *eb;
        double a = strtod(argv[7], &ea), b = strtod(argv[8], &eb);
        if (*ea || *eb || !isfinite(a) || !isfinite(b) || a < 0 || b <= a || b > n / (double) RATE) {
            fprintf(stderr, "invalid trace interval\n"); return 2;
        }
        r.trace = fopen(argv[6], "w");
        if (!r.trace) { perror(argv[6]); return 2; }
        r.gate = g;
        r.trace_from = (unsigned long long) (a * RATE);
        r.trace_to = (unsigned long long) (b * RATE);
        fprintf(r.trace, "frame,frames,offset_samples,balance_db,balance_applied,admitted,recentres,pred_confirmed,pred_failed");
        for (int ss = 0; ss < 2; ++ss) for (int c = 0; c < 2; ++c) for (int band = 0; band < NB; ++band)
            fprintf(r.trace, ",s%d_c%d_b%d_g0,s%d_c%d_b%d_g1,s%d_c%d_b%d_why", ss,c,band,ss,c,band,ss,c,band);
        fputc('\n', r.trace);
    }
    coop_pgate_set_hook(g, hook, &r);
    run_gate(g, s, p, n, 2);
    if (r.trace && fclose(r.trace) != 0) { perror("trace close"); return 2; }
    r.trace = NULL;
    const int L = coop_pgate_latency_frames(g);
    char path[1024];
    for (long i = 0; i < n * 2; ++i) mx[i] = clip16(0.75f * S[i] + 0.75f * P[i]);
    snprintf(path, sizeof path, "%s-before.wav", argv[5]); write_wav(path, mx, n, 2);
    for (long i = 0; i + 2L * L < n * 2; ++i) mx[i] = clip16(0.75f * s[i + 2 * L] + 0.75f * p[i + 2 * L]);
    snprintf(path, sizeof path, "%s-after.wav", argv[5]); write_wav(path, mx, n, 2);
    FILE *f = fopen(argv[4], "r");
    double a, b; char lab[64];
    printf("echo removal only (dB): stream overall, then per band");
    for (int k = 0; k < NB; ++k) printf(" %s", BAND_NAME[k]);
    printf("\n");
    while (fscanf(f, "%lf %lf %63s", &a, &b, lab) == 3) {
        const long t0 = (long) (a * RATE), t1 = (long) (b * RATE);
        printf("%-11s", lab);
        for (int st = 0; st < 2; ++st) {
            double ei = 0, eo = 0, bi[NB] = {0}, bo[NB] = {0};
            for (int c = 0; c < 2; ++c) {
                const int k = st * 2 + c;
                ei += en(in[st][c], n, t0, t1, -1); eo += en(r.out[k], n, t0, t1, -1);
                for (int bb = 0; bb < NB; ++bb) { bi[bb] += en(in[st][c], n, t0, t1, bb); bo[bb] += en(r.out[k], n, t0, t1, bb); }
            }
            printf(" | %s %6.1f :", st ? "PC2 " : "VEGA", 10 * log10(eo / ei));
            for (int bb = 0; bb < NB; ++bb) printf(" %5.1f", 10 * log10(bo[bb] / bi[bb]));
        }
        {   /* "S" BURSTS: blocks where a stream's treble (>3.5 kHz) is >= 4x its section median; treble removed there */
            for (int st = 0; st < 2; ++st) {
                long nbk = (t1 - t0) / 240, q = 0;
                double *ei = calloc(nbk + 1, sizeof(double)), *eo = calloc(nbk + 1, sizeof(double)), *srt = calloc(nbk + 1, sizeof(double));
                for (int c = 0; c < 2; ++c) {
                    const int k = st * 2 + c;
                    coop_pgate_xover_t xo; coop_pgate_xover_init(&xo, RATE); float bb[NB];
                    for (long t = t0 - 4800 > 0 ? t0 - 4800 : 0; t < t1 && t < n; ++t) {
                        coop_pgate_xover_run(&xo, r.out[k][t], bb);
                        if (t < t0) continue;
                        q = (t - t0) / 240; if (q >= nbk) break;
                        const double vi = (double) r.bands[k][4][t] * r.bands[k][4][t] + (double) r.bands[k][5][t] * r.bands[k][5][t];
                        ei[q] += vi; eo[q] += (double) bb[4] * bb[4] + (double) bb[5] * bb[5];
                    }
                }
                for (long i = 0; i < nbk; ++i) srt[i] = ei[i];
                for (long i = 0; i < nbk; ++i) for (long j = i + 1; j < nbk; ++j) if (srt[j] < srt[i]) { double tt = srt[i]; srt[i] = srt[j]; srt[j] = tt; }
                const double med = nbk ? srt[nbk / 2] : 0; double si = 0, so = 0; long cnt = 0;
                long leak = 0;
                for (long i = 0; i < nbk; ++i) if (ei[i] > 4 * med && ei[i] > 1e4) {
                    si += ei[i]; so += eo[i]; cnt++;
                    if (eo[i] > 0.1 * ei[i]) {   /* this burst leaked: less than 10 dB removed */
                        leak++;
                        if (getenv("SIM_LEAKS") && st == atoi(getenv("SIM_LEAKS")))
                            fprintf(stderr, "LEAK %s %.3f s removed %.1f dB level %.0f dB\n", st ? "PC2" : "VEGA", (t0 + i * 240) / (double) RATE,
                                    10 * log10((eo[i] + 1) / (ei[i] + 1)), 10 * log10(ei[i] / 240 + 1));
                    }
                }
                printf(" leaky %ld", leak);
                printf(" | %s S-bursts(%ld) treble %5.1f", st ? "PC2" : "VEGA", cnt, cnt ? 10 * log10((so + 1) / (si + 1)) : 0.0);
                free(ei); free(eo); free(srt);
            }
        }
        long flips = 0; int prev = -1;
        for (long k = 0; k < r.nblocks; ++k) {
            if ((long) r.bstart[k] < t0 || (long) r.bstart[k] >= t1) continue;
            int state = r.closed[k][0][0] | r.closed[k][0][1] << 6 | r.closed[k][1][0] << 12 | r.closed[k][1][1] << 18;
            if (prev >= 0 && state != prev) flips++;
            prev = state;
        }
        printf(" | flips/s %5.1f\n", flips / (b - a));
    }
    print_stats(g);
    {   /* HOLES: 5 ms blocks where the gate-only mix falls >= 6 dB below the LOUDER of the two input streams --
         * removing one copy of a shared sound can cost at most ~3 dB there, so a deeper dip removed something that
         * was not a duplicate (a near-silence). Grouped into events; the cause is read from the gate's own state. */
        const double FLOOR = 32768.0 * 32768.0 * 1e-5 * 240;   /* ignore blocks quieter than -50 dBFS */
        long ev_start = -1, ev_n = 0; double ev_depth = 0; int ev_why[4] = {0}, ev_streams = 0;
        long events = 0, ev_blocks = 0;
        const char *why_name[4] = {"open", "match", "hold", "PREDICTION"};
        for (long k = 0; k < r.nblocks; ++k) {
            const long t0 = (long) r.bstart[k];
            if (t0 + 240 > n) break;
            double es = 0, ep = 0, eo = 0;
            for (long i = t0; i < t0 + 240; ++i) for (int c = 0; c < 2; ++c) {
                es += (double) in[0][c][i] * in[0][c][i]; ep += (double) in[1][c][i] * in[1][c][i];
                const double o = (double) r.out[0 * 2 + c][i] + r.out[1 * 2 + c][i]; eo += o * o;
            }
            const double emax = es > ep ? es : ep;
            const int hole = emax > FLOOR && eo < 0.25 * emax;
            if (hole) {
                if (ev_start < 0) { ev_start = t0; ev_n = 0; ev_depth = 0; memset(ev_why, 0, sizeof(ev_why)); ev_streams = 0; }
                ev_n++;
                const double d = 10 * log10((eo + 1) / emax); if (d < ev_depth) ev_depth = d;
                for (int s2 = 0; s2 < 2; ++s2) for (int c = 0; c < 2; ++c) for (int b = 0; b < NB; ++b)
                    if (r.why[k][s2][c][b]) { ev_why[r.why[k][s2][c][b]]++; ev_streams |= 1 << s2; }
            }
            if ((!hole || k == r.nblocks - 1) && ev_start >= 0) {
                events++; ev_blocks += ev_n;
                if (getenv("SIM_HOLES"))
                    printf("  HOLE %8.3f s  %3ld ms  %5.1f dB below the louder stream | closed in %s | causes: match %d hold %d PREDICTION %d\n",
                           ev_start / (double) RATE, ev_n * 5, ev_depth, ev_streams == 3 ? "BOTH streams" : ev_streams == 2 ? "PC2" : ev_streams == 1 ? "VEGA" : "none",
                           ev_why[1], ev_why[2], ev_why[3]);
                ev_start = -1;
            }
            (void) why_name;
        }
        printf("  holes: %ld events, %ld ms in total\n", events, ev_blocks * 5);
    }
    {   /* (1) NEAR-SILENCE: output >= 20 dB below the input mix while the input mix is audible (> -50 dBFS) */
        const double FLOOR = 32768.0 * 32768.0 * 1e-5 * 240 * 2;
        long ev = 0, evms = 0, run = 0; double worst = 0;
        for (long k = 0; k < r.nblocks; ++k) {
            const long t0 = (long) r.bstart[k];
            if (t0 + 240 > n) break;
            double ei = 0, eo = 0;
            for (long i = t0; i < t0 + 240; ++i) for (int c = 0; c < 2; ++c) {
                const double x = (double) in[0][c][i] + in[1][c][i], o = (double) r.out[c][i] + r.out[2 + c][i];
                ei += x * x; eo += o * o;
            }
            const int sil = ei > FLOOR && eo < 0.01 * ei;
            if (sil) { run++; const double d = 10 * log10((eo + 1) / ei); if (d < worst) worst = d;
                if (getenv("SIM_SILENCE")) printf("  NEAR-SILENCE %.3f s %.1f dB (input mix %.0f dBFS)\n", t0 / (double) RATE, 10 * log10((eo + 1) / ei), 10 * log10(ei / 480 / 32768.0 / 32768.0)); }
            if (!sil && run) { ev++; evms += run * 5; run = 0; }
        }
        printf("  near-silences (output >= 20 dB under the input, input audible): %ld events, %ld ms total, deepest %.1f dB\n", ev, evms, worst);
    }
    {   /* (2) FAILED PREDICTIONS: a band closed ahead (why 3) that reopened without the copy ever matching */
        long fails = 0, fail_blocks = 0, confirmed = 0;
        for (int s2 = 0; s2 < 2; ++s2) for (int c = 0; c < 2; ++c) for (int b = 0; b < NB; ++b) {
            long run = 0;
            for (long k = 0; k < r.nblocks; ++k) {
                const unsigned char w = r.why[k][s2][c][b];
                if (w == 3) { run++; continue; }
                if (run) { if (w == 0) { fails++; fail_blocks += run; } else confirmed++; run = 0; }
            }
        }
        printf("  predictions: %ld confirmed, %ld FAILED (copy never came; %ld ms of band-closures in total, all bands/channels)\n",
               confirmed, fails, fail_blocks * 5);
    }
    {   coop_pgate_stats_t st; coop_pgate_get_stats(g, &st);
        printf("  offset changes: %u one-sample drift steps, %u re-centres of an idle offset | offset predictions: %u confirmed, %u failed\n",
               st.drift_steps, st.recentres, st.pred_confirmed, st.pred_failed);
    }
    return 0;
}

/* ================================================================== truth */
static unsigned rng = 12345;
static double frand(void) { rng = rng * 1664525u + 1013904223u; return ((rng >> 8) & 0xFFFFFF) / 16777216.0; }

static float *mono_of(const int16_t *x, long n, int ch, long from, long len) {
    float *m = calloc((size_t) len, sizeof(float));
    for (long i = 0; i < len && from + i < n; ++i) { float v = 0; for (int c = 0; c < ch; ++c) v += x[(from + i) * ch + c]; m[i] = v / ch; }
    return m;
}

/* ---- SPEECH CLASSES (TRUTH_CLASS). Each fills m[] (the earlier copy) and c[] (the later copy) for one "sentence".
 * The material is the real capture (vsrc). Noise-like parts are a NOISE VOCODER of it: in the given bands, white noise
 * shaped by the source band's own envelope -- with an INDEPENDENT noise per copy, the way the two games rendered the
 * narrator's "s" (same loudness shape, different noise; measured 2026-10-05). */
static unsigned nrng = 777;
static float nrand(void) { nrng = nrng * 1664525u + 1013904223u; return ((nrng >> 8) & 0xFFFFFF) / 8388608.0f - 1.0f; }

/* noise-vocode src[0..len) into out[] (added), bands in `mask`, gain g, with noise seed `seed` */
static void vocode(const float *src, long len, float *out, unsigned mask, float g, unsigned seed) {
    coop_pgate_xover_t xs, xn; coop_pgate_xover_init(&xs, RATE); coop_pgate_xover_init(&xn, RATE);
    float bs[NB], bn[NB], env[NB] = {0}, nenv[NB] = {0};
    nrng = seed;
    const float a = 1.0f - expf(-1.0f / (0.002f * RATE));   /* 2 ms envelope followers */
    for (long i = 0; i < len; ++i) {
        coop_pgate_xover_run(&xs, src[i], bs);
        coop_pgate_xover_run(&xn, nrand(), bn);
        float v = 0;
        for (int b = 0; b < NB; ++b) {
            env[b] += a * (fabsf(bs[b]) - env[b]);
            nenv[b] += a * (fabsf(bn[b]) - nenv[b]);
            if (mask & (1u << b)) v += bn[b] / (nenv[b] + 1e-3f) * env[b];
        }
        out[i] += g * v;
    }
}

static void bandpass_of(const float *src, long len, float *out, unsigned mask, float g) {
    coop_pgate_xover_t x; coop_pgate_xover_init(&x, RATE); float bb[NB];
    for (long i = 0; i < len; ++i) { coop_pgate_xover_run(&x, src[i], bb); float v = 0; for (int b = 0; b < NB; ++b) if (mask & (1u << b)) v += bb[b]; out[i] += g * v; }
}

/* returns the class's copy length (may differ from len for the stretched class) */
static long gen_pair(const char *cls, const float *src, long len, float *m, float *c, unsigned seed) {
    memset(m, 0, sizeof(float) * len); memset(c, 0, sizeof(float) * len);
    const unsigned ALL = 0x3F;
    if (!strcmp(cls, "voice")) { for (long i = 0; i < len; ++i) m[i] = c[i] = src[i]; }
    else if (!strcmp(cls, "s")) { vocode(src, len, m, 0x30, 2.0f, seed); vocode(src, len, c, 0x30, 2.0f, seed + 1); }       /* 3.5 kHz+ */
    else if (!strcmp(cls, "sh")) { vocode(src, len, m, 0x18, 1.5f, seed); vocode(src, len, c, 0x18, 1.5f, seed + 1); }      /* 1.5-8 kHz */
    else if (!strcmp(cls, "f")) { vocode(src, len, m, 0x3C, 0.4f, seed); vocode(src, len, c, 0x3C, 0.4f, seed + 1); }       /* weak, 600 Hz+ */
    else if (!strcmp(cls, "h")) { vocode(src, len, m, 0x0C, 1.0f, seed); vocode(src, len, c, 0x0C, 1.0f, seed + 1); }      /* 0.6-3.5 kHz */
    else if (!strcmp(cls, "breath")) { vocode(src, len, m, 0x1E, 0.3f, seed); vocode(src, len, c, 0x1E, 0.3f, seed + 1); } /* quiet, broad */
    else if (!strcmp(cls, "whisper")) { vocode(src, len, m, 0x3E, 1.0f, seed); vocode(src, len, c, 0x3E, 1.0f, seed + 1); } /* all noise */
    else if (!strcmp(cls, "speech_s")) {   /* voiced speech with a sibilant every ~300 ms: what a real line looks like */
        for (long i = 0; i < len; ++i) m[i] = c[i] = src[i];
        for (long a = RATE / 10; a + RATE / 12 < len; a += (long) (0.3 * RATE)) {
            const long L = RATE / 12;   /* 83 ms "s" */
            for (long i = a; i < a + L; ++i) { m[i] *= 0.2f; c[i] *= 0.2f; }
            vocode(src + a, L, m + a, 0x30, 3.0f, seed + (unsigned) a); vocode(src + a, L, c + a, 0x30, 3.0f, seed + 7 + (unsigned) a);
        }
    }
    else if (!strcmp(cls, "plosive")) {    /* 60 ms closure, 8 ms burst (independent noise), 30 ms aspiration, 150 ms vowel */
        long a = 0;
        while (a + RATE / 4 < len) {
            const long cl = (long) (0.06 * RATE), bu = (long) (0.008 * RATE), as = (long) (0.03 * RATE), vo = (long) (0.15 * RATE);
            for (long i = a + cl; i < a + cl + bu; ++i) { nrng = seed + (unsigned) i; m[i] = 3000 * nrand(); nrng = seed + 99991 + (unsigned) i; c[i] = 3000 * nrand(); }
            vocode(src + a + cl + bu, as, m + a + cl + bu, 0x0C, 1.0f, seed + 3 + (unsigned) a); vocode(src + a + cl + bu, as, c + a + cl + bu, 0x0C, 1.0f, seed + 5 + (unsigned) a);
            for (long i = a + cl + bu + as; i < a + cl + bu + as + vo; ++i) m[i] = c[i] = src[i];
            a += cl + bu + as + vo;
        }
    }
    else if (!strcmp(cls, "nasal")) { bandpass_of(src, len, m, 0x03, 1.5f); for (long i = 0; i < len; ++i) c[i] = m[i]; }   /* < 600 Hz only */
    else if (!strcmp(cls, "stretch")) {   /* the copy plays slower (random pitch variation on a bark); TRUTH_STRETCH % */
        const double sf = 1.0 + (getenv("TRUTH_STRETCH") ? atof(getenv("TRUTH_STRETCH")) : 2.0) / 100.0;
        for (long i = 0; i < len; ++i) { m[i] = src[i]; const double p = i / sf; const long q = (long) p; const double f = p - q; c[i] = (float) ((1 - f) * src[q] + f * src[q + 1]); }
    }
    else if (!strcmp(cls, "positional")) {   /* in-world: copy is duller (no top band, -4 dB treble) and has its own room */
        for (long i = 0; i < len; ++i) m[i] = src[i];
        float bb[NB]; coop_pgate_xover_t x; coop_pgate_xover_init(&x, RATE);
        for (long i = 0; i < len; ++i) { coop_pgate_xover_run(&x, src[i], bb); c[i] = bb[0] + bb[1] + bb[2] + bb[3] + 0.63f * bb[4] + 0.1f * bb[5]; }
        float *rv = calloc(len, sizeof(float)); nrng = seed + 11;   /* 80 ms reverb, independent per player */
        const long rl = (long) (0.08 * RATE); float *ir = malloc(sizeof(float) * rl);
        for (long k = 0; k < rl; ++k) ir[k] = nrand() * expf(-k / (0.02f * RATE)) * 0.02f;
        for (long i = 0; i < len; i += 1) { double acc = 0; for (long k = 0; k < rl && k <= i; k += 4) acc += ir[k] * c[i - k]; rv[i] = (float) acc * 4; }
        for (long i = 0; i < len; ++i) c[i] += rv[i];
        free(rv); free(ir);
    }
    else if (!strcmp(cls, "clipped")) {   /* the later PC's mix clips its peaks */
        float pk = 1; for (long i = 0; i < len; ++i) if (fabsf(src[i]) > pk) pk = fabsf(src[i]);
        for (long i = 0; i < len; ++i) { m[i] = src[i]; float v = src[i] * 2.0f; const float lim = 0.6f * pk; c[i] = v > lim ? lim : (v < -lim ? -lim : v); c[i] *= 0.5f; }
    }
    else { fprintf(stderr, "unknown TRUTH_CLASS %s\n", cls); exit(2); }
    /* SPEECH RHYTHM for the noise classes: real fricatives and whispers rise and fall syllable by syllable (~4-6/s);
     * the game audio the vocoder borrows its envelope from barely moves, which would make the test harder than speech.
     * Syllables of 120-250 ms (25 ms attack/decay) with 20-80 ms gaps, the SAME for both copies (it is the line). */
    if (!strcmp(cls, "s") || !strcmp(cls, "sh") || !strcmp(cls, "f") || !strcmp(cls, "h") || !strcmp(cls, "breath") ||
        !strcmp(cls, "whisper")) {
        nrng = seed * 31u + 5u;
        long a = 0;
        float *env = calloc(len, sizeof(float));
        while (a < len) {
            const long syl = (long) ((0.12 + 0.13 * (nrand() * 0.5f + 0.5f)) * RATE), gap = (long) ((0.02 + 0.06 * (nrand() * 0.5f + 0.5f)) * RATE);
            const long at = RATE / 40;
            for (long i = 0; i < syl && a + i < len; ++i) env[a + i] = i < at ? (float) i / at : (i > syl - at ? (float) (syl - i) / at : 1.0f);
            a += syl + gap;
        }
        for (long i = 0; i < len; ++i) { m[i] *= env[i]; c[i] *= env[i]; }
        free(env);
    }
    (void) ALL;
    return len;
}

static int mode_truth(int argc, char **argv) {
    if (argc != 8) return 2;
    const bool unseen = strcmp(argv[1], "truth-unseen") == 0;
    long nv, na, nb2; int cv, ca, cb;
    char bgs[1024]; snprintf(bgs, sizeof bgs, "%s", argv[3]);
    char *comma = strchr(bgs, ','); if (!comma) { fprintf(stderr, "BG must be a,b\n"); return 2; } *comma = 0;
    int16_t *V = read_wav(argv[2], &nv, &cv), *BA = read_wav(bgs, &na, &ca), *BB = read_wav(comma + 1, &nb2, &cb);
    if (!V || !BA || !BB) { fprintf(stderr, "bad input\n"); return 2; }
    const long n = (long) (atof(argv[6]) * RATE);
    float *bg[2] = {mono_of(BA, na, ca, (long) (atof(argv[4]) * RATE), n), mono_of(BB, nb2, cb, (long) (atof(argv[5]) * RATE), n)};
    float *vsrc = mono_of(V, nv, cv, 0, nv);
    double ebg = 0, ev = 0;
    for (long i = 0; i < n; ++i) ebg += bg[0][i] * bg[0][i] + bg[1][i] * bg[1][i];
    ebg /= 2.0 * n;
    for (long i = 0; i < nv; ++i) ev += vsrc[i] * vsrc[i];
    ev /= nv;
    const float vg = (float) sqrt(ebg / (ev + 1) * pow(10, 6.0 / 10));
    const float lv_peer = (float) pow(10, (getenv("TRUTH_PEER_DB") ? atof(getenv("TRUTH_PEER_DB")) : 0.0) / 20);
    float *voice[2] = {calloc(n, sizeof(float)), calloc(n, sizeof(float))};
    float *copy[2] = {calloc(n, sizeof(float)), calloc(n, sizeof(float))};
    float *orig[2] = {calloc(n, sizeof(float)), calloc(n, sizeof(float))};
    /* TRUTH_OFFS="a,b,..." (ms, up to 16) replaces the schedule's offsets -- e.g. two within PG_SAME_MS of each other */
    int offs_ms[16] = {272, 294, 353, -180, 34, 60, -143};
    int noffs = 7;
    if (getenv("TRUTH_OFFS")) {
        noffs = 0;
        for (const char *q = getenv("TRUTH_OFFS"); *q && noffs < 16; ) {
            char *e2; const long v = strtol(q, &e2, 10);
            if (e2 == q) break;
            offs_ms[noffs++] = (int) v;
            q = *e2 == ',' ? e2 + 1 : e2;
        }
        if (noffs == 0) { fprintf(stderr, "bad TRUTH_OFFS\n"); return 2; }
    }
    char offs_txt[160]; int ot = 0;
    for (int i = 0; i < noffs && ot < 150; ++i) ot += snprintf(offs_txt + ot, sizeof offs_txt - (size_t) ot, "%s%d", i ? "," : "", offs_ms[i]);
    typedef struct { long t0, t1; int lag, slave, first; } sent_t;
    sent_t sent[400]; int ns2 = 0;
    long t = 2 * RATE, vp = 20 * RATE;
    int seen[16] = {0};
    while (ns2 < 400) {
        if (unseen && ns2 == noffs) break;
        const long dur = (long) ((1.0 + 2.5 * frand()) * RATE), pause = (long) ((0.4 + 1.1 * frand()) * RATE);
        const int chosen = (int) (frand() * noffs) % noffs;
        const int k = unseen ? ns2 : chosen, lag = offs_ms[k] * RATE / 1000, al = abs(lag);
        if (t + dur + al + RATE > n) break;
        if (vp + dur >= nv) vp = 20 * RATE;
        const int master = lag > 0 ? 0 : 1, slave = 1 - master;
        const long fade = RATE / 100;
        const char *cls = getenv("TRUTH_CLASS") ? getenv("TRUTH_CLASS") : unseen ? "whisper" : "voice";
        if (!unseen && !seen[k]) cls = "voice";   /* legacy truth case deliberately trains each offset first */
        float *gm = malloc(sizeof(float) * dur), *gc = malloc(sizeof(float) * dur);
        gen_pair(cls, vsrc + vp, dur, gm, gc, 1000u + (unsigned) ns2 * 7919u);
        double em = 0, eg = 0; for (long i = 0; i < dur; ++i) { em += (double) vsrc[vp + i] * vsrc[vp + i]; eg += (double) gm[i] * gm[i]; }
        const float norm = (float) sqrt(em / (eg + 1));   /* every class at the narrator's level */
        for (long i = 0; i < dur; ++i) {
            float e = i < fade ? (float) i / fade : (i > dur - fade ? (float) (dur - i) / fade : 1.0f);
            const float v = vg * e * norm * gm[i], w = vg * e * norm * gc[i];
            const float vm = master == 1 ? v * lv_peer : v, vs = slave == 1 ? w * lv_peer : w;   /* host level */
            voice[master][t + i] += vm; orig[master][t + i] += vm;
            voice[slave][t + al + i] += vs; copy[slave][t + al + i] += vs;
        }
        free(gm); free(gc);
        sent[ns2].t0 = t + al; sent[ns2].t1 = t + al + dur; sent[ns2].lag = lag; sent[ns2].slave = slave; sent[ns2].first = !seen[k];
        if (getenv("TRUTH_LIST")) fprintf(stderr, "SENT %d %.3f-%.3f lag %+d ms slave %s %s class=%s\n", ns2, (t + al) / (double) RATE, (t + al + dur) / (double) RATE, offs_ms[k], slave ? "peer" : "self", seen[k] ? "" : "(first scheduled)", cls);
        seen[k] = 1; ns2++;
        vp += dur; t += dur + pause;
    }
    int16_t *st[2];
    for (int s = 0; s < 2; ++s) {
        st[s] = malloc(sizeof(int16_t) * n * 2);
        for (long i = 0; i < n; ++i) st[s][2 * i] = st[s][2 * i + 1] = clip16(bg[s][i] + voice[s][i]);
    }
    int16_t *in0 = malloc(sizeof(int16_t) * n * 2), *in1 = malloc(sizeof(int16_t) * n * 2);
    memcpy(in0, st[0], sizeof(int16_t) * n * 2); memcpy(in1, st[1], sizeof(int16_t) * n * 2);
    rec_t r; memset(&r, 0, sizeof(r)); r.n = n;
    rec_add(&r, 0, 0, copy[0]); rec_add(&r, 1, 0, copy[1]); rec_add(&r, 0, 0, orig[0]); rec_add(&r, 1, 0, orig[1]);
    rec_add(&r, 0, 0, bg[0]); rec_add(&r, 1, 0, bg[1]);
    coop_pgate_t *g = coop_pgate_create(RATE, 2, 1000);
    coop_pgate_set_hook(g, hook, &r);
    run_gate(g, st[0], st[1], n, 2);
    char *in_sent = calloc(n, 1), *slave_at = calloc(n, 1);
    for (int k = 0; k < ns2; ++k) {
        const long al = labs((long) sent[k].lag);
        for (long i = sent[k].t0 - al; i < sent[k].t1 - al; ++i) if (!in_sent[i]) in_sent[i] = 2;
        for (long i = sent[k].t0; i < sent[k].t1; ++i) { in_sent[i] = 1; slave_at[i] = (char) (1 + sent[k].slave); }
    }
    double ce_in = 0, ce_out = 0, oe_in = 0, oe_out = 0, bs_in = 0, bs_out = 0, bp_in = 0, bp_out = 0;
    for (int s = 0; s < 2; ++s) for (long i = RATE; i < n; ++i) {
        ce_in += (double) copy[s][i] * copy[s][i]; ce_out += (double) r.out[s][i] * r.out[s][i];
        oe_in += (double) orig[s][i] * orig[s][i]; oe_out += (double) r.out[2 + s][i] * r.out[2 + s][i];
        const double bi = (double) bg[s][i] * bg[s][i], bo = (double) r.out[4 + s][i] * r.out[4 + s][i];
        if (in_sent[i] == 1 && slave_at[i] == 1 + s) { bs_in += bi; bs_out += bo; } else if (in_sent[i] == 0) { bp_in += bi; bp_out += bo; }
    }
    printf("truth: %d sentences, %.0f s, offsets {%s} ms, narrator +6 dB, PC2 level %+.1f dB\n", ns2,
           n / (double) RATE, offs_txt, 20 * log10(lv_peer));
    if (unseen) puts("  truth-unseen: each offset scheduled once, no voiced substitution; background may still teach incidental offsets");
    printf("  later copy (echo) left:          %6.1f dB\n", 10 * log10((ce_out + 1) / (ce_in + 1)));
    printf("  earlier copy (must stay):        %6.2f dB\n", 10 * log10((oe_out + 1) / (oe_in + 1)));
    printf("  slave background under its copy: %6.2f dB\n", 10 * log10((bs_out + 1) / (bs_in + 1)));
    printf("  background in TRUE pauses:       %6.2f dB\n", 10 * log10((bp_out + 1) / (bp_in + 1)));
    {   /* FALSE BURSTS: the average above hides WHERE the loss is. 5 ms blocks inside true pauses where a stream's own
         * background (unique by construction) lost >= 6 / >= 12 dB while audible (> -50 dBFS): audible damage to sound
         * that had no copy anywhere. */
        const double FLOOR = 32768.0 * 32768.0 * 1e-5 * 240;
        long b6 = 0, b12 = 0, blocks = 0;
        for (int s = 0; s < 2; ++s) for (long a = RATE; a + 240 <= n; a += 240) {
            bool pause = true;
            for (long i = a; i < a + 240 && pause; ++i) if (in_sent[i]) pause = false;
            if (!pause) continue;
            double bi = 0, bo = 0;
            for (long i = a; i < a + 240; ++i) { bi += (double) bg[s][i] * bg[s][i]; bo += (double) r.out[4 + s][i] * r.out[4 + s][i]; }
            if (bi < FLOOR) continue;
            blocks++;
            if (bo < 0.25 * bi) b6++;
            if (bo < 0.0625 * bi) b12++;
        }
        printf("  false bursts in TRUE pauses: %ld of %ld audible stream-blocks (%.2f%%) lost >= 6 dB, %ld (%.2f%%) >= 12 dB\n",
               b6, blocks, blocks ? 100.0 * b6 / blocks : 0.0, b12, blocks ? 100.0 * b12 / blocks : 0.0);
    }
    {   double bi[NB] = {0}, bo[NB] = {0}; float u[NB], w2[NB];
        for (int s = 0; s < 2; ++s) { coop_pgate_xover_t xa, xb; coop_pgate_xover_init(&xa, RATE); coop_pgate_xover_init(&xb, RATE);
            for (long i = 0; i < n; ++i) { coop_pgate_xover_run(&xa, copy[s][i], u); coop_pgate_xover_run(&xb, r.out[s][i], w2);
                if (i >= RATE) for (int k = 0; k < NB; ++k) { bi[k] += (double) u[k] * u[k]; bo[k] += (double) w2[k] * w2[k]; } } }
        printf("  echo left per band:"); for (int k = 0; k < NB; ++k) printf(" %s %.1f", BAND_NAME[k], 10 * log10((bo[k] + 1) / (bi[k] + 1)));
        printf("  (echo energy per band:"); double tot = 0; for (int k = 0; k < NB; ++k) tot += bi[k];
        for (int k = 0; k < NB; ++k) printf(" %.0f%%", 100 * bi[k] / tot);
        printf(")\n"); }
    double on_first[64], on_rep[400], left_rep[400]; int nf = 0, nr = 0;
    for (int k = 0; k < ns2; ++k) {
        const int s = sent[k].slave; const long w = RATE / 200;
        double onset = -1;
        for (long a = sent[k].t0; a + w <= sent[k].t1; a += w) {
            double ei = 0, eo = 0;
            for (long i = a; i < a + w; ++i) { ei += (double) copy[s][i] * copy[s][i]; eo += (double) r.out[s][i] * r.out[s][i]; }
            if (ei > 1e3 && eo / ei <= 0.01) { onset = (a - sent[k].t0) * 1000.0 / RATE; break; }
        }
        double ei = 0, eo = 0;
        for (long i = sent[k].t0; i < sent[k].t1; ++i) { ei += (double) copy[s][i] * copy[s][i]; eo += (double) r.out[s][i] * r.out[s][i]; }
        if (sent[k].first) { if (nf < 64) on_first[nf++] = onset; }
        else { on_rep[nr] = onset; left_rep[nr] = 10 * log10((eo + 1) / (ei + 1)); nr++; }
    }
    double so[400]; int never = 0, m = 0;
    for (int i = 0; i < nr; ++i) { if (on_rep[i] < 0) never++; else so[m++] = on_rep[i]; }
    for (int i = 0; i < m; ++i) for (int j = i + 1; j < m; ++j) if (so[j] < so[i]) { double tt = so[i]; so[i] = so[j]; so[j] = tt; }
    for (int i = 0; i < nr; ++i) for (int j = i + 1; j < nr; ++j) if (left_rep[j] < left_rep[i]) { double tt = left_rep[i]; left_rep[i] = left_rep[j]; left_rep[j] = tt; }
    printf("  first scheduled line per offset (bank novelty not implied): onset ms:");
    for (int i = 0; i < nf; ++i) printf(" %.0f", on_first[i]);
    printf("\n  lines at a REPEATED scheduled offset (%d; bank admission not implied): onset median %.0f ms, 90th %.0f ms, never %d; echo per line median %.1f dB\n", nr,
           m ? so[m / 2] : -1, m ? so[m * 9 / 10] : -1, never, nr ? left_rep[nr / 2] : 0);
    print_stats(g);
    char path[1024]; int16_t *mx = malloc(sizeof(int16_t) * n * 2);
    const int L = coop_pgate_latency_frames(g);
    for (long i = 0; i < n * 2; ++i) mx[i] = clip16(0.75f * in0[i] + 0.75f * in1[i]);
    snprintf(path, sizeof path, "%s-before.wav", argv[7]); write_wav(path, mx, n, 2);
    for (long i = 0; i + 2L * L < n * 2; ++i) mx[i] = clip16(0.75f * st[0][i + 2 * L] + 0.75f * st[1][i + 2 * L]);
    snprintf(path, sizeof path, "%s-after.wav", argv[7]); write_wav(path, mx, n, 2);
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && (!strcmp(argv[1], "real") || !strcmp(argv[1], "real-trace"))) return mode_real(argc, argv);
    if (argc > 1 && (!strcmp(argv[1], "truth") || !strcmp(argv[1], "truth-unseen"))) return mode_truth(argc, argv);
    fprintf(stderr, "usage: %s real SELF PEER SECTIONS OUT | real-trace SELF PEER SECTIONS OUT CSV FROM TO | truth[-unseen] VOICE BG_A,BG_B A_FROM B_FROM SECONDS OUT\n", argv[0]);
    return 2;
}
