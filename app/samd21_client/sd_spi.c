// microSD over SERCOM0 SPI (XIAO base). See sd_spi.h for the pin map.
// SD SPI-mode protocol: CMD0 -> CMD8 -> ACMD41 -> CMD58(OCR/CCS), then CMD17/CMD24.
#include "sd_spi.h"
#include "samd21.h"
#include "samd21_sync.h"            // BOUNDED_SPIN

#define SD_SERCOM      SERCOM0
#define SD_GCLK_CORE   SERCOM0_GCLK_ID_CORE
#define SD_CS          10u          // PA10 = D2 (active low)
#define SD_MISO         5u          // PA05  SERCOM0/PAD1
#define SD_MOSI         6u          // PA06  SERCOM0/PAD2
#define SD_SCK          7u          // PA07  SERCOM0/PAD3

// BAUD = GCLK0(48 MHz) / (2*fSCK) - 1
#define SD_BAUD_SLOW   59u          // ~400 kHz for card init
#define SD_BAUD_FAST    3u          // ~6 MHz for data

// Bounded poll budgets (iterations of a 1-byte SPI exchange). Generous vs real
// card timings (ACMD41 init <~1 s; block-write busy <~250 ms) yet always finite.
#define SD_INIT_TRIES   40000u
#define SD_TOKEN_TRIES  100000u
#define SD_BUSY_TRIES   500000u

static bool s_ready;
static bool s_sdhc;                 // true = block-addressed (SDHC/SDXC)

static inline void cs_high(void) { PORT->Group[0].OUTSET.reg = (1u << SD_CS); }
static inline void cs_low (void) { PORT->Group[0].OUTCLR.reg = (1u << SD_CS); }

static uint8_t spi_xfer(uint8_t b) {
    SD_SERCOM->SPI.DATA.reg = b;
    BOUNDED_SPIN(!SD_SERCOM->SPI.INTFLAG.bit.RXC);   // wait for the exchanged byte
    return (uint8_t)SD_SERCOM->SPI.DATA.reg;
}

static void spi_set_baud(uint8_t baud) {
    SD_SERCOM->SPI.CTRLA.bit.ENABLE = 0;
    BOUNDED_SPIN(SD_SERCOM->SPI.SYNCBUSY.bit.ENABLE);
    SD_SERCOM->SPI.BAUD.reg = baud;
    SD_SERCOM->SPI.CTRLA.bit.ENABLE = 1;
    BOUNDED_SPIN(SD_SERCOM->SPI.SYNCBUSY.bit.ENABLE);
}

static void spi_bus_init(void) {
    PM->APBCMASK.reg |= PM_APBCMASK_SERCOM0;
    GCLK->CLKCTRL.reg = (uint16_t)(GCLK_CLKCTRL_ID(SD_GCLK_CORE)
                                 | GCLK_CLKCTRL_GEN_GCLK0 | GCLK_CLKCTRL_CLKEN);
    BOUNDED_SPIN(GCLK->STATUS.bit.SYNCBUSY);

    SD_SERCOM->SPI.CTRLA.bit.SWRST = 1;
    BOUNDED_SPIN(SD_SERCOM->SPI.SYNCBUSY.bit.SWRST);

    // master, MSB-first, mode 0; MOSI=PAD2 & SCK=PAD3 (DOPO=1), MISO=PAD1 (DIPO=1)
    SD_SERCOM->SPI.CTRLA.reg = SERCOM_SPI_CTRLA_MODE_SPI_MASTER
                             | SERCOM_SPI_CTRLA_DOPO(1)
                             | SERCOM_SPI_CTRLA_DIPO(1);
    SD_SERCOM->SPI.CTRLB.reg = SERCOM_SPI_CTRLB_RXEN;   // 8-bit char, receiver on
    BOUNDED_SPIN(SD_SERCOM->SPI.SYNCBUSY.bit.CTRLB);
    SD_SERCOM->SPI.BAUD.reg = SD_BAUD_SLOW;

    // PMUX PA05/PA06/PA07 -> function D (SERCOM0). PA05/PA07 odd, PA06 even.
    PORT->Group[0].PINCFG[SD_MISO].bit.PMUXEN = 1;
    PORT->Group[0].PMUX[SD_MISO >> 1].bit.PMUXO = PORT_PMUX_PMUXO_D_Val;
    PORT->Group[0].PINCFG[SD_MOSI].bit.PMUXEN = 1;
    PORT->Group[0].PMUX[SD_MOSI >> 1].bit.PMUXE = PORT_PMUX_PMUXE_D_Val;
    PORT->Group[0].PINCFG[SD_SCK ].bit.PMUXEN = 1;
    PORT->Group[0].PMUX[SD_SCK  >> 1].bit.PMUXO = PORT_PMUX_PMUXO_D_Val;

    PORT->Group[0].DIRSET.reg = (1u << SD_CS);   // CS as output, deselected
    cs_high();

    SD_SERCOM->SPI.CTRLA.bit.ENABLE = 1;
    BOUNDED_SPIN(SD_SERCOM->SPI.SYNCBUSY.bit.ENABLE);
}

