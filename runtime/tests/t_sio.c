// Phase 2 SIO0 tests build pads and card bytes in code: no save files, BIOS, game or GPU needed. Public psx-spx: Serial Interfaces (SIO), DR/SR/MR/CR/BR and FIFO/IRQ
// notes; Controllers and Memory Cards, Communication Sequence, Standard Controllers, Memory Card Read/Write Commands, FLAG Byte, Timings and Data Format
#include "ps1.h"
#include <stdlib.h>
#include <string.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
enum { DATA = 0x1f801040, STAT = DATA + 4, MODE = DATA + 8, CTRL = DATA + 10, BAUD = DATA + 14, BYTE_CYCLES = 136 * 8 };
static void advance(ps1 *m, uint64_t cycles) {
    uint64_t end = m->cycles + cycles;
    while (m->next_event <= end) { m->cycles = m->next_event; sched_run(m); }
    m->cycles = end;
}
static void begin(ps1 *m, int port, int irq) {
    mem_write16(m, CTRL, 0x10); mem_write16(m, MODE, 0x0d); mem_write16(m, BAUD, 136);
    for (int i = 0; i < 8 && mem_read32(m, STAT) & 2; i++) mem_read8(m, DATA);
    mem_write16(m, CTRL, (uint16_t)(3 | port << 13 | (irq ? 0x1000 : 0)));
    mem_write32(m, 0x1f801070, ~(1u << IRQ_SIO0));
}
static uint8_t xfer(ps1 *m, uint8_t v, uint32_t extra) {
    mem_write8(m, DATA, v); advance(m, BYTE_CYCLES);
    CHECK(mem_read32(m, STAT) & 2, "a complete selected serial byte reaches RX FIFO");
    uint8_t r = mem_read8(m, DATA); advance(m, 500 + extra); return r;
}
static void card_start(ps1 *m, uint8_t cmd, uint16_t sector) {
    CHECK(xfer(m, 0x81, 0) == 0xff, "card address byte receives high impedance");
    uint8_t flag = xfer(m, cmd, 0); CHECK(!(flag & ~0x0c), "card command returns FLAG byte");
    CHECK(xfer(m, 0, 0) == 0x5a && xfer(m, 0, 0) == 0x5d, "card ID prefix");
    if (cmd != 0x53) { xfer(m, (uint8_t)(sector >> 8), 0); xfer(m, (uint8_t)sector, 0); }
}
static uint8_t card_write(ps1 *m, uint16_t sector, const uint8_t *bytes, int bad) {
    begin(m, 0, 0); card_start(m, 0x57, sector); uint8_t sum = (uint8_t)(sector >> 8 ^ sector);
    for (int i = 0; i < CARD_SECTOR; i++) { xfer(m, bytes[i], 0); sum ^= bytes[i]; }
    xfer(m, (uint8_t)(sum ^ bad), 0);
    CHECK(xfer(m, 0, 0) == 0x5c && xfer(m, 0, 0) == 0x5d, "card write acknowledge bytes"); return xfer(m, 0, 0);
}

