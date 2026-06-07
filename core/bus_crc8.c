// ============================================================================
// bus_crc8.c — CRC-8/AUTOSAR. See bus_crc8.h.
// ============================================================================
#include "bus_crc8.h"

uint8_t bus_crc8_update(uint8_t crc, uint8_t byte) {
    crc ^= byte;
    for (int i = 0; i < 8; ++i) {
        crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0x2Fu) : (uint8_t)(crc << 1);
    }
    return crc;
}

uint8_t bus_crc8(const uint8_t *data, size_t len) {
    uint8_t c = 0xFFu;
    for (size_t i = 0; i < len; ++i) c = bus_crc8_update(c, data[i]);
    return (uint8_t)(c ^ 0xFFu);   // CRC-8/AUTOSAR final XOR
}
