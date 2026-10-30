// Phase 2 SPU checks from synthetic ADPCM built in RAM: psx-spx, SPU ADPCM Samples/Pitch, Volume/ADSR Generator, Voice Flags, Noise, Memory Access and Interrupt.
// Integer expected values below are calculated directly from the documented predictor/gaussian coefficients and envelope rates; no game data or audio device is needed
#include "ps1.h"
#include <stdlib.h>
#include <string.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
enum { CNT = 0x1aa, ADDR = 0x1a6, DATA = 0x1a8, STAT = 0x1ae, KON = 0x188, KOFF = 0x18c };
static void tick(ps1 *m, int n) { while (n--) { m->cycles = m->spu.next; sched_run(m); } }
static void block(ps1 *m, uint32_t addr, uint8_t header, uint8_t flags, uint8_t data) {
    spu_write(m, ADDR, (uint16_t)(addr / 8));
    spu_dma_write(m, header | (uint32_t)flags << 8 | (uint32_t)data << 16 | (uint32_t)data << 24);
    for (int i = 0; i < 3; i++) spu_dma_write(m, (uint32_t)data * 0x01010101u);
}
static void voice(ps1 *m, int n, uint32_t addr, uint16_t pitch) {
    spu_write(m, n * 16, 0x3fff); spu_write(m, n * 16 + 2, 0x4000);
    spu_write(m, n * 16 + 4, pitch); spu_write(m, n * 16 + 6, (uint16_t)(addr / 8));
    spu_write(m, n * 16 + 8, 0x000f); spu_write(m, n * 16 + 10, 0x1fc0);
    spu_write(m, KON + (n >= 16 ? 2 : 0), (uint16_t)(1u << (n & 15)));
}
static void init(ps1 *m) {
    ps1_reset(m); memset(m->spu.ram, 0, sizeof m->spu.ram);
    spu_write(m, 0x1ac, 4); spu_write(m, 0x180, 0x3fff); spu_write(m, 0x182, 0x3fff); spu_write(m, CNT, 0xc000);
}
static void word(ps1 *m, unsigned addr, int16_t value) { memcpy(m->spu.ram + addr, &value, 2); }
static int16_t read_word(ps1 *m, unsigned addr) { int16_t value; memcpy(&value, m->spu.ram + addr, 2); return value; }
static void reverb_checks(ps1 *m) {
    enum { BASE = 0x78000 }; int16_t out[80];
    init(m); spu_write(m, 0x1a2, BASE / 8);
    spu_write(m, 0x1c4, 0x4000); spu_write(m, 0x1ce, 0x4000);
    spu_write(m, 0x1d4, 10); spu_write(m, 0x1d6, 11); spu_write(m, 0x1e0, 12); spu_write(m, 0x1e2, 13);
    spu_write(m, 0x1e4, 0x20); spu_write(m, 0x1e6, 0x21); spu_write(m, 0x1f0, 0x23); spu_write(m, 0x1f2, 0x22);
    word(m, BASE + 10 * 8 - 2, 2000); word(m, BASE + 12 * 8, 10000);
    word(m, BASE + 0x20 * 8 - 2, 1000); word(m, BASE + 0x22 * 8, 6000);
    spu_write(m, 0x1a4, BASE / 8 + 10); spu_write(m, CNT, 0xc0c0); tick(m, 1);
    CHECK(read_word(m, BASE + 10 * 8) == 3500, "reverb same-side reflection applies signed wall and IIR products plus previous sample");
    CHECK(read_word(m, BASE + 0x20 * 8) == 2000, "reverb different-side reflection reads the opposite side's delay register");
    CHECK(!read_word(m, BASE + 11 * 8) && m->spu.reverb_addr == BASE, "reverb left and right work alternate at44100Hz");
    CHECK((m->i_stat & 1u << IRQ_SPU) && (spu_read(m, STAT) & 0x40), "reverb buffer access raises the sound RAM address interrupt");
    tick(m, 1); CHECK(m->spu.reverb_addr == BASE + 2, "reverb pointer advances one halfword per22050Hz stereo cycle");
    spu_write(m, 0x1a2, 0xffff); tick(m, 2); tick(m, 6);
    CHECK(m->spu.reverb_addr == SPU_RAM_SIZE - 8, "reverb current address wraps to ESA at the end of sound RAM");

    init(m); spu_write(m, 0x1a2, BASE / 8); spu_write(m, 0x1c4, 0x8000); spu_write(m, 0x1ce, 0x4000);
    spu_write(m, 0x1d4, 10); spu_write(m, 0x1e0, 12);
    word(m, BASE + 10 * 8 - 2, 2000); word(m, BASE + 12 * 8, 10000); spu_write(m, CNT, 0xc080); tick(m, 1);
    CHECK(read_word(m, BASE + 10 * 8) == 1000, "vIIR8000 hardware quirk negates the whole reflection result");

    init(m); spu_write(m, 0x1a2, BASE / 8);
    spu_write(m, 0x1c0, 1); spu_write(m, 0x1c2, 1);
    spu_write(m, 0x1f4, 0x10); spu_write(m, 0x1f8, 0x20);
    spu_write(m, 0x1d0, 0x4000); spu_write(m, 0x1d2, 0x4000);
    spu_write(m, 0x1c6, 0x4000); spu_write(m, 0x1d8, 0x30);
    word(m, BASE + 0x30 * 8, 10000); word(m, BASE + 0x0f * 8, 2000); word(m, BASE + 0x1f * 8, 3000);
    spu_write(m, CNT, 0xc080); tick(m, 1);
    CHECK(read_word(m, BASE + 0x10 * 8) == 4000 && read_word(m, BASE + 0x20 * 8) == 2500 && m->spu.reverb_last[0] == 4250, "comb and both all-pass stages apply their delayed signed feedback in sequence");
    init(m); spu_write(m, 0x1a2, BASE / 8); spu_write(m, 0x1c0, 2); spu_write(m, 0x1f4, 1);
    spu_write(m, 0x1c2, 1); spu_write(m, 0x1f8, 0x40); spu_write(m, 0x1d2, 0x4000);
    word(m, SPU_RAM_SIZE - 8, 12345); spu_write(m, CNT, 0xc080); tick(m, 1);
    CHECK(m->spu.reverb_last[0] == 6172 && read_word(m, BASE + 0x40 * 8) == 12345, "negative reverb delay addresses wrap within ESA to7fffe");
    for (int negative = 0; negative < 2; negative++) {
        init(m); spu_write(m, 0x1a2, BASE / 8); spu_write(m, 0x1c0, 1); spu_write(m, 0x1f4, 0x10);
        static const unsigned offset[4] = {0x1d8, 0x1dc, 0x1e8, 0x1ec};
        for (int i = 0; i < 4; i++) {
            spu_write(m, 0x1c6 + i * 2, 0x7fff); spu_write(m, offset[i], (uint16_t)(0x30 + i));
            word(m, BASE + (unsigned)(0x30 + i) * 8, negative ? -32768 : 32767);
        }
        spu_write(m, CNT, 0xc080); tick(m, 1);
        CHECK(read_word(m, BASE + 0x10 * 8) == (negative ? -32768 : 32767), "reverb memory writes saturate signed accumulations from all four comb paths");
    }

    init(m); spu_write(m, 0x1a2, BASE / 8); spu_write(m, 0x1c2, 1); spu_write(m, 0x1f8, 0x30); spu_write(m, 0x1fa, 0x40);
    spu_write(m, 0x184, 0x7fff); word(m, BASE + 0x2f * 8, 16000); word(m, BASE + 0x30 * 8, 777);
    tick(m, 20);
    CHECK(read_word(m, BASE + 0x30 * 8) == 777, "ATTR reverb disable prevents writes while the playback path still reads the buffer");
    CHECK(spu_audio_read(m, out, 40) == 20 && out[38] == 15998 && !out[39], "39-tap upsampler places the impulse's4000 coefficient at sample19 and applies output/main volumes");

    init(m); block(m, 0x1000, 0, 7, 0x11); voice(m, 0, 0x1000, 0x1000); tick(m, 4);
    spu_write(m, 12, 0x7fff); spu_write(m, 0x198, 1); tick(m, 1);
    CHECK(m->spu.reverb_in[0][4] == 4075 && m->spu.reverb_in[1][4] == -4076, "EON routes each voice's signed stereo output into the reverb resampling input");
    spu_write(m, 0x198, 0); tick(m, 1);
    CHECK(!m->spu.reverb_in[0][5] && !m->spu.reverb_in[1][5], "clearing EON removes the voice from the reverb input while dry playback continues");

    init(m); block(m, 0x1000, 0, 7, 0x11); voice(m, 0, 0x1000, 0); tick(m, 4);
    spu_write(m, 12, 0x4000); spu_write(m, 0, 0x2000); spu_write(m, 0x194, 1); spu_write(m, 0x198, 1);
    m->spu.noise = 0x4000; m->spu.noise_timer = 100000; tick(m, 40);
    spu_write(m, 0x1a2, BASE / 8); spu_write(m, 0x1d4, 10); spu_write(m, 0x1c4, 0x4000); spu_write(m, 0x1fc, 0x4000);
    spu_write(m, CNT, 0xc080); tick(m, 1);
    CHECK(read_word(m, BASE + 10 * 8) == 1023, "input FIR sum32766 turns constant4096 into4095 before input and IIR half-volume products");
}

