// The console the runtime models (plan.md, section 2): memory, the R3000A and COP0, the interrupt controller, DMA, the timers, the GPU with its software renderer, the
// CD-ROM drive, SPU voices and memory, MDEC, and the controller/memory-card ports. All of a machine's state is in its ps1 struct and nothing
// is global, so two machines can run side by side (the lockstep comparison, plan.md section 6). Each file cites the psx-spx sections it models
#pragma once
#include <stdint.h>
#include <stdio.h>
#include "disc.h"
#include "gte.h"
#include "sio.h"
#include "spu.h"
#include "mdec.h"
#include "render.h"

_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "the console and the host are both little-endian: memory is read and written as host integers");

enum { RAM_SIZE = 2 << 20, SCRATCH_SIZE = 1 << 10, BIOS_SIZE = 512 << 10, SPU_RAM_SIZE = 512 << 10, VRAM_W = 1024, VRAM_H = 512 };
enum { CPU_HZ = 33868800 };
enum { IRQ_VBLANK, IRQ_GPU, IRQ_CDROM, IRQ_DMA, IRQ_TMR0, IRQ_TMR1, IRQ_TMR2, IRQ_SIO0, IRQ_SIO1, IRQ_SPU, IRQ_PIO };   // I_STAT bits (psx-spx, Interrupts)
enum { C0_BADVADDR = 8, C0_SR = 12, C0_CAUSE = 13, C0_EPC = 14, C0_PRID = 15 };
enum { EXC_INT = 0, EXC_ADEL = 4, EXC_ADES = 5, EXC_IBE = 6, EXC_DBE = 7, EXC_SYS = 8, EXC_BP = 9, EXC_RI = 10, EXC_CPU = 11, EXC_OV = 12 };   // CAUSE.Excode
#define NEVER UINT64_MAX

typedef struct ps1_cpu {
    uint32_t r[32], hi, lo, pc, next_pc, cur_pc;
    uint32_t ld_r, ld_v;   // the load in flight: it reaches its register after the next instruction (the load delay slot)
    int branch, delay;     // the instruction just run was a branch / the one running is in its delay slot
    int boundary;          // the instruction just run ended a block (it was in a delay slot, or an exception came): events and interrupts are taken here (cpu_run)
    uint32_t cop0[32], cop2d[32], cop2c[32];
    uint64_t gte_busy, hilo_busy;   // the cycles at which the GTE's command and MULT/DIV's result are done (reading early waits)
} ps1_cpu;

typedef struct ps1_dma { uint32_t madr, bcr, chcr, addr, block_left, block_size, blocks; uint64_t left; int active, gpu_wait; } ps1_dma;
// acc: CPU cycles not yet counted, 16.16; gate: whether MODE.0's synchronization lets the counter count
typedef struct ps1_timer { uint32_t mode; uint16_t count, target; uint64_t last, next; uint64_t acc; int fired, gate; } ps1_timer;

typedef struct ps1_gpu {
    uint16_t vram[VRAM_H * VRAM_W];
    uint32_t buf[16]; int n, need;   // the GP0 command being received
    int poly, pgour, psemi, pnv, phave; uint32_t pcol; ps1_vtx plast;   // a polyline being received (its vertices arrive until a terminator)
    int xfer; uint16_t xx, xy, xw, xh; uint32_t xi;   // a VRAM transfer: 1 = CPU to VRAM (GP0 A0h), 2 = VRAM to CPU (GP0 C0h); its rectangle and next halfword
    uint32_t read, texpage, texwin, mask;
    uint16_t texcache[256][4], clutcache[256], clut_id; uint32_t textag[256]; int clut_valid, clut_n;
    int16_t ax1, ay1, ax2, ay2, ox, oy;   // drawing area (GP0 E3h/E4h) and offset (GP0 E5h)
    int irq, display_off, dma_dir, tex_disable_ok;   // ...; GP1(09h): GP0(E1h) bit 11 may disable textures
    uint16_t disp_x, disp_y, hx1, hx2, vy1, vy2; uint32_t dmode;   // GP1(05h..08h)
    int line, field; uint64_t next_line; uint32_t line_frac;   // video timing: scanline, interlace field, next scanline's cycle and its 16.16 remainder
    uint64_t line_start, next_hblank; int hblank;   // the scanline's first cycle; while timer 0 synchronizes: its next hblank edge, and whether in hblank
    uint64_t draw_until;   // measured flat-rectangle draw occupancy, in CPU cycles; other drawing costs remain unmodelled
} ps1_gpu;

