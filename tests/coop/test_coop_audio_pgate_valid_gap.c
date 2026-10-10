/* Two actual-input episodes separated by missing/PLC-marked silence. The second
 * has a different lag and opposite leader. Assertions cover the final episode. */
#include "coop_audio_pgate.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int later; long onset, first_close; unsigned earlier_closed; } observation;
static void observe(void *ctx,unsigned long long start,int frames,
                    const float g0[2][2][COOP_PGATE_BANDS],
                    const float g1[2][2][COOP_PGATE_BANDS],
                    const unsigned char why[2][2][COOP_PGATE_BANDS]) {
    observation *o=ctx; (void)g0; (void)why;
    if ((long)start+frames<=o->onset) return;
    if (g1[o->later][0][2]==0 && g1[o->later][0][3]==0 && o->first_close<0)
        o->first_close=(long)start+frames;
    for (int b=0;b<COOP_PGATE_BANDS;++b) o->earlier_closed+=g1[1-o->later][0][b]<1;
}
static uint32_t next(uint32_t *s) { *s=*s*1664525u+1013904223u; return *s; }
static void exercise(int later,int lag_ms,int phase_ms,int independent) {
    enum { RATE=48000, N=RATE*4, EVENT=RATE/4 };
    coop_pgate_t *g=coop_pgate_create(RATE,2,1000); assert(g);
    const int B=coop_pgate_block_frames(g), onset=RATE*2+phase_ms*48, lag=lag_ms*48;
    int16_t motif[2][EVENT],a[480],b[480],before[2][480];
    uint32_t seed=41731,noise[2]={911,912}; float low=0;
    for (int e=0;e<2;++e) for (int i=0;i<EVENT;++i) {
        low=.35f*low+.65f*((int)(next(&seed)>>16)-32768);
        motif[e][i]=(int16_t)(low*.4f);
    }
    observation o={later,onset+lag,-1,0}; coop_pgate_set_hook(g,observe,&o);
    for (int frame=0;frame<N;frame+=B) {
        int16_t *out[2]={a,b}; unsigned mask=0;
        for (int s=0;s<2;++s) {
            const int starts[2]={RATE/5+(s==1-later?272*48:0),onset+(s==later?lag:0)};
            for (int e=0;e<2;++e)
                if (frame>=starts[e] && frame+B<=starts[e]+EVENT+RATE/10) mask|=1u<<s;
            for (int i=0;i<B;++i) {
                int v=0;
                for (int e=0;e<2;++e) {
                    const int k=frame+i-starts[e];
                    if (k>=0 && k<EVENT+RATE/10) v=(int)(next(&noise[s])%65)-32;
                    if (k>=0 && k<EVENT) {
                        int sample=independent?((int)(next(&noise[s])>>16)-32768)/3:motif[e][k];
                        v+=(int)(sample*(k<B?(float)k/B:1.0f));
                    }
                }
                out[s][2*i]=out[s][2*i+1]=(int16_t)v;
            }
        }
        memcpy(before[0],a,sizeof a); memcpy(before[1],b,sizeof b);
        assert(coop_pgate_process_ready_block_valid(g,a,b,B,mask));
        for (int s=0;s<2;++s)
            if (!(mask&(1u<<s)) || independent) assert(memcmp(out[s],before[s],sizeof a)==0);
    }
    coop_pgate_stats_t st; coop_pgate_get_stats(g,&st);
    if (independent) { assert(st.offsets==0); assert(o.first_close<0); }
    else {
        assert(o.first_close>=o.onset && o.first_close-o.onset<=3*B);
        assert(o.earlier_closed==0);
        int found=0, expected=(later?1:-1)*lag;
        for (int j=0;j<st.offsets;++j) if (abs(st.offset_samples[j]-expected)<=1) found=1;
        assert(found);
    }
    coop_pgate_destroy(g);
}
int main(void) {
    for (int later=0;later<2;++later) for (int phase=0;phase<=7;phase+=7) {
        exercise(later,34,phase,0); exercise(later,900,phase,0);
        exercise(later,34,phase,1); exercise(later,900,phase,1);
    }
    puts("valid gap new-lag/opposite-leader/independent assertions PASS (16 cases)");
    return 0;
}
