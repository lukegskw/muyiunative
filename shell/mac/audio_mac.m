// Audio Queue adapter for the Mac front end. The machine produces samples; one serial delivery queue owns callbacks and enqueues. Tests substitute only the
// AudioQueue entry points, so transport, buffer ownership and sample order can be checked without an audio device. The console SPU state is never shared here.
#import <AudioToolbox/AudioToolbox.h>
#include <dispatch/dispatch.h>
#include <os/lock.h>
#include <stdatomic.h>
#include <string.h>
#include "front.h"
#include "audio.h"
#include "ps1.h"

// The machine writes the bounded sample ring; one serial delivery queue owns returned buffers and every enqueue, preserving sample order without blocking a callback.
enum { AUDIO_CHUNK = FRONT_AUDIO_CHUNK, AUDIO_BUFFERS = FRONT_AUDIO_BUFFERS };
static os_unfair_lock g_audio_lock = OS_UNFAIR_LOCK_INIT;
static front_audio_state g_audio_state;
static AudioQueueRef g_audio_queue;
static AudioQueueBufferRef g_audio_buffers[AUDIO_BUFFERS];
static dispatch_queue_t g_audio_delivery;
static atomic_bool g_audio_failed;
static int g_audio_paused, g_audio_resume;   // delivery queue owns transport; callbacks may still return/refill buffers while paused
static int audio_service(void) {
    if (!g_audio_queue) return 0;
    for (;;) {
        unsigned id; int16_t samples[AUDIO_CHUNK * 2];
        os_unfair_lock_lock(&g_audio_lock);
        int ready = front_audio_take(&g_audio_state, &id, samples);
        os_unfair_lock_unlock(&g_audio_lock);
        if (!ready) break;
        AudioQueueBufferRef b = g_audio_buffers[id];
        memcpy(b->mAudioData, samples, sizeof samples); b->mAudioDataByteSize = sizeof samples;
        OSStatus err = AudioQueueEnqueueBuffer(g_audio_queue, b, 0, NULL);
        if (err) { if (!atomic_exchange(&g_audio_failed, true)) RED("front end: cannot enqueue audio (%d)", (int)err); return -1; }
    }
    os_unfair_lock_lock(&g_audio_lock); int full = !g_audio_state.pending_count; os_unfair_lock_unlock(&g_audio_lock);
    // Pause can finish callbacks for prefetched buffers. Starting before those buffers refill recreates the empty reserve; wait for the original three buffers
    // to hold real samples. The producer keeps running after the state operation, so a partial chunk is completed normally, without silence or extra buffers.
    if (g_audio_resume && full) {
        g_audio_resume = 0; OSStatus err = AudioQueueStart(g_audio_queue, NULL);
        if (err) { g_audio_paused = 1; RED("front end: cannot resume audio for a state operation (%d)", (int)err); return -1; }
    }
    return 0;
}
void front_audio(const int16_t *samples, int frames) {
    if (!g_audio_delivery || frames <= 0) return;
    os_unfair_lock_lock(&g_audio_lock);
    front_audio_push(&g_audio_state, samples, (unsigned)frames);
    os_unfair_lock_unlock(&g_audio_lock);
    dispatch_async(g_audio_delivery, ^{ audio_service(); });
}
static void audio_buffer(AudioQueueRef q, AudioQueueBufferRef b) {
    if (!g_audio_queue || q != g_audio_queue) return;
    // The results callback padded 144 zero samples although real data arrived 1.68 ms later and two buffers were still queued. A returned buffer belongs to us
    // (Apple, AudioQueueOutputCallbackBlock); retain it until a complete real block is available, then enqueue from this same serial queue without blocking callbacks.
    os_unfair_lock_lock(&g_audio_lock);
    int err = front_audio_return(&g_audio_state, (unsigned)(uintptr_t)b->mUserData);
    os_unfair_lock_unlock(&g_audio_lock);
    if (err) { if (!atomic_exchange(&g_audio_failed, true)) RED("front end: invalid returned audio buffer"); return; }
    audio_service();
}
void front_audio_start(void) {
    dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0);
    g_audio_delivery = dispatch_queue_create("org.muyiunative.audio", attr);
    if (!g_audio_delivery) { RED("front end: cannot create audio delivery queue"); return; }
    __block OSStatus err = 0;
    dispatch_sync(g_audio_delivery, ^{
        AudioStreamBasicDescription format = {.mSampleRate = 44100, .mFormatID = kAudioFormatLinearPCM,
            .mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked, .mBytesPerPacket = 4, .mFramesPerPacket = 1,
            .mBytesPerFrame = 4, .mChannelsPerFrame = 2, .mBitsPerChannel = 16};
        err = AudioQueueNewOutputWithDispatchQueue(&g_audio_queue, &format, 0, g_audio_delivery,
            ^(AudioQueueRef q, AudioQueueBufferRef b) { audio_buffer(q, b); });
        for (unsigned i = 0; !err && i < AUDIO_BUFFERS; i++) {
            AudioQueueBufferRef b; err = AudioQueueAllocateBuffer(g_audio_queue, AUDIO_CHUNK * 4, &b);
            if (!err) {
                g_audio_buffers[i] = b; b->mUserData = (void *)(uintptr_t)i;
                memset(b->mAudioData, 0, AUDIO_CHUNK * 4); b->mAudioDataByteSize = AUDIO_CHUNK * 4;
                err = AudioQueueEnqueueBuffer(g_audio_queue, b, 0, NULL);   // the same three silent startup buffers, before the machine begins
            }
        }
    });
    if (!err && !g_audio_paused) err = AudioQueueStart(g_audio_queue, NULL);
    if (err) {
        RED("front end: cannot start audio (%d)", (int)err);
        __block AudioQueueRef failed;
        dispatch_sync(g_audio_delivery, ^{ failed = g_audio_queue; g_audio_queue = NULL; });
        if (failed) AudioQueueDispose(failed, true);   // let the callback queue drain while disposal waits for outstanding callbacks
    }
}

