// Host playback policy, not console state. A live callback had 368 of 512 frames and inserted 144 zeros; 737 real frames arrived 1.68 ms later while two other
// buffers were still queued. Keep returned buffer IDs pending until a complete real chunk is available instead of inserting silence into buffered playback.
// The adapter serializes access; this plain-C state owns no OS resources and needs no audio device for t_audio. Zero initialization resets it.
#pragma once
#include <stdint.h>

enum { FRONT_AUDIO_CAP = 8192, FRONT_AUDIO_CHUNK = 512, FRONT_AUDIO_BUFFERS = 3 };
typedef struct front_audio_state {
    int16_t ring[FRONT_AUDIO_CAP * 2];
    unsigned read, count;
    unsigned pending[FRONT_AUDIO_BUFFERS], pending_read, pending_count, pending_mask;
} front_audio_state;

static inline void front_audio_push(front_audio_state *s, const int16_t *samples, unsigned frames) {
    for (unsigned i = 0; i < frames; i++) {
        if (s->count == FRONT_AUDIO_CAP) { s->read = (s->read + 1) % FRONT_AUDIO_CAP; s->count--; }
        unsigned at = (s->read + s->count++) % FRONT_AUDIO_CAP;
        s->ring[at * 2] = samples[i * 2]; s->ring[at * 2 + 1] = samples[i * 2 + 1];
    }
}

static inline int front_audio_return(front_audio_state *s, unsigned id) {
    if (id >= FRONT_AUDIO_BUFFERS || (s->pending_mask & (1u << id))) return -1;
    s->pending[(s->pending_read + s->pending_count++) % FRONT_AUDIO_BUFFERS] = id;
    s->pending_mask |= 1u << id; return 0;
}

static inline int front_audio_take(front_audio_state *s, unsigned *id, int16_t out[FRONT_AUDIO_CHUNK * 2]) {
    if (!s->pending_count || s->count < FRONT_AUDIO_CHUNK) return 0;
    for (unsigned i = 0; i < FRONT_AUDIO_CHUNK; i++) {
        unsigned at = (s->read + i) % FRONT_AUDIO_CAP;
        out[i * 2] = s->ring[at * 2]; out[i * 2 + 1] = s->ring[at * 2 + 1];
    }
    s->read = (s->read + FRONT_AUDIO_CHUNK) % FRONT_AUDIO_CAP; s->count -= FRONT_AUDIO_CHUNK;
    *id = s->pending[s->pending_read]; s->pending_read = (s->pending_read + 1) % FRONT_AUDIO_BUFFERS;
    s->pending_count--; s->pending_mask &= ~(1u << *id); return 1;
}