int main(void) {
    ps1 *m = ps1_new(), *other = ps1_new(); int16_t out[SPU_AUDIO_FRAMES * 2];
    if (!m || !other) { fputs("FAIL: no memory for SPU test machines\n", stderr); ps1_free(m); ps1_free(other); return 1; }
    CHECK(m->spu.next == 768 && m->next_event <= 768, "SPU is scheduled at exactly CPU_HZ/44100 cycles");
    init(m); block(m, 0x1000, 0, 7, 0x11); voice(m, 0, 0x1000, 0x1000);
    CHECK(spu_read(m, 12) == 0, "key-on starts with zero envelope before the sample clock");
    tick(m, 1); CHECK(spu_read(m, 12) == 14336, "fast linear attack increases by seven shifted left11");
    CHECK(m->spu.voice[0].decoded[3] == 4096 && m->spu.voice[0].decoded[30] == 4096, "ADPCM filter0 expands each low/high nibble into signed PCM");
    tick(m, 1); CHECK(spu_read(m, 12) == 28672, "attack repeats once per 44.1kHz sample");
    tick(m, 1); CHECK(spu_read(m, 12) == 32767, "attack saturates at7fff");
    tick(m, 1); CHECK(spu_read(m, 12) == 16383, "exponential decay scales its negative step by the current level");
    CHECK(spu_read(m, 14) == 0x200, "loop-start header updates the loop address register");
    spu_write(m, 12, 32767); spu_audio_read(m, out, SPU_AUDIO_FRAMES); tick(m, 1);
    CHECK(spu_audio_read(m, out, 1) == 1 && out[0] == 4074 && out[1] == -4076, "Gaussian taps then ADSR, signed voice volume and main volume produce exact stereo samples");
    CHECK(spu_read(m, 0x200) == 0x7ffe && spu_read(m, 0x202) == 0x8000 && spu_read(m, 0x1b8) == 0x7ffe, "current volume registers expose signed direct volumes");
    tick(m, 23); CHECK(spu_read(m, 0x19c) & 1, "ENDX is set when the final sample of a loop-end block is consumed");
    CHECK(m->spu.voice[0].addr == 0x1000 && spu_read(m, 12) == 32767, "repeat flag preserves the envelope and jumps to loop-start");
    spu_write(m, KON, 1); tick(m, 1); CHECK(!(spu_read(m, 0x19c) & 1) && spu_read(m, 12) == 14336, "retrigger clears ENDX and restarts attack");
    spu_write(m, KOFF, 1); tick(m, 1); CHECK(spu_read(m, 12) == 0, "key-off enters linear release even during attack");
    spu_write(m, CNT, 0x8000); spu_audio_read(m, out, SPU_AUDIO_FRAMES); tick(m, 1);
    CHECK(spu_audio_read(m, out, 1) == 1 && !out[0] && !out[1], "mute suppresses output without disabling the sample clock");

    init(m); block(m, 0x1000, 0x10, 7, 0x11); voice(m, 0, 0x1000, 0); tick(m, 1);
    CHECK(m->spu.voice[0].decoded[3] == 4096 && m->spu.voice[0].decoded[4] == 7936, "predictor1 adds rounded60/64 of the preceding sample");
    CHECK(m->spu.voice[0].decoded[30] == 32767 && !m->spu.voice[0].pitch, "ADPCM prediction saturates PCM and zero pitch holds sample position");
    static const int second[3] = {11456, 10368, 11904}, third[3] = {21353, 16452, 22948};
    for (int filter = 2; filter <= 4; filter++) {
        init(m); block(m, 0x1000, (uint8_t)(filter << 4), 7, 0x11); voice(m, 0, 0x1000, 0); tick(m, 1);
        CHECK(m->spu.voice[0].decoded[4] == second[filter - 2] && m->spu.voice[0].decoded[5] == third[filter - 2], "ADPCM predictors2..4 combine both preceding samples with documented signed coefficients");
    }
    init(m); block(m, 0x1000, 0x0f, 7, 0x98); voice(m, 0, 0x1000, 0x4001); tick(m, 1);
    CHECK(m->spu.voice[0].decoded[3] == -64 && m->spu.voice[0].decoded[4] == -56, "reserved shift15 uses shift9 and nibble8/9 sign-extend");
    CHECK(m->spu.voice[0].pitch == 0x4000, "pitch above3fff caps at4000, advancing four input samples");
    init(m); block(m, SPU_RAM_SIZE - 8, 0, 7, 0x22); voice(m, 23, SPU_RAM_SIZE - 8, 0x1000); tick(m, 1);
    CHECK(m->spu.voice[23].decoded[3] == 8192 && m->spu.voice[23].decoded[30] == 8192, "ADPCM block and DMA writes wrap the512KB RAM boundary");

    init(m); block(m, 0x1000, 0, 7, 0x11); voice(m, 0, 0x1000, 0);
    spu_write(m, 8, 12 << 10); tick(m, 1); CHECK(spu_read(m, 12) == 0, "shift12 attack waits for its fractional counter");
    tick(m, 1); CHECK(spu_read(m, 12) == 7, "shift12 attack steps every other output sample");
    spu_write(m, 8, 0x7f00); tick(m, 100); CHECK(spu_read(m, 12) == 7, "all-ones attack rate never steps");
    spu_write(m, 8, 0xac00); spu_write(m, 12, 0x6001); tick(m, 3); CHECK(spu_read(m, 12) == 0x6001, "exponential attack above6000 slows shift11 by four");
    tick(m, 1); CHECK(spu_read(m, 12) == 0x6008, "exponential attack fractional counter eventually applies the step");
    spu_write(m, 10, 31); spu_write(m, KOFF, 1); tick(m, 32); CHECK(spu_read(m, 12) == 0x6008, "all-ones release rate holds its level");
    spu_write(m, 0, 0); tick(m, 1); spu_write(m, 0, 0x8000); tick(m, 1);
    CHECK(spu_read(m, 0x200) == 14336, "linear volume sweep starts from the previous direct volume");
    spu_write(m, 0, 0x807f); tick(m, 32); CHECK(spu_read(m, 0x200) == 14336, "all-ones volume sweep leaves current volume unchanged");

    init(m); block(m, 0x1000, 0, 1, 0x11); voice(m, 0, 0x1000, 0x1000); tick(m, 28);
    CHECK((spu_read(m, 0x19c) & 1) && !spu_read(m, 12), "one-shot loop-end without repeat forces envelope zero");
    init(m); spu_write(m, CNT, 0xfc00); tick(m, 1); CHECK(m->spu.noise == 1, "noise generator begins with the documented feedback parity");
    tick(m, 3); CHECK(m->spu.noise == 15, "noise timer maximum shift advances the shared generator every sample");

    init(m); block(m, 0x1000, 0, 7, 0x11); voice(m, 0, 0x1000, 0); voice(m, 1, 0x1000, 0x1000); tick(m, 4);
    spu_write(m, 12, 0x4000); spu_write(m, 28, 0x4000); spu_write(m, 0x194, 1); spu_write(m, 0x190, 2);
    m->spu.noise = 0x4000; m->spu.noise_timer = 100; tick(m, 1);
    CHECK(m->spu.voice[1].pitch == 0x5400, "pitch modulation uses the previous voice's post-ADSR sample, even when that voice uses noise");

    init(m); spu_write(m, ADDR, 0xffff); spu_write(m, DATA, 0x1122); spu_write(m, DATA, 0x3344);
    CHECK(!m->spu.ram[SPU_RAM_SIZE - 8], "CPU data writes wait in the32-halfword transfer FIFO");
    spu_write(m, CNT, 0xc010); tick(m, 1);
    CHECK(m->spu.ram[SPU_RAM_SIZE - 8] == 0x22 && m->spu.ram[SPU_RAM_SIZE - 5] == 0x33 && (spu_read(m, STAT) & 0x3f) == 0x10, "manual mode commits FIFO and status at the sample clock");
    spu_write(m, ADDR, 0xffff); CHECK(spu_dma_read(m) == 0x33441122, "DMA reads preserve little-endian halfword order");
    spu_write(m, 0x1a4, 0x200); spu_write(m, CNT, 0xc040); block(m, 0x1000, 0, 7, 0x11);
    CHECK((spu_read(m, STAT) & 0x40) && (m->i_stat & 1u << IRQ_SPU), "sound RAM DMA access at IRQA latches SPU status and IRQ9");
    spu_write(m, CNT, 0xc000); CHECK(!(spu_read(m, STAT) & 0x40), "clearing IRQ enable acknowledges the SPU interrupt latch");
    m->i_stat = 0; spu_write(m, CNT, 0xc040); voice(m, 0, 0x1000, 0x1000); tick(m, 1);
    CHECK((m->i_stat & 1u << IRQ_SPU) && (spu_read(m, STAT) & 0x40), "voice ADPCM fetch can trigger the enabled RAM interrupt");
    init(m); tick(m, 1); spu_write(m, 0x1a4, 1); spu_write(m, CNT, 0xc040); tick(m, 4);
    CHECK((m->i_stat & 1u << IRQ_SPU) && (spu_read(m, STAT) & 0x40), "capture-buffer writes trigger IRQ even with silent voices");

    reverb_checks(m);

    init(other); tick(other, SPU_AUDIO_FRAMES + 9);
    CHECK(other->spu.audio_count == SPU_AUDIO_FRAMES && spu_audio_read(other, out, SPU_AUDIO_FRAMES) == SPU_AUDIO_FRAMES, "audio delivery ring bounds output when the front end does not drain it");
    CHECK(!spu_audio_read(other, out, 1), "audio drain consumes queued samples exactly once");
    CHECK(m->spu.reverb_sample && !other->spu.irq, "two machines keep independent voice, audio and interrupt state");
    uint8_t saved = m->spu.ram[0x1000]; ps1_reset(m);
    CHECK(!m->spu.audio_count && m->spu.ram[0x1000] == saved && !spu_read(m, STAT), "reset clears voices, registers and output while preserving sound RAM");
    ps1_free(m); ps1_free(other); printf("spu: %d checks, %d failed\n", checks, fails); return fails != 0;
}
