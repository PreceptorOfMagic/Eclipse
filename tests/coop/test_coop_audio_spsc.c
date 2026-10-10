/* Publication/continuity evidence with actual concurrent owners. No sound device
 * and no wall-clock latency claim. Every accepted frame must emerge once. */
#include "coop_audio_spsc.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>

enum { TOTAL = 1000000 };
static coop_audio_spsc_t queue;

static void *produce(void *unused) {
    (void)unused;
    int16_t pcm[62];
    for (unsigned index=0; index<TOTAL;) {
        unsigned n=1+(index%31);
        if (n>TOTAL-index) n=TOTAL-index;
        /* Never put two epochs in one packet. */
        unsigned until_epoch=10000-index%10000;
        if(n>until_epoch)n=until_epoch;
        for(unsigned i=0;i<n;++i) {
            pcm[2*i]=(int16_t)((index+i)%30000);
            pcm[2*i+1]=(int16_t)(-pcm[2*i]);
        }
        if(coop_audio_spsc_push(&queue,pcm,n,2,index,index+100,1+index/10000,index%3))index+=n;
        else sched_yield();
    }
    return NULL;
}

static void *consume(void *unused) {
    (void)unused;
    coop_audio_frame_t frames[47];
    unsigned packet_start=0,packet_end=0;
    for(unsigned expected=0;expected<TOTAL;) {
        unsigned n=coop_audio_spsc_pop(&queue,frames,1+expected%47);
        for(unsigned i=0;i<n;++i) {
            if(expected+i==packet_end) {
                packet_start=packet_end;
                unsigned size=1+packet_start%31;
                if(size>10000-packet_start%10000)size=10000-packet_start%10000;
                if(size>TOTAL-packet_start)size=TOTAL-packet_start;
                packet_end=packet_start+size;
            }
            assert(frames[i].index==expected+i);
            assert(frames[i].pcm[0]==(int16_t)((expected+i)%30000));
            assert(frames[i].pcm[1]==-frames[i].pcm[0]);
            assert(frames[i].epoch==1+(expected+i)/10000);
            assert(frames[i].epoch==frames[0].epoch);
            assert(frames[i].ready_us==packet_start+100);
            assert(frames[i].flags==packet_start%3);
        }
        expected+=n;
        if(!n)sched_yield();
    }
    return NULL;
}

static void boundaries(void) {
    coop_audio_spsc_t q;
    int16_t pcm[]={11,12,13,14,15,16,17,18};
    coop_audio_frame_t got[8];
    assert(coop_audio_spsc_init(&q,7));
    assert(coop_audio_spsc_push(&q,pcm,5,1,100,1,7,0));
    assert(!coop_audio_spsc_push(&q,pcm,3,1,105,2,7,1));
    assert(coop_audio_spsc_count(&q)==5);
    assert(coop_audio_spsc_pop(&q,got,3)==3 && got[2].index==102);
    assert(coop_audio_spsc_push(&q,pcm,5,1,0,3,8,1));
    assert(coop_audio_spsc_pop(&q,got,7)==2 && got[1].index==104);
    assert(coop_audio_spsc_pop(&q,got,7)==5 && got[4].index==4);
    for(unsigned i=0;i<5;++i)assert(got[i].flags==1 && got[i].epoch==8 && got[i].pcm[0]==pcm[i]);
    assert(!coop_audio_spsc_count(&q));
    assert(!coop_audio_spsc_push(&q,pcm,2,1,UINT64_MAX,1,1,0));
    assert(!coop_audio_spsc_push(&q,pcm,2,3,0,1,1,0));
    assert(!coop_audio_spsc_push(&q,pcm,2,1,0,1,0,0));
    coop_audio_spsc_destroy(&q);
}

int main(void) {
    boundaries();
    assert(coop_audio_spsc_init(&queue,126));
    pthread_t producer,consumer;
    assert(!pthread_create(&producer,NULL,produce,NULL));
    assert(!pthread_create(&consumer,NULL,consume,NULL));
    assert(!pthread_join(producer,NULL));
    assert(!pthread_join(consumer,NULL));
    assert(!coop_audio_spsc_count(&queue));
    coop_audio_spsc_destroy(&queue);
    puts("SPSC: one million ordered stereo frames, epoch boundaries and full/wrap conservation PASS");
}