int main(void) {
    ps1 *m = ps1_new(), *other = ps1_new(); uint8_t *image = malloc(CARD_SIZE);
    if (!m || !other || !image) { fputs("FAIL: no memory for SIO test\n", stderr); ps1_free(m); ps1_free(other); free(image); return 1; }
    begin(m, 0, 1); mem_write8(m, DATA, 1); advance(m, BYTE_CYCLES - 1);
    CHECK(!(mem_read32(m, STAT) & 6), "RX and TX idle remain clear before final serial clock"); advance(m, 1);
    CHECK(mem_read8(m, DATA) == 0xff, "unplugged pad returns high impedance"); advance(m, 2000);
    CHECK(!(mem_read32(m, STAT) & 0x280) && !(m->i_stat & 1u << IRQ_SIO0), "unplugged device sends no ACK or IRQ");
    sio_set_pad(m, 0, 1, PAD_START | PAD_UP | PAD_CROSS); begin(m, 0, 1);
    mem_write8(m, DATA, 1); advance(m, BYTE_CYCLES);
    CHECK((mem_read32(m, STAT) & 7) == 7 && !(mem_read32(m, STAT) & 0x280), "RX byte arrives before peripheral ACK");
    CHECK(mem_read8(m, DATA) == 0xff, "first controller byte is high impedance"); advance(m, 399);
    CHECK(!(mem_read32(m, STAT) & 0x280), "ACK waits beyond the kernel's 100-cycle ignore window"); advance(m, 1);
    CHECK((mem_read32(m, STAT) & 0x280) == 0x280 && m->i_stat & 1u << IRQ_SIO0, "ACK low asserts DSR status and delayed IRQ7");
    mem_write32(m, 0x1f801070, ~(1u << IRQ_SIO0)); mem_write16(m, CTRL, 0x1013);
    CHECK(mem_read32(m, STAT) & 0x200 && !(m->i_stat & 1u << IRQ_SIO0), "ACK low holds the SIO IRQ without creating another I_STAT edge"); advance(m, 100);
    CHECK(!(mem_read32(m, STAT) & 0x80), "ACK returns high after its pulse");
    mem_write32(m, 0x1f801070, ~(1u << IRQ_SIO0)); mem_write16(m, CTRL, 0x1013);
    CHECK(!(mem_read32(m, STAT) & 0x200), "IRQ acknowledges after ACK returns high");
    CHECK(xfer(m, 0x42, 0) == 0x41 && xfer(m, 0, 0) == 0x5a, "digital pad identifies as 5A41h");
    CHECK(xfer(m, 0, 0) == 0xe7, "Start and Up are active-low in the low button byte");
    mem_write32(m, 0x1f801070, ~(1u << IRQ_SIO0)); mem_write16(m, CTRL, 0x1013);
    CHECK(xfer(m, 0, 0) == 0xbf && !(mem_read32(m, STAT) & 0x280) && !(m->i_stat & 1u << IRQ_SIO0), "Cross is active-low; final packet byte has no ACK");
    begin(m, 1, 1); xfer(m, 1, 0); CHECK(!(m->i_stat & 1u << IRQ_SIO0), "second port stays empty until separately connected");
    sio_set_pad(m, 1, 1, PAD_CIRCLE); begin(m, 1, 0);
    CHECK(xfer(m, 1, 0) == 0xff && xfer(m, 0x42, 0) == 0x41 && xfer(m, 0, 0) == 0x5a && xfer(m, 0, 0) == 0xff && xfer(m, 0, 0) == 0xdf, "port selection polls its own pad and button state");
    begin(other, 0, 1); xfer(other, 1, 0); CHECK(!(other->i_stat & 1u << IRQ_SIO0), "another machine has independent connections");

    begin(m, 0, 0); xfer(m, 1, 0); mem_write16(m, CTRL, 0); mem_write16(m, CTRL, 3);
    CHECK(xfer(m, 1, 0) == 0xff && xfer(m, 0x42, 0) == 0x41, "deasserting CS restarts address selection");
    begin(m, 0, 0); mem_write16(m, BAUD, 273); mem_write8(m, DATA, 1); advance(m, 272 * 8 - 1);
    CHECK(!(mem_read32(m, STAT) & 2), "odd baud reload rounds down and scales byte duration"); advance(m, 1);
    CHECK(mem_read32(m, STAT) & 2, "slower baud byte arrives at the configured cycle");
    begin(m, 0, 0); mem_write16(m, CTRL, 2); mem_write8(m, DATA, 1); advance(m, BYTE_CYCLES * 2);
    CHECK(!(mem_read32(m, STAT) & 2), "TX-disabled byte waits in its holding register"); mem_write16(m, CTRL, 3); advance(m, BYTE_CYCLES);
    CHECK(mem_read8(m, DATA) == 0xff, "enabling TX starts its queued byte");
    begin(m, 0, 0); mem_write8(m, DATA, 1); mem_write8(m, DATA, 0x42);
    CHECK(!(mem_read32(m, STAT) & 5), "one shifting byte plus one holding byte fills the TX direction");
    mem_write16(m, CTRL, 2); advance(m, BYTE_CYCLES);
    CHECK(mem_read8(m, DATA) == 0xff && (mem_read32(m, STAT) & 5) == 1, "latched TX enable starts the held byte after TXEN is cleared"); advance(m, BYTE_CYCLES);
    CHECK(mem_read8(m, DATA) == 0x41 && mem_read32(m, STAT) & 4, "queued transfer completes at the next character boundary");
    begin(m, 0, 0); mem_write16(m, CTRL, 0x0903); mem_write8(m, DATA, 1); advance(m, BYTE_CYCLES + 500);
    CHECK(!(mem_read32(m, STAT) & 0x200), "two-byte RX interrupt threshold ignores a single FIFO byte");
    mem_write8(m, DATA, 0x42); advance(m, BYTE_CYCLES + 500);
    CHECK(mem_read32(m, STAT) & 0x200 && m->i_stat & 1u << IRQ_SIO0, "RX threshold raises IRQ when second byte arrives");
    mem_write32(m, 0x1f801070, ~(1u << IRQ_SIO0)); mem_write16(m, CTRL, 0x0913);
    CHECK(m->i_stat & 1u << IRQ_SIO0, "acknowledging an active RX threshold creates another IRQ edge"); mem_read8(m, DATA);
    mem_write16(m, CTRL, 0x0913); CHECK(!(mem_read32(m, STAT) & 0x200), "RX IRQ clears after FIFO drops below threshold");
    begin(m, 0, 0); mem_write16(m, CTRL, 5); mem_write8(m, DATA, 0); advance(m, BYTE_CYCLES);
    CHECK(mem_read8(m, DATA) == 0xff && !(mem_read16(m, CTRL) & 4), "RXEN forces one unselected receive and then clears itself");
    mem_write8(m, DATA, 0); advance(m, BYTE_CYCLES);
    CHECK(!(mem_read32(m, STAT) & 2), "no selected port and no RXEN discards incoming data");

    begin(m, 0, 0);
    for (int i = 0; i < 5; i++) { mem_write8(m, DATA, (const uint8_t[]){1, 0x42, 0, 0, 0}[i]); advance(m, BYTE_CYCLES + 500); }
    CHECK(mem_read16(m, DATA) == 0x41ff && mem_read8(m, DATA) == 0x41, "halfword DATA read previews two bytes but pops one");
    CHECK(mem_read32(m, DATA) == 0x00bfe75a && !(mem_read32(m, STAT) & 2), "word DATA read pops four FIFO bytes");
    mem_write16(m, CTRL, 0x40); mem_write32(m, MODE, 0x0003000d);
    CHECK(mem_read32(m, MODE) == 0x0003000d, "word access covers MODE and CTRL without losing register lanes");
    mem_write16(m, CTRL, 0x40); CHECK(mem_read16(m, MODE) == 0 && mem_read16(m, CTRL) == 0 && !(mem_read32(m, STAT) & 2), "port reset clears registers and FIFOs");

    for (int i = 0; i < CARD_SIZE; i++) image[i] = (uint8_t)(i * 11 + i / 128);
    CHECK(sio_card_insert(m, 0, image, CARD_SIZE) == 0 && sio_card_insert(m, 1, image, CARD_SIZE - 1) < 0, "card insertion validates exact image size");
    begin(m, 0, 1); card_start(m, 0x52, 7);
    mem_write32(m, 0x1f801070, ~(1u << IRQ_SIO0)); mem_write16(m, CTRL, 0x1013); mem_write8(m, DATA, 0); advance(m, BYTE_CYCLES);
    CHECK(mem_read8(m, DATA) == 0x5c, "seventh card-read byte arrives before the long ACK delay"); advance(m, 31399);
    CHECK(!(mem_read32(m, STAT) & 0x80) && !(m->i_stat & 1u << IRQ_SIO0), "card-read delay postpones ACK by an extra 31000 cycles"); advance(m, 1);
    CHECK(mem_read32(m, STAT) & 0x80 && m->i_stat & 1u << IRQ_SIO0, "late card-read ACK reaches IRQ7"); advance(m, 100);
    CHECK(xfer(m, 0, 0) == 0x5d && xfer(m, 0, 0) == 0 && xfer(m, 0, 0) == 7, "read confirms the requested sector address");
    uint8_t sum = 7; int same = 1;
    for (int i = 0; i < CARD_SECTOR; i++) { uint8_t r = xfer(m, 0, 0); if (r != image[7 * CARD_SECTOR + i]) same = 0; sum ^= r; }
    CHECK(same && xfer(m, 0, 0) == sum && xfer(m, 0, 0) == 0x47, "card read returns 128 bytes, address/data XOR checksum and good end flag");
    CHECK(m->sio.card[0].flag == 8, "reading a card does not clear its insertion flag");
    uint8_t bytes[CARD_SECTOR]; for (int i = 0; i < CARD_SECTOR; i++) bytes[i] = (uint8_t)(255 - i);
    CHECK(card_write(m, 7, bytes, 0) == 0x47 && !memcmp(m->sio.card[0].data + 7 * CARD_SECTOR, bytes, CARD_SECTOR), "valid card write updates only its addressed sector");
    CHECK(m->sio.card[0].flag == 0 && m->sio.card[0].dirty && m->sio.card[0].data[6 * CARD_SECTOR] == image[6 * CARD_SECTOR], "write clears insertion flag, marks dirty and preserves neighboring sectors");
    memset(bytes, 0x6b, sizeof bytes);
    CHECK(card_write(m, 7, bytes, 1) == 0x4e && m->sio.card[0].data[7 * CARD_SECTOR] == 0xff, "bad write checksum returns 4Eh and preserves previous data");
    CHECK(card_write(m, 1024, bytes, 0) == 0xff && m->sio.card[0].data[0] == image[0], "invalid write sector is rejected without address wrapping");
    begin(m, 0, 0); card_start(m, 0x52, 1024);
    CHECK(xfer(m, 0, 31000) == 0x5c && xfer(m, 0, 0) == 0x5d && xfer(m, 0, 0) == 0xff && xfer(m, 0, 0) == 0xff, "invalid read sector confirms FFFFh then aborts");
    CHECK(xfer(m, 0, 0) == 0xff, "invalid read does not expose wrapped card data");
    begin(m, 0, 0); card_start(m, 0x53, 0); uint8_t id[6]; for (int i = 0; i < 6; i++) id[i] = xfer(m, 0, 0);
    CHECK(!memcmp(id, (const uint8_t[]){0x5c, 0x5d, 4, 0, 0, 0x80}, sizeof id), "Sony card ID reports sector count and size");
    begin(m, 0, 0); const uint8_t query[10] = {0x81, 0x53};
    for (int i = 0; i < 10; i++) { mem_write8(m, DATA, query[i]); advance(m, BYTE_CYCLES + 500); }
    uint8_t fifo[8]; for (int i = 0; i < 8; i++) fifo[i] = mem_read8(m, DATA);
    CHECK(!memcmp(fifo, (const uint8_t[]){0xff, 0, 0x5a, 0x5d, 0x5c, 0x5d, 4, 0x80}, sizeof fifo) && !(mem_read32(m, STAT) & 0x12), "RX overrun preserves oldest seven bytes and replaces only newest, without an SIO0 overrun flag");
    ps1_reset(m); CHECK(m->sio.pad[0].present && m->sio.card[0].present && m->sio.card[0].data[7 * CARD_SECTOR] == 0xff, "machine reset preserves plugged pads and card contents");
    sio_card_remove(m, 0); begin(m, 0, 1); xfer(m, 0x81, 0); CHECK(!(m->i_stat & 1u << IRQ_SIO0), "removed card sends no ACK");
    sio_card_format(m, 1); uint8_t *c = m->sio.card[1].data;
    CHECK(c[0] == 'M' && c[1] == 'C' && c[127] == 0x0e && c[128] == 0xa0 && c[128 + 127] == 0xa0 && !memcmp(c + 16 * CARD_SECTOR, "\xff\xff\xff\xff", 4), "formatted card has header, free directory and no broken sectors");
    CHECK(!other->sio.card[0].present && !other->sio.card[1].present, "card insertion and formatting stay local to one machine");
    ps1_free(m); ps1_free(other); free(image); printf("sio: %d checks, %d failed\n", checks, fails); return fails != 0;
}