enum { CD_AUDIO_FRAMES = 16384 };   // 44.1 kHz frames the drive holds for the SPU: one 18.9 kHz mono XA-ADPCM sector makes 9408
typedef struct ps1_cdrom {
    uint8_t index, param[16], nparam, resp[16], nresp, rpos, hintmsk, hintsts, stat, mode, busy, irq;
    uint8_t atv[4], atv_next[4], muted, adp_mute;   // ATV0..3 applied and as written (ADPCTL.5 applies them), Mute, ADPCTL.0 (psx-spx, CDROM Controller I/O Ports)
    int pending; uint8_t pending_cmd;   // a command written while an interrupt was unacknowledged waits for it (psx-spx, BUSYSTS flag)
    uint8_t now_int, now_resp[16], now_n; uint64_t next;   // the response on its way, delivered at `next`
    uint8_t later_int, later_resp[16], later_n; uint32_t later_delay; int later;   // a second response, sent once the first is acknowledged
    uint64_t sector_next; uint32_t target, lba, last_lba; int setloc, reading, playing, have_header, complete_stat;
    uint8_t sector[8][2352], header[8], data[2340];   // decoder slots and the independently locked host FIFO (psx-spx, CDROM Response/Data Queueing)
    uint16_t data_n, data_pos; uint8_t head, ready, have_sector, data_pending, end_pending, filter_file, filter_channel, bfrd;
    uint8_t filter_drop, size_mode, region, scex, report[8], report_n;   // ...; the licence region (A/E/I, 0 = none) and SCEx counters; a Play report waiting
    int16_t xa_old[2], xa_older[2], zz[2][32]; unsigned zz_p, zz_six;   // XA-ADPCM: the predictors' history and the zigzag resampler's 37.8 kHz ring
    int16_t audio[CD_AUDIO_FRAMES * 2]; unsigned audio_read, audio_n; int audio_xa;   // stereo frames on their way to the SPU, and whether they are XA-ADPCM
} ps1_cdrom;

typedef struct ps1_native_fn ps1_native_fn;
typedef struct ps1_native_set ps1_native_set;   // native code generated for some guest code (native.h)

typedef struct ps1 {
    uint8_t ram[RAM_SIZE];           // main RAM; the BIOS kernel keeps the first 64 KB
    uint8_t scratch[SCRATCH_SIZE];   // the scratchpad: the R3000A's data cache, used as fast RAM
    uint8_t bios[BIOS_SIZE];         // the BIOS ROM
    ps1_cpu cpu;
    uint64_t cycles, next_event;     // CPU cycles since reset; the cycle of the next scheduled event (sched.c)
    uint32_t i_stat, i_mask, memctl[9], ram_size, cache_ctl, dpcr, dicr, post;
    ps1_dma dma[7]; uint64_t dma_next;   // GPU linked-list packet event, NEVER when stopped
    ps1_timer tmr[3];
    ps1_gpu gpu;
    ps1_cdrom cd;
    ps1_disc disc;                  // external read-only media; reset keeps the mounted disc, destruction closes it
    ps1_spu spu;
    ps1_sio sio;
    ps1_mdec mdec;
    char tty[256]; int ntty;         // the debug console line being written (0x1F802080)
    void (*tty_out)(void *ctx, const char *line); void *tty_ctx;   // where finished debug console lines go: stderr when NULL
    void (*on_render)(void *ctx, const ps1_render_op *op); void *render_ctx;   // optional host rendering consumer; console state stays in gpu
    void (*on_step)(struct ps1 *m, uint32_t pc, uint32_t op); void *step_ctx;   // the host's, told of each interpreted instruction after it ran (capture.c)
    uint8_t *exe; size_t exe_n;      // a PS-X EXE waiting for the BIOS to reach its shell (exe.c)
    const ps1_native_set *native[4]; int nnative;   // the native code this machine runs where memory holds the code it was made from (native.c)
    uint64_t native_runs, native_steps, interp_steps;   // native entries and instruction counts: how much runs native
    const ps1_native_fn *native_active; int native_dirty;   // host bookkeeping: a store into the running function returns to the dispatcher immediately
    uint32_t native_pc, native_fn;   // where native code was last entered, and its guest function (for lockstep reports)
    int frame_done, exit_requested, exit_code;
    unsigned red, unmapped;          // [RED] reports already made: features (RED_ONCE bits), and unmapped accesses (a count)
} ps1;

