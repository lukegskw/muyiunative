// Exercise the production Mac playback adapter with a deterministic device: a state-file stall advances the device clock without producing console samples.
// Only AudioQueue calls are replaced. Dispatch, locking, the sample ring, callbacks and transport/reset code are the real adapter; no audio device is opened.
#import <AudioToolbox/AudioToolbox.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
typedef struct device {
    AudioQueueBufferRef buffers[3], queued[3]; unsigned allocated, read, count, mask, callbacks, empty, silence, real;
    int running, paused, disposed;
} device;
static device devices[8]; static unsigned ndevices, pauses, starts, disposals;
static AudioQueueOutputCallbackBlock callbacks[8]; static dispatch_queue_t deliveries[8];
static int pause_error, start_error;
static device *dev(AudioQueueRef q) { return (device *)q; }
static OSStatus mock_new(AudioQueueRef *out, const AudioStreamBasicDescription *format, UInt32 flags, dispatch_queue_t delivery, AudioQueueOutputCallbackBlock callback) {
    CHECK(ndevices < 8 && format->mSampleRate == 44100 && format->mBytesPerFrame == 4 && !flags, "the production adapter requests interleaved 44.1 kHz stereo");
    callbacks[ndevices] = callback; deliveries[ndevices] = delivery; *out = (AudioQueueRef)&devices[ndevices++]; return 0;
}
static OSStatus mock_allocate(AudioQueueRef q, UInt32 bytes, AudioQueueBufferRef *out) {
    device *d = dev(q); CHECK(d->allocated < 3 && bytes == 2048, "playback retains three 512-frame buffers");
    AudioQueueBuffer initial = {.mAudioDataBytesCapacity = bytes, .mAudioData = calloc(1, bytes)};
    *out = malloc(sizeof **out); memcpy(*out, &initial, sizeof initial); d->buffers[d->allocated++] = *out; return 0;
}
static OSStatus mock_enqueue(AudioQueueRef q, AudioQueueBufferRef b, UInt32 count, const AudioStreamPacketDescription *packets) {
    device *d = dev(q); unsigned id = (unsigned)(uintptr_t)b->mUserData;
    CHECK(!d->disposed && d->count < 3 && !(d->mask & (1u << id)) && !count && !packets, "each enqueue owns a live, available buffer");
    d->queued[(d->read + d->count++) % 3] = b; d->mask |= 1u << id; return 0;
}
static OSStatus mock_pause(AudioQueueRef q) { pauses++; if (pause_error) return pause_error; dev(q)->paused = 1; return 0; }
static OSStatus mock_start(AudioQueueRef q, const AudioTimeStamp *time) {
    (void)time; starts++; if (start_error) return start_error; dev(q)->running = 1; dev(q)->paused = 0; return 0;
}
static OSStatus mock_dispose(AudioQueueRef q, Boolean immediate) {
    device *d = dev(q); unsigned index = (unsigned)(d - devices); disposals++;
    CHECK(immediate, "reset waits for the previous timeline to finish");
    // Disposal may finish an old callback. It must see the detached queue, including when new buffers will reuse the same numerical IDs.
    dispatch_sync(deliveries[index], ^{ callbacks[index](q, d->buffers[0]); });
    for (unsigned i = 0; i < d->allocated; i++) { free(d->buffers[i]->mAudioData); free(d->buffers[i]); }
    d->disposed = 1; return 0;
}
#define AudioQueueNewOutputWithDispatchQueue mock_new
#define AudioQueueAllocateBuffer mock_allocate
#define AudioQueueEnqueueBuffer mock_enqueue
#define AudioQueuePause mock_pause
#define AudioQueueStart mock_start
#define AudioQueueDispose mock_dispose
#include "../audio_mac.m"

