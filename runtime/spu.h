// Phase 2 SPU state: psx-spx, Sound Processing Unit (ADPCM, Pitch, Volume/ADSR, Voice Flags, Memory Access and Interrupt). Every voice and queued sample belongs to ps1
#pragma once
#include <stdint.h>

enum { SPU_VOICES = 24, SPU_AUDIO_FRAMES = 4096 };
typedef struct ps1_spu_volume { int32_t level; uint32_t counter; } ps1_spu_volume;
typedef struct ps1_spu_voice {
    int16_t decoded[31], old, older;   // three preceding samples, then the current 28-sample ADPCM block
    uint32_t addr, pitch, env_counter;
    int32_t env; int phase, loaded; uint8_t flags;
    ps1_spu_volume volume[2];
} ps1_spu_voice;
typedef struct ps1_spu {
    uint16_t reg[0x200]; uint32_t addr;
    ps1_spu_voice voice[SPU_VOICES]; ps1_spu_volume main[2];
    uint64_t next; uint32_t key_on, key_off, endx;
    uint16_t fifo[32], noise, capture; int nfifo, noise_timer, irq;
    uint32_t reverb_addr; unsigned reverb_phase, reverb_sample;
    int16_t reverb_in[2][64], reverb_up[2][64];   // 39-tap input downsampling and zero-stuffed output interpolation
    int16_t reverb_last[2];
    int16_t audio[SPU_AUDIO_FRAMES * 2]; unsigned audio_read, audio_count;
    uint8_t ram[512 << 10];
} ps1_spu;
struct ps1;
void spu_event(struct ps1 *m);   // one 44.1 kHz sample, every 768 CPU cycles
int spu_audio_read(struct ps1 *m, int16_t *out, int maxframes);   // interleaved signed stereo; call on the machine thread
