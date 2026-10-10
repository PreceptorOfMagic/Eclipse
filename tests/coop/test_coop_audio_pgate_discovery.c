/* Offline cold admission/control fixture. No live/perceptual acceptance claim. */
#include "coop_audio_pgate.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int later, block; long onset, first_close; unsigned earlier_closed; } observation;
static void observe(void *ctx,unsigned long long start,int frames,const float g0[2][2][COOP_PGATE_BANDS],const float g1[2][2][COOP_PGATE_BANDS],const unsigned char why[2][2][COOP_PGATE_BANDS]) {
    observation *o=ctx; (void)g0;(void)why;
    if((long)start+frames>o->onset && g1[o->later][0][2]==0 && g1[o->later][0][3]==0 && o->first_close<0) o->first_close=(long)start+frames;
    for(int b=0;b<COOP_PGATE_BANDS;++b) o->earlier_closed+=g1[1-o->later][0][b]<1;
}
static uint32_t next(uint32_t *s){*s=*s*1664525u+1013904223u;return *s;}
static void exercise(int rate,int channels,int lag_ms,int start_ms,int later,int independent){
    coop_pgate_t *g=coop_pgate_create(rate,channels,1000);assert(g);
    int B=coop_pgate_block_frames(g),N=(rate*2/B)*B,lag=(rate/1000)*lag_ms,onset=(rate/1000)*start_ms;
    int16_t *source=malloc((size_t)N*sizeof(*source)),*a=malloc((size_t)B*channels*sizeof(*a)),*b=malloc((size_t)B*channels*sizeof(*b));assert(source&&a&&b);
    uint32_t seed=41731,noise[2]={911,912};float low=0;
    for(int i=0;i<N;++i){low=.35f*low+.65f*((int)(next(&seed)>>16)-32768);source[i]=(int16_t)(low*.4f);}
    observation o={later,B,onset+lag,-1,0};coop_pgate_set_hook(g,observe,&o);
    int16_t before[2][1920];assert(B*channels<=1920);
    for(int frame=0;frame<N;frame+=B){
        int16_t *out[2]={a,b};
        for(int s=0;s<2;++s)for(int i=0;i<B;++i){
            int v=(int)(next(&noise[s])%65)-32;
            int k=frame+i-onset-(s==later?lag:0);
            if(k>=0 && k<rate/2){
                float fade=k<B?(float)k/B:1.0f;
                int sample=independent?((int)(next(&noise[s])>>16)-32768)/3:source[k];
                v+=(int)(sample*fade);
            }
            for(int c=0;c<channels;++c)out[s][i*channels+c]=(int16_t)v;
        }
        memcpy(before[0],a,(size_t)B*channels*sizeof(*a));memcpy(before[1],b,(size_t)B*channels*sizeof(*b));
        assert(coop_pgate_process_ready_block(g,a,b,B));
        if(independent){assert(memcmp(before[0],a,(size_t)B*channels*sizeof(*a))==0);assert(memcmp(before[1],b,(size_t)B*channels*sizeof(*b))==0);}
    }
    coop_pgate_stats_t st;coop_pgate_get_stats(g,&st);
    if(independent)assert(st.admitted==0);
    else {assert(o.first_close>=o.onset);assert(o.first_close-o.onset<=3*B);assert(o.earlier_closed==0);}
    printf("cold rate=%d ch=%d lag=%d start=%d later=%d independent=%d first_decision_frames=%ld admitted=%u earlier_closed=%u\n",rate,channels,lag_ms,start_ms,later,independent,o.first_close<0?-1:o.first_close-o.onset,st.admitted,o.earlier_closed);
    coop_pgate_destroy(g);free(source);free(a);free(b);
}
int main(void){
    const int rates[]={8000,44100,48000,96000};
    for(unsigned r=0;r<sizeof rates/sizeof rates[0];++r)
        for(int later=0;later<2;++later)for(int start=0;start<=7;start+=7)
            for(int lag=0;lag<3;++lag){const int lags[]={34,272,900};exercise(rates[r],r==0?1:2,lags[lag],start,later,0);}
    for(unsigned r=0;r<sizeof rates/sizeof rates[0];++r)for(int start=0;start<=7;start+=7)exercise(rates[r],r==0?1:2,272,start,1,1);
    return 0;
}