// The machine (ps1.c)
ps1 *ps1_new(void);   // a machine after reset, its memories zeroed; NULL when out of memory
void ps1_free(ps1 *m);
void ps1_reset(ps1 *m);
int ps1_load_bios(ps1 *m, const char *path);   // 0, or -1 after a [RED] line saying why
int ps1_load_exe(ps1 *m, const uint8_t *exe, size_t n);   // a PS-X EXE (copied) to start in place of the BIOS shell; 0, or -1 after a [RED] line
int ps1_exe_due(const ps1 *m);
void ps1_exe_start(ps1 *m);
void tty_putc(ps1 *m, char c);                   // the debug console: written through 0x1F802080 or the kernel's std_out_putchar
void ps1_run_frame(ps1 *m);                     // runs to the next vertical blank (or to an exit request through the debug port)
// The picture the console shows now, as BGRA8 pixels (0xAARRGGBB words) into out (cap_w x cap_h at most): its size in *w, *h (gpu_sw.c)
void ps1_display(const ps1 *m, uint32_t *out, int cap_w, int cap_h, int *w, int *h);
uint64_t ps1_frame_hash(const uint32_t *px, int w, int h);   // FNV-1a over the pixels: frame comparisons keep hashes, never pictures (AGENTS.md)

// Memory as the CPU sees it, by virtual address (mem.c). Alignment is the CPU's to check (an address error); an unaligned access here reaches nothing
uint32_t ps1_phys(uint32_t addr);
uint8_t mem_read8(ps1 *m, uint32_t addr);
uint16_t mem_read16(ps1 *m, uint32_t addr);
uint32_t mem_read32(ps1 *m, uint32_t addr);
void mem_write8(ps1 *m, uint32_t addr, uint32_t v);    // v: the whole register being stored (mem.c, wr)
void mem_write16(ps1 *m, uint32_t addr, uint32_t v);
void mem_write32(ps1 *m, uint32_t addr, uint32_t v);
int mem_fetch(ps1 *m, uint32_t addr, uint32_t *op);   // an instruction word; 0 where nothing executable is mapped (a bus error)
const uint8_t *mem_code(ps1 *m, uint32_t addr, uint32_t n);   // the n bytes from addr in RAM or the BIOS, which code runs from; NULL if not all in one
unsigned mem_load_cycles(uint32_t addr, unsigned n);   // the CPU cycles a load of n bytes takes, its instruction's included
// The CPU cycles fetching the instruction at pc takes: the instruction cache serves KUSEG and KSEG0 in a cycle (a hit is assumed: the cache is not modelled); KSEG1 is
// uncached, so each instruction is a 32-bit read. Inline, so that native code, whose pc is a constant, folds it
static inline unsigned mem_fetch_cycles(uint32_t pc) { return pc >> 29 == 5 ? mem_load_cycles(pc, 4) : 1; }
void irq_raise(ps1 *m, int n);
void irq_update(ps1 *m);                        // CAUSE.IP2 follows I_STAT AND I_MASK

// The CPU (cpu.c, interp.c) and the native code it runs (native.c)
void cpu_reset(ps1 *m);
void cpu_step(ps1 *m);                          // one instruction, interpreted (cpu_ops.h); interrupts are cpu_run's
void cpu_run(ps1 *m);                           // runs until the end of a block at or after the next scheduled event
void cpu_exception(ps1 *m, uint32_t code);
int ps1_native_add(ps1 *m, const ps1_native_set *set);   // 0, or -1 after a [RED] line when the machine has no room for more
void native_written(ps1 *m, uint32_t addr, unsigned n);   // CPU or synchronous DMA wrote RAM/scratchpad while native code was running
int native_run(ps1 *m);                         // runs native code from the CPU's pc, where some matches memory: 1 if it ran

// The lockstep comparison (lockstep.c): two machines from the same state, ref interpreting and nat with native code, stop together at the end of the first block
// `step` cycles on (0 or 1: every block); the CPU is compared at each stop, all memory and the devices every full_every stops (0: only at the end of each frame)
typedef struct ps1_lockstep { uint64_t stops; unsigned step, full_every; char report[400]; } ps1_lockstep;
int ps1_lockstep_frame(ps1 *ref, ps1 *nat, ps1_lockstep *ls);   // one frame in step: 0 while they agree, else -1 with the first difference in ls->report

// The capture (capture.c): interpreted blocks and their versions, call targets and final RAM, for tools/codegen.py --capture; the host's, not machine state
typedef struct ps1_capture_block ps1_capture_block;
typedef struct ps1_capture {
    uint32_t *a; size_t n, cap; int failed;
    ps1_capture_block *blocks[4096]; uint32_t pc, words[258]; unsigned nwords;   // immutable interpreted blocks, including overwritten versions
} ps1_capture;
void ps1_capture_start(ps1 *m, ps1_capture *c);   // from now on (it takes on_step)
int ps1_capture_write(ps1 *m, ps1_capture *c, const char *out);   // <out>.txt, .ram and .blocks, then stop capturing; 0, or -1 after a [RED] line

