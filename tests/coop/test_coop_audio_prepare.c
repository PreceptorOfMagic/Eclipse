/* Actual session audio startup/cleanup with mocked output and mixer lifetimes.
 * No output device, worker thread, connection, sentinel or live log is opened. */
#ifdef NDEBUG
#error "Startup contract assertions must be enabled"
#endif
#include <assert.h>
#include <unistd.h>
static int no_probe_access(const char *path, int mode) { (void)path; (void)mode; return -1; }
#define access no_probe_access
#include "../../src/app/stream/audio/session_audio.c"
#undef access

static bool advertised=true, clock_ok=true, worker_ok=true, device_open, worker_live, coord_live;
static bool pull_advertised, pull_used;
static unsigned opened, closed, coord_started, worker_started, stopped;
static unsigned mock_rate=48000;
static SS4S_AudioOpenResult open_result=SS4S_AUDIO_OPEN_OK;

void commons_log_printf(commons_log_level level,const char *tag,const char *fmt,...) {
    (void)level;(void)tag;(void)fmt;
}
bool app_launch_coop_requested(void) { return true; }
bool SS4S_AudioHasClock(void) { return advertised; }
bool SS4S_AudioHasPull(void) { return pull_advertised; }
SS4S_AudioCodec SS4S_GetAudioPreferredCodecs(const SS4S_AudioInfo *info) {
    (void)info;return SS4S_AUDIO_PCM_S16LE;
}
SS4S_AudioOpenResult SS4S_PlayerAudioOpen(SS4S_Player *target,const SS4S_AudioInfo *info) {
    assert(target && !device_open);
    assert(info->codec==SS4S_AUDIO_PCM_S16LE && info->sampleRate==48000 && info->numOfChannels==2);
    ++opened;
    device_open=open_result==SS4S_AUDIO_OPEN_OK;
    return open_result;
}
bool SS4S_PlayerAudioGetClock(SS4S_Player *target,SS4S_AudioClock *clock) {
    assert(target && device_open);
    *clock=(SS4S_AudioClock){.sample_rate=mock_rate,.quantum_frames=240,.generation=1};
    return clock_ok;
}
bool SS4S_PlayerAudioClose(SS4S_Player *target) {
    assert(target);
    if(!device_open)return false;
    assert(!worker_live && !coord_live);
    device_open=false;++closed;return true;
}
SS4S_Player *SS4S_PlayerOpen(void) { assert(!"probe must remain disabled");return NULL; }
void SS4S_PlayerClose(SS4S_Player *target) { (void)target;assert(!device_open && !worker_live); }
SS4S_AudioFeedResult SS4S_PlayerAudioFeed(SS4S_Player *target,const unsigned char *pcm,size_t bytes) {
    (void)target;(void)pcm;(void)bytes;assert(!"no live feed expected");return SS4S_AUDIO_FEED_ERROR;
}
void coop_audio_coord_start(int rate,int channels) {
    assert((device_open || pull_advertised) && rate==48000 && channels==2);coord_live=true;++coord_started;
}
bool coop_audio_output_start(SS4S_Player *target) {
    assert(target && device_open && coord_live);++worker_started;
    worker_live=worker_ok;return worker_ok;
}
bool coop_audio_output_open_pull(SS4S_Player *target,const SS4S_AudioInfo *info) {
    assert(pull_advertised && coord_live && !device_open);
    pull_used=true;
    if(SS4S_PlayerAudioOpen(target,info)!=SS4S_AUDIO_OPEN_OK)return false;
    worker_live=worker_ok; ++worker_started;
    return worker_ok;
}
void coop_audio_coord_stop(void) {
    worker_live=false;coord_live=false;++stopped;
    if(pull_used && device_open)SS4S_PlayerAudioClose(player ? player : (SS4S_Player*)&device_open);
}
bool coop_audio_coord_active(void) { return coord_live; }
void coop_audio_mix(int16_t *pcm,int frames) { (void)pcm;(void)frames;assert(!"no legacy pump expected"); }
bool coop_audio_output_submit(const int16_t *pcm,unsigned frames,uint64_t index,uint64_t ready,bool plc) {
    (void)pcm;(void)frames;(void)index;(void)ready;(void)plc;assert(!"no decode feed expected");return false;
}

