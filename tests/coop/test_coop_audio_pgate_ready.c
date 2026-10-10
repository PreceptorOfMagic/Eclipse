/* Complete-block gate API contract: no additional held block, indexed equivalence,
 * wrong-mode/size rejection and explicit epoch reset. No live-quality assertion. */
#include "coop_audio_pgate.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { unsigned calls, closed; unsigned long long next; } observed_t;
static void observe(void *ctx, unsigned long long start, int frames,
                    const float a[2][2][COOP_PGATE_BANDS], const float b[2][2][COOP_PGATE_BANDS],
                    const unsigned char why[2][2][COOP_PGATE_BANDS]) {
    observed_t *o = ctx;
    (void)a; (void)why;
    assert(start == o->next);
    o->next += (unsigned)frames; ++o->calls;
    for (int s=0;s<2;++s) for(int c=0;c<2;++c) for(int k=0;k<COOP_PGATE_BANDS;++k)
        o->closed += b[s][c][k] < 1.0f;
}
static uint32_t rnd(uint32_t *r) { *r = *r * 1664525u + 1013904223u; return *r; }
static void exercise(int rate, int channels) {
    coop_pgate_t *ready=coop_pgate_create(rate,channels,1000), *legacy=coop_pgate_create(rate,channels,1000);
    /* "Accurate" driven through the ready-block entry cannot hold blocks back: it must run exactly as FAST */
    coop_pgate_t *acc=coop_pgate_create_quality(rate,channels,1000,COOP_PGATE_ACCURATE, COOP_PGATE_ACCURATE_LOOKAHEAD_MS);
    assert(ready && legacy && acc);
    const int block=coop_pgate_block_frames(ready), n=rate*3, samples=n*channels, bs=block*channels;
    assert(block==coop_pgate_latency_frames(legacy));
    int16_t *input[2], *output[2];
    int16_t *a=malloc((size_t)bs*sizeof(*a)),*b=malloc((size_t)bs*sizeof(*b));
    int16_t *c=malloc((size_t)bs*sizeof(*c)),*d=malloc((size_t)bs*sizeof(*d));
    int16_t *e=malloc((size_t)bs*sizeof(*e)),*f=malloc((size_t)bs*sizeof(*f)); assert(e&&f);
    float *shared=malloc((size_t)n*sizeof(*shared)); assert(a&&b&&c&&d&&shared);
    uint32_t r=41731; float low=0;
    for(int i=0;i<n;++i) { low=.45f*low+.55f*((int)(rnd(&r)>>16)-32768); shared[i]=low*.35f; }
    for(int s=0;s<2;++s) {
        input[s]=malloc((size_t)samples*sizeof(int16_t));output[s]=malloc((size_t)samples*sizeof(int16_t));
        assert(input[s]&&output[s]); uint32_t state=91u+(unsigned)s;
        for(int i=0;i<n;++i) for(int ch=0;ch<channels;++ch) {
            int value=(int)(rnd(&state)%71)-35;
            int source=i-(s?rate*272/1000:0);
            if(source>=rate*12/10 && source<rate*25/10) value+=(int)shared[source];
            input[s][i*channels+ch]=(int16_t)value;
        }
    }
    observed_t ro={0},lo={0};coop_pgate_set_hook(ready,observe,&ro);coop_pgate_set_hook(legacy,observe,&lo);
    memcpy(a,input[0],(size_t)bs*2);memcpy(b,input[1],(size_t)bs*2);
    assert(!coop_pgate_process_ready_block(ready,a,b,block-1));assert(ro.calls==0);
    assert(memcmp(a,input[0],(size_t)bs*2)==0 && memcmp(b,input[1],(size_t)bs*2)==0);
    for(int frame=0;frame<n;frame+=block) {
        memcpy(a,input[0]+frame*channels,(size_t)bs*2);memcpy(b,input[1]+frame*channels,(size_t)bs*2);
        memcpy(c,a,(size_t)bs*2);memcpy(d,b,(size_t)bs*2);
        memcpy(e,a,(size_t)bs*2);memcpy(f,b,(size_t)bs*2);
        assert(coop_pgate_process_ready_block(ready,a,b,block));coop_pgate_process(legacy,c,d,block);
        assert(coop_pgate_process_ready_block(acc,e,f,block));
        assert(memcmp(e,a,(size_t)bs*2)==0&&memcmp(f,b,(size_t)bs*2)==0);
        memcpy(output[0]+frame*channels,a,(size_t)bs*2);memcpy(output[1]+frame*channels,b,(size_t)bs*2);
        if(frame==0) for(int i=0;i<bs;++i) { assert(c[i]==0&&d[i]==0);assert(a[i]==input[0][i]&&b[i]==input[1][i]); }
        else { assert(memcmp(c,output[0]+(frame-block)*channels,(size_t)bs*2)==0);assert(memcmp(d,output[1]+(frame-block)*channels,(size_t)bs*2)==0); }
    }
    assert(ro.calls==lo.calls && ro.closed==lo.closed && ro.closed>0);
    assert(coop_pgate_quality(acc)==COOP_PGATE_FAST && coop_pgate_quality(ready)==COOP_PGATE_FAST);
    coop_pgate_destroy(acc); free(e); free(f);
    {   /* and on the streaming entry it starts ACCURATE: look-ahead in the reported delay */
        coop_pgate_t *acc2=coop_pgate_create_quality(rate,channels,1000,COOP_PGATE_ACCURATE, COOP_PGATE_ACCURATE_LOOKAHEAD_MS); assert(acc2);
        assert(coop_pgate_quality(acc2)==COOP_PGATE_FAST);   /* not settled before the first call */
        int16_t *x=calloc((size_t)bs,sizeof(*x)),*y=calloc((size_t)bs,sizeof(*y)); assert(x&&y);
        coop_pgate_process(acc2,x,y,block);
        assert(coop_pgate_quality(acc2)==COOP_PGATE_ACCURATE, COOP_PGATE_ACCURATE_LOOKAHEAD_MS);
        assert(coop_pgate_latency_frames(acc2)==block+(rate/1000*COOP_PGATE_ACCURATE_LOOKAHEAD_MS+block-1)/block*block);
        coop_pgate_destroy(acc2); free(x); free(y);
    }
    coop_pgate_stats_t sr,sl;coop_pgate_get_stats(ready,&sr);coop_pgate_get_stats(legacy,&sl);
    assert(memcmp(&sr,&sl,sizeof sr)==0 && sr.admitted>0);
    memset(c,0,(size_t)bs*2);memset(d,0,(size_t)bs*2);coop_pgate_process(legacy,c,d,block);
    assert(memcmp(c,output[0]+(n-block)*channels,(size_t)bs*2)==0);assert(memcmp(d,output[1]+(n-block)*channels,(size_t)bs*2)==0);
    unsigned calls=ro.calls;memcpy(c,a,(size_t)bs*2);memcpy(d,b,(size_t)bs*2);
    coop_pgate_process(ready,a,b,block);assert(ro.calls==calls);assert(memcmp(c,a,(size_t)bs*2)==0&&memcmp(d,b,(size_t)bs*2)==0);
    assert(!coop_pgate_process_ready_block(legacy,c,d,block));
    assert(coop_pgate_reset(ready));coop_pgate_get_stats(ready,&sr);assert(sr.blocks==0&&sr.offsets==0&&sr.admitted==0);
    ro=(observed_t){0};memcpy(a,input[0],(size_t)bs*2);memcpy(b,input[1],(size_t)bs*2);
    assert(coop_pgate_process_ready_block(ready,a,b,block));assert(ro.calls==1);assert(memcmp(a,input[0],(size_t)bs*2)==0&&memcmp(b,input[1],(size_t)bs*2)==0);
    assert(coop_pgate_reset(ready));ro=(observed_t){0};coop_pgate_process(ready,a,b,block);assert(ro.calls==1);
    for(int i=0;i<bs;++i)assert(a[i]==0&&b[i]==0);
    /* Full learned epoch after reset must be identical to a newly allocated gate. */
    coop_pgate_t *fresh=coop_pgate_create(rate,channels,1000);assert(fresh);
    assert(coop_pgate_reset(ready));ro=(observed_t){0};
    for(int frame=0;frame<n;frame+=block) {
        memcpy(a,input[0]+frame*channels,(size_t)bs*2);memcpy(b,input[1]+frame*channels,(size_t)bs*2);
        memcpy(c,a,(size_t)bs*2);memcpy(d,b,(size_t)bs*2);
        assert(coop_pgate_process_ready_block(ready,a,b,block));
        assert(coop_pgate_process_ready_block(fresh,c,d,block));
        assert(memcmp(a,c,(size_t)bs*2)==0 && memcmp(b,d,(size_t)bs*2)==0);
    }
    coop_pgate_get_stats(ready,&sr);coop_pgate_get_stats(fresh,&sl);assert(memcmp(&sr,&sl,sizeof sr)==0);
    coop_pgate_destroy(fresh);
    /* A marked-concealed leader burst cannot become evidence later, even after both
     * current inputs are valid again and its perfect delayed copy is arriving. */
    coop_pgate_t *invalid=coop_pgate_create(rate,channels,1000);assert(invalid);
    for(int frame=0;frame<n;frame+=block) {
        memcpy(a,input[0]+frame*channels,(size_t)bs*2);memcpy(b,input[1]+frame*channels,(size_t)bs*2);
        unsigned mask=frame>=rate*12/10 && frame<rate*25/10 ? COOP_PGATE_VALID_PEER : COOP_PGATE_VALID_BOTH;
        assert(coop_pgate_process_ready_block_valid(invalid,a,b,block,mask));
        assert(memcmp(a,input[0]+frame*channels,(size_t)bs*2)==0);
        assert(memcmp(b,input[1]+frame*channels,(size_t)bs*2)==0);
    }
    coop_pgate_get_stats(invalid,&sr);assert(sr.admitted==0 && sr.balance_samples==0);
    assert(!coop_pgate_process_ready_block_valid(invalid,a,b,block,8u));
    coop_pgate_destroy(invalid);
    /* A quiet/missing CURRENT leader must not invalidate its valid historical copy. */
    coop_pgate_t *quiet=coop_pgate_create(rate,channels,1000);assert(quiet);
    double input_energy=0, output_energy=0;
    for(int frame=0;frame<n;frame+=block) {
        memcpy(a,input[0]+frame*channels,(size_t)bs*2);memcpy(b,input[1]+frame*channels,(size_t)bs*2);
        unsigned mask=COOP_PGATE_VALID_BOTH;
        if(frame>=rate*25/10) { memset(a,0,(size_t)bs*2);mask=COOP_PGATE_VALID_PEER; }
        assert(coop_pgate_process_ready_block_valid(quiet,a,b,block,mask));
        if(frame>=rate*25/10 && frame<rate*27/10) for(int i=0;i<bs;++i) {
            const double raw=input[1][frame*channels+i];input_energy+=raw*raw;output_energy+=(double)b[i]*b[i];
            assert(a[i]==0);
        }
    }
    assert(input_energy>0 && output_energy/input_energy<0.2);
    coop_pgate_destroy(quiet);
    coop_pgate_destroy(ready);coop_pgate_destroy(legacy);
    free(a);free(b);free(c);free(d);free(shared);for(int s=0;s<2;++s){free(input[s]);free(output[s]);}
    printf("ready-block indexed equivalence/reset passed: rate=%d channels=%d\n",rate,channels);
}
int main(void) { assert(coop_pgate_block_frames(NULL)==0);assert(!coop_pgate_reset(NULL));exercise(48000,1);exercise(48000,2);exercise(96000,2);return 0; }
