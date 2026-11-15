// Quick states use synthetic programs/devices and a temporary directory only. Replay verifies actual continuation, not merely the serializer's field list.
#include "native.h"
#include "../state.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
static void sink(void *ctx, const char *line) { (void)ctx; (void)line; }
static const ps1_native_set native = {.name = "synthetic"};
// The immediately preceding public runtime, d24ac2f, has this 64-bit ps1 size. New host bookkeeping must stay beyond its byte-compatible console prefix.
_Static_assert(offsetof(ps1, render_sync) == 4575520, "format-2 console prefix remains compatible");
typedef struct state_header {
    char magic[8]; uint32_t version, bytes, scale, fine_bytes;
    uint64_t layout, bios, media, frame, checksum; char game[128];
} state_header;
static uint64_t bytes_hash(uint64_t h, const void *data, size_t n) {
    const uint8_t *p = data; while (n--) { h ^= *p++; h *= UINT64_C(0x100000001b3); } return h;
}
static int previous_format(const char *path) {
    FILE *f = fopen(path, "rb"); state_header h;
    if (!f) return -1;
    if (fread(&h, sizeof h, 1, f) != 1) { fclose(f); return -1; }
    size_t bytes = h.bytes; uint8_t *payload = malloc(bytes + h.fine_bytes);
    if (!payload || fread(payload, bytes + h.fine_bytes, 1, f) != 1) { free(payload); fclose(f); return -1; } fclose(f);
    h.version = 2; h.bytes = 4575520; h.checksum = 0;
    uint64_t hash = bytes_hash(UINT64_C(0xcbf29ce484222325), &h, sizeof h);
    hash = bytes_hash(hash, payload, h.bytes); h.checksum = bytes_hash(hash, payload + bytes, h.fine_bytes);
    f = fopen(path, "wb");
    int bad = !f || fwrite(&h, sizeof h, 1, f) != 1 || fwrite(payload, h.bytes, 1, f) != 1 ||
              (h.fine_bytes && fwrite(payload + bytes, h.fine_bytes, 1, f) != 1);
    if (f && fclose(f)) bad = 1; free(payload); return bad ? -1 : 0;
}
static void external_emit(void *ctx, const ps1_render_op *op) { (void)ctx; (void)op; }
static int external_sync(void *ctx) { ps1 *m = ctx; m->gpu.vram[42] = 0x4567; return 0; }
static int failed_sync(void *ctx) { (void)ctx; return -1; }
static uint64_t file_hash(const char *path) {
    FILE *f = fopen(path, "rb"); uint64_t h = 0xcbf29ce484222325ULL; int c;
    if (!f) return 0; while ((c = fgetc(f)) != EOF) { h ^= (unsigned)c; h *= 0x100000001b3ULL; } fclose(f); return h;
}
static void program(ps1 *m) {
    const uint32_t code[] = {0x24210001, 0xad010000, 0x1000fffd, 0};
    memcpy(m->ram + 0x1000, code, sizeof code); m->cpu.pc = 0x80001000; m->cpu.next_pc = m->cpu.pc + 4; m->cpu.cop0[C0_SR] = 0;
    m->cpu.r[8] = 0x80003000;
    for (int i = 0; i < VRAM_W * VRAM_H; i++) m->gpu.vram[i] = (uint16_t)(i * 37);
    m->spu.ram[100] = 0x43; m->cd.audio_n = 3; m->cd.audio[0] = 123; m->cpu.cop2d[12] = 0x12340056;
    mdec_write(m, 0, 0x40000001); mdec_write(m, 0, 0x12345678);
    sio_card_format(m, 0); sio_card_format(m, 1); sio_set_pad(m, 0, 1, PAD_CROSS);
    // A half-received VRAM upload must continue from the same halfword, not restart or disappear.
    gpu_gp0(m, 0xa0000000); gpu_gp0(m, 20 | 20u << 16); gpu_gp0(m, 3 | 1u << 16); gpu_gp0(m, 0x12345678);
    ps1_run_frame(m);
}
static int image(const char *path, uint8_t marker) {
    FILE *f = fopen(path, "wb"); if (!f) return -1;
    for (int i = 0; i < 40; i++) {
        uint8_t bytes[DISC_SECTOR] = {0}; memset(bytes + 1, 0xff, 10); bytes[15] = 2;
        if (i == 39) bytes[2000] = marker;   // identical TOC, boot/system area and first 39 sectors; differing later content
        if (fwrite(bytes, 1, sizeof bytes, f) != sizeof bytes) { fclose(f); return -1; }
    }
    return fclose(f);
}