static int16_t sample(unsigned frame, unsigned channel) { return channel ? -(int16_t)(frame % 30000 + 1) : (int16_t)(frame % 30000 + 1); }
static void produce(unsigned first, unsigned count) {
    int16_t data[FRONT_AUDIO_CAP * 2]; CHECK(count <= FRONT_AUDIO_CAP, "the synthetic producer fits the existing ring");
    for (unsigned i = 0; i < count; i++) { data[i * 2] = sample(first + i, 0); data[i * 2 + 1] = sample(first + i, 1); }
    front_audio(data, (int)count); dispatch_sync(g_audio_delivery, ^{});
}
static void advance(unsigned chunks) {
    dispatch_sync(g_audio_delivery, ^{
        device *d = dev(g_audio_queue);
        for (unsigned n = 0; n < chunks; n++) {
            if (!d->running || d->paused) continue;
            if (!d->count) { d->empty++; continue; }
            AudioQueueBufferRef b = d->queued[d->read]; d->read = (d->read + 1) % 3; d->count--; d->mask &= ~(1u << (unsigned)(uintptr_t)b->mUserData);
            const int16_t *data = b->mAudioData; int same = 1;
            for (unsigned i = 0; i < FRONT_AUDIO_CHUNK; i++) for (unsigned c = 0; c < 2; c++)
                if (data[i * 2 + c] != (d->silence < 3 ? 0 : sample(d->real + i, c))) same = 0;
            CHECK(same, "playback preserves every stereo sample in order, with silence only in the original startup buffers");
            if (d->silence < 3) d->silence++; else d->real += FRONT_AUDIO_CHUNK;
            d->callbacks++; callbacks[d - devices](g_audio_queue, b);
        }
    });
}
int main(void) {
    front_audio_start(); device *d = dev(g_audio_queue); CHECK(starts == 1 && d->count == 3, "startup primes and starts its original buffers");
    unsigned input = 2048; produce(0, input); advance(3);
    for (unsigned operation = 0; operation < 3; operation++) {
        front_audio_state before = g_audio_state; unsigned played = d->real, returned = d->callbacks, count = d->count;
        CHECK(!front_audio_pause(1) && g_audio_paused && d->paused, "state operations suspend the actual device transport");
        unsigned calls = pauses; CHECK(!front_audio_pause(1) && pauses == calls, "repeated suspension does not alter the transport twice");
        advance(200);   // >2 seconds of device time while hashing/writing/fsync produces no new samples
        CHECK(!d->empty && d->real == played && d->callbacks == returned && d->count == count && !memcmp(&before, &g_audio_state, sizeof before),
              "a long state operation consumes neither queued audio nor ring samples and cannot starve the device");
        CHECK(!front_audio_pause(0) && !g_audio_paused && !d->paused, "success or a file/GPU failure resumes the preserved timeline");
        for (unsigned i = 0; i < 8; i++) { produce(input, 512); input += 512; advance(1); }
        CHECK(!d->empty && input == d->real + d->count * 512 + g_audio_state.count, "normal cadence after saving retains all produced samples without gaps or drops");
    }
    CHECK(!front_audio_pause(1), "a device may finish prefetched callbacks during suspension");
    dispatch_sync(g_audio_delivery, ^{
        while (d->count) {
            AudioQueueBufferRef b = d->queued[d->read]; d->read = (d->read + 1) % 3; d->count--; d->mask &= ~(1u << (unsigned)(uintptr_t)b->mUserData);
            callbacks[d - devices](g_audio_queue, b);
        }
    });
    CHECK(g_audio_state.pending_count == 3 && !g_audio_state.count, "prefetched callbacks can leave all physical buffers waiting for fresh samples");
    unsigned started = starts;
    CHECK(!front_audio_pause(0) && g_audio_resume && d->paused && starts == started, "finishing a state operation defers device restart until its existing buffers refill");
    advance(200); produce(input, 100);
    CHECK(d->paused && starts == started && g_audio_state.count == 100, "a partial real chunk cannot restart an empty output queue");
    produce(input + 100, 1436);
    CHECK(!g_audio_resume && !d->paused && d->count == 3 && starts == started + 1, "three full real buffers resume playback exactly once after a prefetch drain");
    int same = 1;
    for (unsigned n = 0; n < 3; n++) {
        const int16_t *data = d->queued[(d->read + n) % 3]->mAudioData;
        for (unsigned i = 0; i < 512; i++) for (unsigned c = 0; c < 2; c++) if (data[i * 2 + c] != sample(input + n * 512 + i, c)) same = 0;
    }
    CHECK(same && !g_audio_state.count, "repriming keeps FIFO order and uses no padding, dropped samples or larger buffers");
    pause_error = -50; CHECK(front_audio_pause(1) < 0 && !g_audio_paused && !d->paused, "failed suspension leaves playback and transport state unchanged"); pause_error = 0;
    CHECK(!front_audio_pause(1), "transport remains usable after a pause error");
    start_error = -50; CHECK(front_audio_pause(0) < 0 && g_audio_paused && d->paused, "a failed resume is reported without pretending playback restarted"); start_error = 0;
    CHECK(!front_audio_pause(0), "a reported start error can be retried without resetting samples");
    CHECK(!front_audio_pause(1), "loading begins with playback paused"); started = starts;
    front_audio_reset(); device *fresh = dev(g_audio_queue);
    CHECK(disposals == 1 && d->disposed && fresh != d && !fresh->running && fresh->count == 3 && starts == started && g_audio_paused && !g_audio_state.count && !g_audio_state.pending_count,
          "a successful load discards old audio and primes its new queue without starting before the state operation finishes");
    front_audio_state before = g_audio_state; audio_buffer((AudioQueueRef)d, NULL);
    CHECK(!memcmp(&before, &g_audio_state, sizeof before), "a stale callback cannot change the new timeline");
    CHECK(!front_audio_pause(0) && fresh->running && starts == started + 1, "finishing a load starts its fresh queue exactly once");
    produce(0, 2048); advance(4); CHECK(!fresh->empty && fresh->real == 512, "fresh playback after loading delivers the new sample sequence");
    AudioQueueRef last = g_audio_queue; dispatch_sync(g_audio_delivery, ^{ g_audio_queue = NULL; }); mock_dispose(last, true);
    CHECK(!front_audio_pause(1) && !front_audio_pause(0), "state operations still work if audio could not initialize");
    printf("audio output: %d checks, %d failed\n", checks, fails); return fails != 0;
}
