// FatFs disk I/O glue -> the SD-SPI driver (sd_spi.c). Single drive (pdrv 0).
#include <stddef.h>
#include "ff.h"
#include "diskio.h"
#include "../sd_spi.h"

#define DEV_SD  0
#define SD_SS   512u

DSTATUS disk_status(BYTE pdrv) {
    if (pdrv != DEV_SD) return STA_NOINIT;
    return sd_ready() ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv) {
    if (pdrv != DEV_SD) return STA_NOINIT;
    return sd_init() ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv != DEV_SD || !sd_ready()) return RES_NOTRDY;
    for (UINT i = 0; i < count; i++)
        if (!sd_read_block((uint32_t)(sector + i), buff + (size_t)i * SD_SS)) return RES_ERROR;
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv != DEV_SD || !sd_ready()) return RES_NOTRDY;
    for (UINT i = 0; i < count; i++)
        if (!sd_write_block((uint32_t)(sector + i), buff + (size_t)i * SD_SS)) return RES_ERROR;
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    (void)buff;
    if (pdrv != DEV_SD) return RES_PARERR;
    // CTRL_SYNC is a no-op: sd_write_block already polls the card to not-busy
    // before returning. GET_SECTOR_COUNT/SIZE/BLOCK only matter to f_mkfs (FF_USE_MKFS=0),
    // and FF_MIN_SS==FF_MAX_SS==512 so FatFs never queries the sector size.
    return (cmd == CTRL_SYNC) ? RES_OK : RES_PARERR;
}

// STUB until the RTC binding lands: fixed 2026-01-01 00:00:00.
// FAT time = year-1980<<25 | month<<21 | day<<16 | hour<<11 | min<<5 | sec/2.
DWORD get_fattime(void) {
    return ((DWORD)(2026 - 1980) << 25) | ((DWORD)1 << 21) | ((DWORD)1 << 16);
}