static void reset_mocks(void) {
    assert(!player && !output_prepared && !device_paced && !decoder && !buffer);
    assert(!device_open && !worker_live && !coord_live);
    advertised=clock_ok=worker_ok=true;mock_rate=48000;open_result=SS4S_AUDIO_OPEN_OK;
    pull_advertised=pull_used=false;
    opened=closed=coord_started=worker_started=stopped=0;
}
int main(void) {
    session_t context={0};
    context.player=(SS4S_Player *)&context;
    context.config.stream.audioConfiguration=AUDIO_CONFIGURATION_STEREO;
    OPUS_MULTISTREAM_CONFIGURATION opus={.sampleRate=48000,.channelCount=2,
        .streams=1,.coupledStreams=1,.samplesPerFrame=240,.mapping={0,1}};
    reset_mocks();advertised=false;
    assert(!session_audio_prepare_coop_output(&context) && opened==0);
    reset_mocks();open_result=SS4S_AUDIO_OPEN_ERROR;
    assert(!session_audio_prepare_coop_output(&context) && opened==1 && closed==0);
    reset_mocks();clock_ok=false;
    assert(!session_audio_prepare_coop_output(&context) && closed==1 && coord_started==0);
    reset_mocks();mock_rate=44100;
    assert(!session_audio_prepare_coop_output(&context) && closed==1 && coord_started==0);
    reset_mocks();worker_ok=false;
    assert(!session_audio_prepare_coop_output(&context) && stopped==1 && closed==1);
    reset_mocks();
    assert(session_audio_prepare_coop_output(&context) && device_paced && output_prepared);
    session_audio_finish_output(context.player); /* Connection/video failure before aud_init. */
    assert(stopped==1 && closed==1);
    session_audio_finish_output(context.player);assert(closed==1);
    reset_mocks();
    assert(session_audio_prepare_coop_output(&context));
    assert(aud_init(AUDIO_CONFIGURATION_STEREO,&opus,&context,0)==SS4S_AUDIO_OPEN_OK);
    assert(opened==1 && coord_started==1 && worker_started==1 && decoder && buffer);
    aud_cleanup(); /* Normal upstream cleanup before outer player destruction. */
    session_audio_finish_output(context.player);assert(closed==1 && stopped==1);
    reset_mocks();
    pull_advertised=true; advertised=false;
    assert(session_audio_prepare_coop_output(&context));
    assert(pull_used && opened==1 && coord_started==1 && worker_started==1);
    session_audio_finish_output(context.player);
    assert(closed==1 && stopped==1);
    session_audio_finish_output(context.player);assert(closed==1);
    reset_mocks();pull_advertised=true;open_result=SS4S_AUDIO_OPEN_ERROR;
    assert(!session_audio_prepare_coop_output(&context));
    assert(opened==1 && stopped==1 && closed==0);
    reset_mocks();pull_advertised=true;worker_ok=false;
    assert(!session_audio_prepare_coop_output(&context));
    assert(opened==1 && stopped==1 && closed==1);
    reset_mocks();pull_advertised=true;
    assert(session_audio_prepare_coop_output(&context));
    opus.streams=1;opus.coupledStreams=1;
    assert(aud_init(AUDIO_CONFIGURATION_STEREO,&opus,&context,0)==SS4S_AUDIO_OPEN_OK);
    assert(opened==1 && decoder && buffer);
    aud_cleanup();assert(closed==1 && stopped==1);
    reset_mocks();
    assert(session_audio_prepare_coop_output(&context));
    opus.sampleRate=44100;
    assert(aud_init(AUDIO_CONFIGURATION_STEREO,&opus,&context,0)==SS4S_AUDIO_OPEN_UNSUPPORTED_CODEC);
    session_audio_finish_output(context.player);assert(closed==1 && stopped==1);
    reset_mocks();
    assert(session_audio_prepare_coop_output(&context));
    opus.sampleRate=48000;opus.streams=0; /* Actual decoder creation fails. */
    assert(aud_init(AUDIO_CONFIGURATION_STEREO,&opus,&context,0)!=0);
    session_audio_finish_output(context.player);assert(closed==1 && stopped==1);
    reset_mocks();
    puts("PASS prepare: unsupported/open/clock/worker failures, pre-init cleanup, decoder reuse, format/decoder failure cleanup, stop-before-close, idempotent finish");
    return 0;
}
