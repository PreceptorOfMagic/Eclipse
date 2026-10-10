/* Exercise the real SS4S player against a mock of the audited NDL lifecycle:
 * setter affects the next load; unload clears it. This catches late application. */
#include <unistd.h>
#include <stdarg.h>
#include "../../third_party/ss4s/modules/webos/ndl/webos5/ndl_player.c"
static int pending, applied, calls, loads, unloads;
static void quiet(SS4S_LogLevel level,const char *tag,const char *fmt,...) {
    (void)level;(void)tag;(void)fmt;
}
bool SS4S_NDL_webOS5_Initialized;
SS4S_LoggingFunction *SS4S_NDL_webOS5_Log=quiet;
int NDL_DirectMediaInit(const char *id,ResourceReleased cb) {
    (void)id;(void)cb; return 0;
}
const char *NDL_DirectMediaGetError(void) { return "mock"; }
int NDL_DirectVideoSetFrameDropThreshold(int n) {
    assert(SS4S_NDL_webOS5_Initialized);
    calls++; if(n>1) pending=n; return 0;
}
int NDL_DirectMediaLoad(NDL_DIRECTMEDIA_DATA_INFO_T *info,NDLMediaLoadCallback cb) {
    (void)info;(void)cb; applied=pending; loads++; return 0;
}
int NDL_DirectMediaUnload(void) { pending=0; unloads++; return 0; }
int NDL_DirectAudioPlay(void *p,unsigned int n,long long pts) {
    (void)p;(void)n;(void)pts; return 0;
}
int SS4S_OpusEmptyFeed(SS4S_OpusEmpty *p,SS4S_NDLOpusEmptyFeedFunc f,void *arg) {
    (void)p;(void)f;(void)arg; return 0;
}
static void config(const char *text) {
    FILE *f=fopen(AURORA_NDL_DROP_THRESHOLD_PATH,"w"); assert(f);
    fputs(text,f); assert(fclose(f)==0);
}
int main(void) {
    SS4S_PlayerContext c={0}; c.mediaInfo.video.type=NDL_VIDEO_TYPE_H264;
    unlink(AURORA_NDL_DROP_THRESHOLD_PATH);
    assert(LoadMedia(&c)==0 && calls==0 && applied==0);
    config("2\n");
    assert(SS4S_NDL_webOS5_ReloadMedia(&c)==0 && calls==1 && applied==2);
    assert(SS4S_NDL_webOS5_ReloadMedia(&c)==0 && calls==2 && applied==2);
    config("1\n");
    assert(SS4S_NDL_webOS5_ReloadMedia(&c)==0 && calls==2 && applied==0);
    config("not-an-integer\n");
    assert(SS4S_NDL_webOS5_ReloadMedia(&c)==0 && calls==2 && applied==0);
    unlink(AURORA_NDL_DROP_THRESHOLD_PATH);
    assert(SS4S_NDL_webOS5_ReloadMedia(&c)==0 && calls==2 && applied==0);
    assert(loads==6 && unloads==5);
    puts("NDL load-order tests PASS (first load, reload, absent and invalid settings)");
}
