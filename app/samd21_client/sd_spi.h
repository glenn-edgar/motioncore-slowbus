#ifndef SD_SPI_H
#define SD_SPI_H
// microSD card over SERCOM0 SPI on the XIAO Expansion Base:
//   SCK = PA07 (D8)   MISO = PA05 (D9)   MOSI = PA06 (D10)   CS = PA10 (D2, active-low)
// SERCOM0 PMUX function D (PA05=PAD1/MISO, PA06=PAD2/MOSI, PA07=PAD3/SCK) -> DOPO=1, DIPO=1.
// Fixed 512-byte blocks (SDHC/SDXC block-addressed; SDSC byte-addressed, set via CMD16).
// Every wait is bounded so a stuck/absent card can never hang the gateway.
#include <stdint.h>
#include <stdbool.h>

bool sd_init(void);                                     // init SPI bus + card; true on success
bool sd_read_block(uint32_t lba, uint8_t *buf);         // read one 512-byte block
bool sd_write_block(uint32_t lba, const uint8_t *buf);  // write one 512-byte block
bool sd_ready(void);                                    // true once a card is initialised

#endif // SD_SPI_H
