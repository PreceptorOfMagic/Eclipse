/* Offline first-audio-after-idle discriminator. No production edits or live claim.
 * mode=0: genuine low PCM background remains valid from frame zero (old fixture).
 * mode=1: identical silence-before-onset PCM to mode=2, but all blocks marked valid.
 * mode=2: a source becomes valid only at its first wholly post-onset block.
 * Once begun, each source continues real low background after the shared event.
 */
#include "coop_audio_pgate.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int later;
    long onset, first_close;
    unsigned earlier_closed;
} observation;

static void observe(void *ctx, unsigned long long start, int frames,
                    const float g0[2][2][COOP_PGATE_BANDS],
                    const float g1[2][2][COOP_PGATE_BANDS],
                    const unsigned char why[2][2][COOP_PGATE_BANDS]) {
    observation *o=ctx; (void)g0; (void)why;
    if ((long)start+frames>o->onset && g1[o->later][0][2]==0 &&
        g1[o->later][0][3]==0 && o->first_close<0)
        o->first_close=(long)start+frames;
    for (int b=0;b<COOP_PGATE_BANDS;++b)
        o->earlier_closed+=g1[1-o->later][0][b]<1;
}
static uint32_t next(uint32_t *s) { *s=*s*1664525u+1013904223u; return *s; }

static void exercise(int mode,int lag_ms,int start_ms,int later,int independent) {
    const int rate=48000, channels=2;
    coop_pgate_t *g=coop_pgate_create(rate,channels,1000); assert(g);
    int B=coop_pgate_block_frames(g), N=(rate*4/B)*B;
    int lag=(rate/1000)*lag_ms, onset=rate*2+(rate/1000)*start_ms;
    int16_t *source=malloc((size_t)N*sizeof(*source)); assert(source);
    int16_t a[480],b[480],before[2][480]; assert(B*channels==480);
    uint32_t seed=41731,noise[2]={911,912}; float low=0;
    for (int i=0;i<N;++i) {
        low=.35f*low+.65f*((int)(next(&seed)>>16)-32768);
        source[i]=(int16_t)(low*.4f);
    }
    observation o={later,onset+lag,-1,0}; coop_pgate_set_hook(g,observe,&o);
    long first_bank=-1; int first_bank_lag=0,earlier_max_pcm_error=0;
    unsigned independent_changed_samples=0;
    for (int frame=0;frame<N;frame+=B) {
        int16_t *out[2]={a,b}; unsigned mask=0;
        for (int s=0;s<2;++s) {
            int source_onset=onset+(s==later?lag:0);
            if (mode!=2 || frame>=source_onset) mask|=1u<<s;
            for (int i=0;i<B;++i) {
                int v=(int)(next(&noise[s])%65)-32;
                int k=frame+i-source_onset;
                if (mode!=0 && k<0) v=0;
                if (k>=0 && k<rate/2) {
                    float fade=k<B?(float)k/B:1.0f;
                    int sample=independent?((int)(next(&noise[s])>>16)-32768)/3:source[k];
                    v+=(int)(sample*fade);
                }
                for (int c=0;c<channels;++c) out[s][i*channels+c]=(int16_t)v;
            }
        }
        memcpy(before[0],a,sizeof a); memcpy(before[1],b,sizeof b);
        assert(coop_pgate_process_ready_block_valid(g,a,b,B,mask));
        coop_pgate_stats_t st; coop_pgate_get_stats(g,&st);
        if (first_bank<0 && st.offsets>0) {
            first_bank=frame+B; first_bank_lag=st.offset_samples[0];
        }
        for (int i=0;i<B*channels;++i) {
            int error=abs(out[1-later][i]-before[1-later][i]);
            if (error>earlier_max_pcm_error) earlier_max_pcm_error=error;
            if (independent) for (int s=0;s<2;++s)
                independent_changed_samples+=out[s][i]!=before[s][i];
        }
    }
    printf("mode=%d lag_ms=%d start_ms=%d later=%d independent=%d first_bank_frames=%ld first_bank_lag=%d first_decision_frames=%ld later_first_valid_frames=%ld earlier_closed=%u earlier_max_pcm_error=%d independent_changed_samples=%u\n",
           mode,lag_ms,start_ms,later,independent,
           first_bank<0?-1:first_bank-o.onset,first_bank_lag,
           o.first_close<0?-1:o.first_close-o.onset,
           ((o.onset+B-1)/B)*B-o.onset,o.earlier_closed,
           earlier_max_pcm_error,independent_changed_samples);
    if (independent) {
        assert(first_bank<0 && o.first_close<0 && independent_changed_samples==0);
    } else {
        assert(first_bank_lag==(later?1:-1)*lag);
        assert(first_bank>=o.onset && first_bank-o.onset<=3*B);
        assert(o.first_close>=o.onset && o.first_close-o.onset<=3*B);
        assert(o.earlier_closed==0);
    }
    coop_pgate_destroy(g); free(source);
}
int main(void) {
    const int lags[]={34,272,900};
    for (int mode=0;mode<3;++mode)
        for (int later=0;later<2;++later)
            for (int start=0;start<=7;start+=7)
                for (unsigned l=0;l<sizeof lags/sizeof lags[0];++l)
                    exercise(mode,lags[l],start,later,0);
    for (int mode=0;mode<3;++mode)
        for (int later=0;later<2;++later)
            exercise(mode,272,7,later,1);
    return 0;
}