int main(void) {
    char temp[] = "/tmp/muyiunative-state-XXXXXX", error[FRONT_STATE_ERROR], path[256], previous[256];
    char *root = mkdtemp(temp); ps1 *m = ps1_new(), *other = ps1_new();
    if (!root || !m || !other) return 1;
    int directory = open(root, O_RDONLY | O_DIRECTORY); snprintf(path, sizeof path, "%s/quick.ps1state", root); snprintf(previous, sizeof previous, "%s/previous", root);
    program(m); CHECK(!front_state_exists(directory), "a new game has no quick state");
    m->tty_out = sink; m->tty_ctx = m; ps1_native_add(m, &native);
    CHECK(!front_state_save(directory, "synthetic-A", m, 99, 0, NULL, error), "a complete console state saves");
    CHECK(front_state_exists(directory), "the saved slot becomes available");
    CHECK(!previous_format(path), "a synthetic state can be encoded with the preceding public format-2 layout");
    front_snapshot *s = front_state_read(directory, "synthetic-A", m, error);
    CHECK(s && s->frame == 99 && !s->scale && !s->fine, "a native state reloads with its frame index");
    if (s) {
        CHECK(!s->machine->tty_out && !s->machine->tty_ctx && !s->machine->native[0] && !s->machine->disc.file[0] && !s->machine->on_render && !s->machine->exe,
              "no file handle, native code or callback pointer is serialized");
        CHECK(!s->machine->sio.card[0].present && !s->machine->sio.card[0].data[0], "external card bytes are absent from the payload");
        sio_card_format(other, 0); other->sio.card[0].data[1000] = 0x99; other->sio.card[0].dirty = 1;
        other->tty_out = sink; other->tty_ctx = other; ps1_native_add(other, &native);
        other->disc.file[0] = tmpfile(); other->disc.nfiles = 1; FILE *mounted = other->disc.file[0];
        front_state_apply(other, s);
        CHECK(other->tty_ctx == other && other->tty_out == sink && other->native[0] == &native && other->disc.file[0] == mounted && other->disc.nfiles == 1,
              "restoring keeps current host bindings and mounted streams");
        CHECK(other->sio.card[0].data[1000] == 0x99 && other->sio.card[0].dirty, "restoring does not roll back current memory-card saves or dirty bytes");
        CHECK(other->gpu.xfer == 1 && other->gpu.xi == 2 && other->mdec.busy, "partial GPU and MDEC input survives restoration");
        gpu_gp0(m, 0xabcdef01); gpu_gp0(other, 0xabcdef01);
        for (int i = 0; i < 5; i++) {
            ps1_run_frame(m); ps1_run_frame(other);
            CHECK(m->cycles == other->cycles && !memcmp(&m->cpu, &other->cpu, sizeof m->cpu) && !memcmp(m->ram, other->ram, sizeof m->ram),
                  "restored execution reproduces CPU, cycles and RAM at the next frame");
            CHECK(!memcmp(&m->gpu, &other->gpu, sizeof m->gpu) && !memcmp(&m->spu, &other->spu, sizeof m->spu) && !memcmp(&m->cd, &other->cd, sizeof m->cd) &&
                  !memcmp(&m->mdec, &other->mdec, sizeof m->mdec), "restored video, sound and disc/decoder state continue identically");
            int16_t a[2048], b[2048]; int an = spu_audio_read(m, a, 1024), bn = spu_audio_read(other, b, 1024);
            CHECK(an == bn && !memcmp(a, b, (size_t)an * 4), "restored audio produces the same sample sequence");
        }
        front_state_free(s);
    }
    mdec_reset(m); mdec_write(m, 0, 0x28000001); mdec_write(m, 0, 0xfe000000);
    CHECK(m->mdec.next > m->cycles && m->mdec.next != NEVER, "snapshot fixture has decoded output still processing");
    CHECK(!front_state_save(directory, "synthetic-A", m, 100, 0, NULL, error), "pending decoder processing saves before its completion event");
    s = front_state_read(directory, "synthetic-A", m, error);
    CHECK(s && s->machine->mdec.next == m->mdec.next, "pending decoder deadline survives disk serialization");
    if (s) {
        front_state_apply(other, s); ps1_run_frame(m); ps1_run_frame(other);
        CHECK(!memcmp(&m->mdec, &other->mdec, sizeof m->mdec) && m->next_event == other->next_event &&
              !(mdec_read(m, 4) >> 31) && mdec_read(m, 0) == mdec_read(other, 0), "restoring pending processing exposes the same output at the same guest time");
        front_state_free(s);
    }
    uint64_t before = file_hash(path), cycles = m->cycles;
    CHECK(!front_state_read(directory, "synthetic-B", m, error) && strstr(error, "different game"), "a different game's state is rejected");
    m->bios[0] = 1; CHECK(!front_state_read(directory, "synthetic-A", m, error), "a different BIOS is rejected"); m->bios[0] = 0;
    m->disc.sectors = 100; CHECK(!front_state_read(directory, "synthetic-A", m, error), "a different disc layout is rejected"); m->disc.sectors = 0;
    m->sio.device = 2;
    CHECK(front_state_save(directory, "synthetic-A", m, 100, 0, NULL, error) < 0 && !front_state_read(directory, "synthetic-A", m, error), "active card transactions cannot be rewound");
    m->sio.device = 0;
    CHECK(file_hash(path) == before && m->cycles == cycles, "failed state operations leave both the slot and current machine unchanged");
    CHECK(!rename(path, previous) && !mkdir(path, 0700), "a directory can block atomic state replacement");
    CHECK(front_state_save(directory, "synthetic-A", m, 100, 0, NULL, error) < 0 && file_hash(previous) == before, "failed replacement preserves the previous state");
    rmdir(path); rename(previous, path);
    int fd = open(path, O_RDWR); unsigned char byte; pread(fd, &byte, 1, 300); byte ^= 1; pwrite(fd, &byte, 1, 300); close(fd);
    CHECK(!front_state_read(directory, "synthetic-A", m, error) && strstr(error, "damaged"), "payload corruption is rejected before applying it");
    CHECK(!front_state_save(directory, "synthetic-A", m, 100, 0, NULL, error), "saving repairs a damaged slot by atomic replacement");
    struct stat st; stat(path, &st); truncate(path, st.st_size - 1);
    CHECK(!front_state_read(directory, "synthetic-A", m, error), "a truncated file is rejected");
    unlink(path); symlink(previous, path);
    CHECK(!front_state_exists(directory) && !front_state_read(directory, "synthetic-A", m, error), "a slot symlink is never followed"); unlink(path);
    mkfifo(path, 0600); CHECK(!front_state_read(directory, "synthetic-A", m, error), "a non-regular slot is rejected without blocking"); unlink(path);
    size_t words = (size_t)VRAM_W * VRAM_H * 4; uint16_t *fine = malloc(words * sizeof *fine);
    CHECK(fine != NULL, "fine snapshot fixture allocates");
    if (fine) {
        for (size_t i = 0; i < words; i++) fine[i] = (uint16_t)(i ^ (i >> 12));
        CHECK(!front_state_save(directory, "synthetic-A", m, 123, 2, fine, error), "a 2x fine framebuffer saves with console state");
        CHECK(!previous_format(path), "format-2 compatibility also retains a fine framebuffer payload");
        s = front_state_read(directory, "synthetic-A", m, error);
        CHECK(s && s->scale == 2 && s->frame == 123 && !memcmp(s->fine, fine, words * sizeof *fine), "every fine sample is retained on disk");
        front_state_free(s); free(fine);
    }
    char disc1[256], disc2[256]; snprintf(disc1, sizeof disc1, "%s/one.bin", root); snprintf(disc2, sizeof disc2, "%s/two.bin", root);
    CHECK(!image(disc1, 0x11) && !image(disc2, 0x22) && !disc_open(m, disc1), "equal-layout multi-disc fixtures mount");
    uint8_t sector[DISC_SECTOR]; disc_read(m, 5, sector); off_t position = ftello(m->disc.file[0]);
    CHECK(!front_state_save(directory, "synthetic-A", m, 456, 0, NULL, error) && ftello(m->disc.file[0]) == position, "state identity covers complete media without moving sector streams");
    before = file_hash(path);
    CHECK(!disc_open(m, disc2) && !front_state_read(directory, "synthetic-A", m, error) && strstr(error, "Insert the disc"),
          "equal-layout discs with different later bytes cannot load each other's state");
    CHECK(file_hash(path) == before && !disc_read(m, 39, sector) && sector[2000] == 0x22, "wrong-disc rejection keeps the state file and current media intact");
    CHECK(!disc_open(m, disc1), "original disc can be reinserted"); s = front_state_read(directory, "synthetic-A", m, error);
    CHECK(s && s->frame == 456, "reinserting the saved disc permits loading again"); front_state_free(s);
    m->on_render = external_emit; m->render_ctx = m; m->render_sync = external_sync; m->render_external = 1;
    CHECK(!front_state_save(directory, "synthetic-A", m, 456, 0, NULL, error), "saving synchronizes an external renderer before serializing console state");
    s = front_state_read(directory, "synthetic-A", m, error);
    CHECK(s && s->machine->gpu.vram[42] == 0x4567 && !s->machine->render_sync && !s->machine->render_external,
          "external pixels survive the snapshot without serializing renderer hooks"); front_state_free(s);
    before = file_hash(path); m->render_sync = failed_sync;
    CHECK(front_state_save(directory, "synthetic-A", m, 456, 0, NULL, error) < 0 && file_hash(path) == before,
          "a failed GPU readback preserves the previous quick state");
    m->on_render = NULL; m->render_ctx = NULL; m->render_sync = NULL; m->render_external = 0; m->exit_requested = m->exit_code = 0;
    cd_set_shell(m, 1);
    CHECK(front_state_save(directory, "synthetic-A", m, 500, 0, NULL, error) < 0 && !front_state_read(directory, "synthetic-A", m, error) && file_hash(path) == before,
          "open-cover states cannot silently restore a physical disc mismatch"); cd_set_shell(m, 0);
    CHECK(!disc_open(m, disc2) && !truncate(disc2, DISC_SECTOR) && front_state_save(directory, "synthetic-A", m, 500, 0, NULL, error) < 0 && file_hash(path) == before,
          "an unreadable media identity never overwrites the last good state");
    unlink(disc1); unlink(disc2); unlink(path); close(directory); CHECK(!rmdir(root), "successful and failed saves leave no temporary files behind");
    ps1_free(m); ps1_free(other); printf("state: %d checks, %d failed\n", checks, fails); return fails != 0;
}
