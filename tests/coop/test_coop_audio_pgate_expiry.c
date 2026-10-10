/* Quiet-search optimization must still expire a naturally acquired offset after prolonged silence.
 * Pure component fixture; no live or perceptual acceptance claim. Keep assertions enabled. */
#include "coop_audio_pgate.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
int main(void){
 coop_pgate_t*g=coop_pgate_create(48000,2,1000);assert(g);int16_t shared[24000],a[480],b[480];uint32_t seed=719;
 for(int i=0;i<24000;++i){seed=seed*1664525u+1013904223u;shared[i]=(int16_t)(((int)(seed>>16)-32768)/3);}
 coop_pgate_stats_t before={0},after={0};
 for(int t=0;t<67*48000;t+=240){for(int i=0;i<240;++i){int s=t+i,p=s-13056;int16_t x=s<24000?shared[s]:0,y=p>=0&&p<24000?shared[p]:0;a[2*i]=a[2*i+1]=x;b[2*i]=b[2*i+1]=y;}
  assert(coop_pgate_process_ready_block(g,a,b,240));if(t==2*48000){coop_pgate_get_stats(g,&before);assert(before.offsets>0&&before.admitted>0);}}
 coop_pgate_get_stats(g,&after);assert(after.offsets==0&&after.evicted>0);
 printf("quiet_expiry before_offsets=%d before_admitted=%u final_offsets=%d evicted=%u searches=%u blocks=%u\n",before.offsets,before.admitted,after.offsets,after.evicted,after.searches,after.blocks);coop_pgate_destroy(g);return 0;
}