// FF IX save took 59 ms: all three buffers returned while snapshot hashing/fsync blocked the sample producer. Rebased pacing then resumed with no playback
// reserve. Pause the device along with simulation, preserving buffers and the ring (Apple AudioQueuePause: buffers are unaffected; AudioQueueStart resumes).
// Serialize transport with refills. A successful load creates its fresh queue paused too; a save or failed load keeps the original sample sequence.
int front_audio_pause(int paused) {
    paused = paused != 0;
    if (!g_audio_delivery) { g_audio_paused = paused; return 0; }
    __block int result = 0;
    dispatch_sync(g_audio_delivery, ^{
        if (paused == g_audio_paused) return;
        if (paused) {
            OSStatus err = g_audio_queue ? AudioQueuePause(g_audio_queue) : 0;
            if (err) { RED("front end: cannot pause audio for a state operation (%d)", (int)err); result = -1; return; }
            g_audio_paused = 1; g_audio_resume = 0;
        } else {
            g_audio_paused = 0; g_audio_resume = g_audio_queue != NULL; result = audio_service();
        }
    });
    return result;
}

void front_audio_reset(void) {
    // Old callbacks must finish before their buffer IDs or queue can be reused. Disposing outside the delivery queue avoids waiting for a callback on itself.
    __block AudioQueueRef previous = NULL;
    if (g_audio_delivery) dispatch_sync(g_audio_delivery, ^{ previous = g_audio_queue; g_audio_queue = NULL; g_audio_resume = 0; });
    if (previous) {
        OSStatus err = AudioQueueDispose(previous, true);
        if (err) { RED("front end: cannot dispose playback before state load (%d)", (int)err); return; }
    }
    os_unfair_lock_lock(&g_audio_lock); memset(&g_audio_state, 0, sizeof g_audio_state); os_unfair_lock_unlock(&g_audio_lock);
    memset(g_audio_buffers, 0, sizeof g_audio_buffers); atomic_store(&g_audio_failed, false); front_audio_start();
}
