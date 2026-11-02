// Deferred host playback refills (plan.md, phase 6 follow-up): a short ring must wait for real samples while the device's other buffers keep playing. Synthetic interleaved ramps expose
// inserted silence, channel swaps, dropped/duplicated samples and buffer reordering, without Audio Queue Services, a game, a BIOS or an audio device.
#include "../audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
static int16_t sample(unsigned frame, unsigned channel) { return channel ? -(int16_t)(frame * 53 % 30000 + 1) : (int16_t)(frame * 37 % 30000 + 1); }
static void push(front_audio_state *s, unsigned first, unsigned frames) {
    int16_t *data = malloc((size_t)frames * 2 * sizeof *data);
    CHECK(data != NULL, "synthetic sample storage allocates"); if (!data) return;
    for (unsigned i = 0; i < frames; i++) { data[i * 2] = sample(first + i, 0); data[i * 2 + 1] = sample(first + i, 1); }
    front_audio_push(s, data, frames); free(data);
}
static int matches(const int16_t *out, unsigned first) {
    for (unsigned i = 0; i < FRONT_AUDIO_CHUNK; i++) if (out[i * 2] != sample(first + i, 0) || out[i * 2 + 1] != sample(first + i, 1)) return 0;
    return 1;
}
static void late_samples(void) {
    front_audio_state s = {0}; unsigned id = 99; int16_t out[FRONT_AUDIO_CHUNK * 2], old_out[FRONT_AUDIO_CHUNK * 2];
    memset(out, 0x5a, sizeof out); memcpy(old_out, out, sizeof out);
    push(&s, 0, 368); CHECK(!front_audio_return(&s, 0), "a completed device buffer becomes pending"); front_audio_state old = s;
    CHECK(!front_audio_take(&s, &id, out) && !memcmp(&s, &old, sizeof s) && id == 99 && !memcmp(out, old_out, sizeof out),
          "368 available frames do not consume state or partially overwrite a 512-frame device buffer");
    // The observed 737-frame arrival was 1.68 ms later, within the 23.22 ms held by two queued device buffers. No zero needs to be submitted in that interval.
    push(&s, 368, 737);
    CHECK(front_audio_take(&s, &id, out) && id == 0 && matches(out, 0) && s.count == 593 && !s.pending_count,
          "the late arrival completes the first contiguous real chunk and retains its remaining samples");
    old = s; id = 99; memcpy(old_out, out, sizeof out);
    CHECK(!front_audio_take(&s, &id, out) && !memcmp(&s, &old, sizeof s) && id == 99 && !memcmp(out, old_out, sizeof out),
          "a full chunk without a returned device buffer remains queued unchanged");
    CHECK(!front_audio_return(&s, 2) && front_audio_take(&s, &id, out) && id == 2 && matches(out, 512) && s.count == 81,
          "the next device buffer receives the real continuation across producer batches");
    CHECK(!front_audio_return(&s, 1), "another completed buffer can wait on a partial chunk");
    push(&s, 1105, 431);
    CHECK(front_audio_take(&s, &id, out) && id == 1 && matches(out, 1024) && !s.count, "an exact late completion contains no padding, loss or duplication");
}
static void pending_buffers(void) {
    front_audio_state s = {0}; unsigned id; int16_t out[FRONT_AUDIO_CHUNK * 2];
    push(&s, 0, FRONT_AUDIO_CHUNK * FRONT_AUDIO_BUFFERS);
    CHECK(!front_audio_return(&s, 2) && !front_audio_return(&s, 0) && !front_audio_return(&s, 1), "all three returned buffers retain callback order");
    front_audio_state old = s;
    CHECK(front_audio_return(&s, 1) == -1 && front_audio_return(&s, FRONT_AUDIO_BUFFERS) == -1 && front_audio_return(&s, (unsigned)-1) == -1 &&
          !memcmp(&s, &old, sizeof s), "duplicate and invalid buffer IDs leave samples and pending order unchanged");
    const unsigned order[] = {2, 0, 1};
    for (unsigned i = 0; i < FRONT_AUDIO_BUFFERS; i++)
        CHECK(front_audio_take(&s, &id, out) && id == order[i] && matches(out, i * FRONT_AUDIO_CHUNK), "pending buffers are filled in FIFO order with successive chunks");
    CHECK(!s.pending_count && !s.pending_mask && !s.count, "taking all pending buffers clears ownership and consumes exactly their samples");
    for (unsigned i = 0; i < 40; i++) {
        push(&s, i * FRONT_AUDIO_CHUNK, FRONT_AUDIO_CHUNK); unsigned returned = (i + 2) % FRONT_AUDIO_BUFFERS;
        CHECK(!front_audio_return(&s, returned) && front_audio_take(&s, &id, out) && id == returned && matches(out, i * FRONT_AUDIO_CHUNK),
              "pending indices and sample reads wrap without changing ownership or stereo order");
    }
}
static void ring_bounds(void) {
    front_audio_state s = {0}; unsigned id; int16_t out[FRONT_AUDIO_CHUNK * 2];
    front_audio_push(&s, NULL, 0); CHECK(!s.read && !s.count, "an empty push does not dereference its sample pointer or change state");
    push(&s, 0, FRONT_AUDIO_CAP);
    for (unsigned i = 0; i < 15; i++) {
        CHECK(!front_audio_return(&s, i % FRONT_AUDIO_BUFFERS) && front_audio_take(&s, &id, out) && matches(out, i * FRONT_AUDIO_CHUNK), "full-ring playback advances without loss");
    }
    push(&s, FRONT_AUDIO_CAP, FRONT_AUDIO_CHUNK * 2);
    for (unsigned i = 0; i < 3; i++) {
        CHECK(!front_audio_return(&s, i) && front_audio_take(&s, &id, out) && matches(out, 7680 + i * FRONT_AUDIO_CHUNK), "a chunk crossing the ring end preserves both channels");
    }
    CHECK(!s.count && s.read == 1024, "ring read position wraps after the exact consumed frame count");
    s = (front_audio_state){0}; push(&s, 0, FRONT_AUDIO_CAP); CHECK(!front_audio_return(&s, 2), "overflow fixture has a pending device buffer");
    push(&s, FRONT_AUDIO_CAP, 700);
    CHECK(s.count == FRONT_AUDIO_CAP && s.read == 700 && s.pending_count == 1 && s.pending_mask == 4,
          "a producer overrun drops only the oldest real frames and preserves pending buffer order");
    CHECK(front_audio_take(&s, &id, out) && id == 2 && matches(out, 700), "an overrun begins playback at the oldest retained real frame");
    push(&s, FRONT_AUDIO_CAP + 700, FRONT_AUDIO_CAP + 777);
    CHECK(s.count == FRONT_AUDIO_CAP && !front_audio_return(&s, 1) && front_audio_take(&s, &id, out) && id == 1 && matches(out, FRONT_AUDIO_CAP + 1477),
          "one producer batch larger than capacity retains exactly its newest capacity frames");
    front_audio_state other = {0}; push(&other, 100, FRONT_AUDIO_CHUNK); CHECK(!front_audio_return(&other, 0), "a second playback state has independent pending ownership");
    front_audio_state old = s;
    CHECK(front_audio_take(&other, &id, out) && id == 0 && matches(out, 100) && !memcmp(&s, &old, sizeof s), "playing a second instance leaves the first instance byte-identical");
}
typedef struct playback {
    front_audio_state audio; unsigned queued[FRONT_AUDIO_BUFFERS], read, count, mask, emitted;
} playback;
static void refill(playback *p) {
    unsigned id; int16_t out[FRONT_AUDIO_CHUNK * 2];
    while (front_audio_take(&p->audio, &id, out)) {
        CHECK(p->count < FRONT_AUDIO_BUFFERS && id < FRONT_AUDIO_BUFFERS && !(p->mask & (1u << id)), "a deferred refill owns one available physical buffer");
        CHECK(matches(out, p->emitted), "synthetic playback submits only the next complete real stereo chunk"); p->emitted += FRONT_AUDIO_CHUNK;
        p->queued[(p->read + p->count++) % FRONT_AUDIO_BUFFERS] = id; p->mask |= 1u << id;
    }
}
static void cadence(void) {
    playback p = {0}; push(&p.audio, 0, FRONT_AUDIO_BUFFERS * FRONT_AUDIO_CHUNK);
    for (unsigned i = 0; i < FRONT_AUDIO_BUFFERS; i++) CHECK(!front_audio_return(&p.audio, i), "startup registers its existing three device buffers");
    refill(&p);
    // 737-frame producer bursts every 16.712 ms meet 512-frame device callbacks every 11.610 ms. Deferring a short refill uses already queued audio, with no
    // additional playback buffers, larger ring or inserted silence. Six hundred callbacks exercise every phase between the two independent cadences.
    unsigned producer = 16712, callback = 11610, input = p.emitted, completed = 0, deferred = 0, max_pending = 0;
    while (completed < 600) {
        if (producer <= callback) { push(&p.audio, input, 737); input += 737; producer += 16712; }
        else {
            CHECK(p.count != 0, "the device has real queued audio when its next chunk completes"); if (!p.count) break;
            unsigned id = p.queued[p.read]; p.read = (p.read + 1) % FRONT_AUDIO_BUFFERS; p.count--; p.mask &= ~(1u << id);
            CHECK(!front_audio_return(&p.audio, id), "a device callback returns each buffer exactly once");
            if (p.audio.count < FRONT_AUDIO_CHUNK) deferred++;
            if (p.audio.pending_count > max_pending) max_pending = p.audio.pending_count;
            callback += 11610; completed++;
        }
        refill(&p); CHECK(p.count != 0, "late producer bursts arrive before the device exhausts its previously queued chunks");
    }
    CHECK(completed == 600 && deferred > 100 && max_pending == 2, "the cadence exercises repeated low-watermark deferrals with two pending refills");
    CHECK(input == p.emitted + p.audio.count, "the full cadence accounts for every input frame without silence, duplication or a producer overrun");
}
int main(void) {
    late_samples(); pending_buffers(); ring_bounds(); cadence();
    printf("audio: %d checks, %d failed\n", checks, fails); return fails != 0;
}
