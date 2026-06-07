// ============================================================================
// bus_frame.c — BC-2 frame codec + reentrant assembler. See bus_frame.h.
// ============================================================================
#include "bus_frame.h"
#include "bus_crc8.h"

uint8_t bus_frame_crc(const bus_frame_t *f) {
    uint8_t c = 0xFFu;
    c = bus_crc8_update(c, f->dest);
    c = bus_crc8_update(c, f->src);
    c = bus_crc8_update(c, f->type);
    c = bus_crc8_update(c, f->seq);
    c = bus_crc8_update(c, f->len);
    for (uint8_t i = 0; i < f->len; ++i) c = bus_crc8_update(c, f->payload[i]);
    return (uint8_t)(c ^ 0xFFu);
}

uint16_t bus_frame_encode(uint16_t *words, const bus_frame_t *f) {
    uint8_t len = f->len;
    if (len > BUS_PAYLOAD_MAX) len = BUS_PAYLOAD_MAX;

    // CRC is over the clamped frame; build a header view with the clamped len.
    bus_frame_t tmp = *f;
    tmp.len = len;
    uint8_t crc = bus_frame_crc(&tmp);

    uint16_t n = 0;
    words[n++] = BUS_PREAMBLE_WORD;            // preamble (data, bit8=0)
    words[n++] = BUS_WORD_ADDR(f->dest);       // dest: address marker (bit8=1)
    words[n++] = BUS_WORD_DATA(f->src);
    words[n++] = BUS_WORD_DATA(f->type);
    words[n++] = BUS_WORD_DATA(f->seq);
    words[n++] = BUS_WORD_DATA(len);
    for (uint8_t i = 0; i < len; ++i) words[n++] = BUS_WORD_DATA(f->payload[i]);
    words[n++] = BUS_WORD_DATA(crc);
    return n;
}

void bus_asm_init(bus_asm_t *a, uint8_t my_addr, bool promiscuous) {
    a->state       = BUS_ASM_IDLE;
    a->idx         = 0;
    a->my_addr     = my_addr;
    a->promiscuous = promiscuous || (my_addr == BUS_ADDR_PROMISCUOUS);
    a->crc_fail    = 0;
    a->frames_ok   = 0;
}

static bool addr_is_for_us(const bus_asm_t *a, uint8_t dest) {
    return a->promiscuous || dest == a->my_addr || dest == BUS_ADDR_BROADCAST;
}

bool bus_asm_feed(bus_asm_t *a, uint16_t word9, bus_frame_t *out) {
    bool    is_addr = BUS_WORD_IS_ADDR(word9);
    uint8_t b       = (uint8_t)(word9 & 0xFFu);

    if (is_addr) {
        // Address word always (re)starts a frame.
        if (addr_is_for_us(a, b)) {
            a->acc.dest = b;
            a->state    = BUS_ASM_SRC;
        } else {
            a->state    = BUS_ASM_IDLE;   // someone else's frame; skip its data
        }
        return false;
    }

    switch (a->state) {
    case BUS_ASM_SRC:  a->acc.src  = b; a->state = BUS_ASM_TYPE; break;
    case BUS_ASM_TYPE: a->acc.type = b; a->state = BUS_ASM_SEQ;  break;
    case BUS_ASM_SEQ:  a->acc.seq  = b; a->state = BUS_ASM_LEN;  break;
    case BUS_ASM_LEN:
        if (b > BUS_PAYLOAD_MAX) { a->state = BUS_ASM_IDLE; break; }
        a->acc.len = b;
        a->idx     = 0;
        a->state   = (b == 0u) ? BUS_ASM_CRC : BUS_ASM_PAYLOAD;
        break;
    case BUS_ASM_PAYLOAD:
        a->acc.payload[a->idx++] = b;
        if (a->idx >= a->acc.len) a->state = BUS_ASM_CRC;
        break;
    case BUS_ASM_CRC: {
        uint8_t calc = bus_frame_crc(&a->acc);
        a->state = BUS_ASM_IDLE;
        if (calc == b) { *out = a->acc; a->frames_ok++; return true; }
        a->crc_fail++;            // mismatch -> drop; resync on next address word
        break;
    }
    case BUS_ASM_IDLE:
    default:
        break;                    // preamble / inter-frame noise
    }
    return false;
}