// The devices, each by its register offset
void sched_run(ps1 *m);                         // runs the events that are due (sched.c)
void sched_update(ps1 *m);                      // after an event moved: the next one
void dma_reset(ps1 *m);
void dma_event(ps1 *m);
uint32_t dma_read(ps1 *m, uint32_t off);
void dma_write(ps1 *m, uint32_t off, uint32_t v);
void dma_resume(ps1 *m);                        // re-evaluate MDEC requests after a CPU-side FIFO/control access
void dma_gpu_resume(ps1 *m);                    // re-evaluate a linked-list request after GP1 changes its DMA direction
void tmr_reset(ps1 *m);
uint32_t tmr_read(ps1 *m, uint32_t off);
void tmr_write(ps1 *m, uint32_t off, uint32_t v);
void tmr_hblank(ps1 *m);
void tmr_blank(ps1 *m, int n, int start, uint64_t at);   // timer n's blank (0: hblank, 1: vblank) starts or ends at cycle `at`
void tmr_event(ps1 *m, int n);
void gpu_reset(ps1 *m);
void gpu_gp0(ps1 *m, uint32_t w);
void gpu_gp0_at(ps1 *m, uint32_t w, uint64_t at); // DMA words use their scheduled bus time, independent of the CPU's event-dispatch lateness
void gpu_gp1(ps1 *m, uint32_t w);
uint32_t gpu_read(ps1 *m);
uint32_t gpu_stat(ps1 *m);
void gpu_line(ps1 *m);
void gpu_hblank(ps1 *m);                        // timer 0's next hblank edge is due
int gpu_hblank_now(ps1 *m);                     // in hblank now; schedules the edges while timer 0 synchronizes
int gpu_vblank(const ps1 *m);
uint64_t gpu_dot_fp(const ps1 *m);              // CPU cycles per dotclock tick, 16.16
void cd_reset(ps1 *m);
uint8_t cd_read(ps1 *m, uint32_t off);
void cd_write(ps1 *m, uint32_t off, uint8_t v);
void cd_event(ps1 *m);
void cd_sector_event(ps1 *m);
uint32_t cd_dma_read(ps1 *m);
void cd_audio(ps1 *m, int16_t out[2]);   // the drive's next 44.1 kHz stereo frame into the SPU (silence when it has none)
void spu_reset(ps1 *m);
uint16_t spu_read(ps1 *m, uint32_t off);
void spu_write(ps1 *m, uint32_t off, uint16_t v);
void spu_dma_write(ps1 *m, uint32_t w);
uint32_t spu_dma_read(ps1 *m);
void sio_reset(ps1 *m);

// One line on stderr; a line that marks something unimplemented or wrong starts with "[RED] " (AGENTS.md)
#define LOG(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#define RED(...) do { fputs("[RED] ", stderr); LOG(__VA_ARGS__); } while (0)
// A feature gap is reported once per machine, not on every use
enum { RED_GTE = 1, RED_DMA_LIST = 2, RED_MDEC = 4, RED_SPU_VOICE = 8, RED_DMA_MODE = 16, RED_DMA_MDEC = 32, RED_GPU = 64, RED_CD = 128, RED_COP0 = 256,
       RED_CD_TIMING = 512, RED_CD_AUDIO = 1024, RED_CD_ID = 2048, RED_CD_OVERRUN = 4096, RED_SPU_EFFECT = 8192, RED_GTE_TIMING = 16384, RED_TEX_DISABLE = 32768,
       RED_CD_SCAN = 1 << 16, RED_CD_SESSION = 1 << 17, RED_CD_GETQ = 1 << 18, RED_CD_SOUNDMAP = 1 << 19, RED_CD_IGNORE = 1 << 20, RED_CD_PEAK = 1 << 21,
       RED_CD_XA = 1 << 22, RED_CD_UNLOCK = 1 << 23, RED_GPU_TIMING = 1 << 24, RED_DMA_TO_DEVICE = 1 << 25, RED_DMA_FROM_DEVICE = 1 << 26 };
#define RED_ONCE(m, bit, ...) do { if (!((m)->red & (bit))) { (m)->red |= (bit); RED(__VA_ARGS__); } } while (0)