// Send a command + 32-bit arg + CRC; return R1 (0x00 = OK, 0x01 = idle, 0xFF = no reply).
static uint8_t sd_cmd(uint8_t cmd, uint32_t arg, uint8_t crc) {
    spi_xfer(0xFF);
    spi_xfer((uint8_t)(0x40 | cmd));
    spi_xfer((uint8_t)(arg >> 24)); spi_xfer((uint8_t)(arg >> 16));
    spi_xfer((uint8_t)(arg >> 8));  spi_xfer((uint8_t)arg);
    spi_xfer(crc);
    uint8_t r1 = 0xFF;
    for (int i = 0; i < 10 && (r1 & 0x80); i++) r1 = spi_xfer(0xFF);
    return r1;
}

bool sd_init(void) {
    s_ready = false; s_sdhc = false;
    spi_bus_init();
    spi_set_baud(SD_BAUD_SLOW);

    cs_high();
    for (int i = 0; i < 10; i++) spi_xfer(0xFF);    // >=74 clocks with CS high to wake
    cs_low();

    uint8_t r1 = 0xFF;
    for (int t = 0; t < 10 && r1 != 0x01; t++) r1 = sd_cmd(0, 0, 0x95);   // CMD0 -> idle
    if (r1 != 0x01) { cs_high(); return false; }

    r1 = sd_cmd(8, 0x000001AAu, 0x87);                                    // CMD8 (v2 check)
    if (r1 == 0x01) {
        uint8_t b[4]; for (int i = 0; i < 4; i++) b[i] = spi_xfer(0xFF);
        if (b[2] != 0x01 || b[3] != 0xAA) { cs_high(); return false; }    // voltage/echo mismatch
    }

    r1 = 0xFF;                                                            // ACMD41 (HCS) init
    for (uint32_t t = 0; t < SD_INIT_TRIES && r1 != 0x00; t++) {
        sd_cmd(55, 0, 0x65);
        r1 = sd_cmd(41, 0x40000000u, 0x77);
    }
    if (r1 != 0x00) { cs_high(); return false; }

    if (sd_cmd(58, 0, 0xFD) == 0x00) {                                    // CMD58 read OCR
        uint8_t ocr[4]; for (int i = 0; i < 4; i++) ocr[i] = spi_xfer(0xFF);
        s_sdhc = (ocr[0] & 0x40) != 0;                                    // CCS (bit30)
    }
    if (!s_sdhc) sd_cmd(16, 512, 0xFF);                                   // SDSC: 512-byte blocks

    cs_high(); spi_xfer(0xFF);
    spi_set_baud(SD_BAUD_FAST);
    s_ready = true;
    return true;
}

bool sd_read_block(uint32_t lba, uint8_t *buf) {
    if (!s_ready) return false;
    uint32_t addr = s_sdhc ? lba : lba * 512u;
    cs_low();
    if (sd_cmd(17, addr, 0xFF) != 0x00) { cs_high(); return false; }
    uint8_t tok = 0xFF;
    for (uint32_t i = 0; i < SD_TOKEN_TRIES && tok == 0xFF; i++) tok = spi_xfer(0xFF);
    if (tok != 0xFE) { cs_high(); return false; }                        // data start token
    for (int i = 0; i < 512; i++) buf[i] = spi_xfer(0xFF);
    spi_xfer(0xFF); spi_xfer(0xFF);                                       // discard CRC
    cs_high(); spi_xfer(0xFF);
    return true;
}

bool sd_write_block(uint32_t lba, const uint8_t *buf) {
    if (!s_ready) return false;
    uint32_t addr = s_sdhc ? lba : lba * 512u;
    cs_low();
    if (sd_cmd(24, addr, 0xFF) != 0x00) { cs_high(); return false; }
    spi_xfer(0xFF);                                                       // 1-byte gap
    spi_xfer(0xFE);                                                       // data start token
    for (int i = 0; i < 512; i++) spi_xfer(buf[i]);
    spi_xfer(0xFF); spi_xfer(0xFF);                                       // dummy CRC
    if ((spi_xfer(0xFF) & 0x1F) != 0x05) { cs_high(); return false; }     // data response: accepted?
    uint8_t busy = 0x00;                                                  // card holds MISO low while writing
    for (uint32_t i = 0; i < SD_BUSY_TRIES && busy != 0xFF; i++) busy = spi_xfer(0xFF);
    cs_high(); spi_xfer(0xFF);
    return busy == 0xFF;
}

bool sd_ready(void) { return s_ready; }
