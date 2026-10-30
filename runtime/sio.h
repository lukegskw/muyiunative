// SIO0 and its external digital controllers/128KB cards (plan.md, phase 2). psx-spx: Controllers and Memory Cards; Serial Interfaces (SIO)
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { CARD_SIZE = 128 * 1024, CARD_SECTOR = 128 };
enum { PAD_SELECT = 1u << 0, PAD_START = 1u << 3, PAD_UP = 1u << 4, PAD_RIGHT = 1u << 5, PAD_DOWN = 1u << 6, PAD_LEFT = 1u << 7,
       PAD_L2 = 1u << 8, PAD_R2 = 1u << 9, PAD_L1 = 1u << 10, PAD_R1 = 1u << 11, PAD_TRIANGLE = 1u << 12, PAD_CIRCLE = 1u << 13, PAD_CROSS = 1u << 14, PAD_SQUARE = 1u << 15 };
typedef struct ps1_pad { uint16_t pressed; uint8_t present; } ps1_pad;
typedef struct ps1_card { uint8_t data[CARD_SIZE], present, flag, dirty; } ps1_card;
typedef struct ps1_sio {
    uint16_t mode, ctrl, baud;
    uint8_t rx[8], rpos, nrx, tx, hold, pending, latched, active, tx_port, selected, irq, ack, want_ack;
    uint64_t next, done, ack_at, ack_end, baud_at;
    uint8_t device, cmd, prev, checksum, result, write[CARD_SECTOR], reported;
    uint16_t step, addr, buttons;
    ps1_pad pad[2]; ps1_card card[2];   // reset keeps the plugged devices and card contents
} ps1_sio;
struct ps1;
void sio_reset(struct ps1 *m);
void sio_event(struct ps1 *m);
uint32_t sio_read(struct ps1 *m, uint32_t off, unsigned n);
void sio_write(struct ps1 *m, uint32_t off, uint32_t v, unsigned n);
void sio_set_pad(struct ps1 *m, int port, int connected, uint16_t pressed);   // port 0/1; pressed=1 on host, pressed=0 on the wire
int sio_card_insert(struct ps1 *m, int port, const uint8_t *image, size_t n);   // copy exactly 128KB into the machine, no file access; 0/-1
void sio_card_remove(struct ps1 *m, int port);
void sio_card_format(struct ps1 *m, int port);   // insert a newly formatted in-memory card; never writes a host file
