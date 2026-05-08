#ifndef ADVENTOS_ATA_H
#define ADVENTOS_ATA_H

#include "../include/types.h"

#define ATA_SECTOR_SIZE 512

/* Polled-PIO driver for the primary ATA controller, master drive only.
 * No IRQs (the controller's IRQ line is masked at init). */

void ata_init(void);

/* Returns 0 on success, -1 on a status-error or timeout. */
int  ata_read_sector (uint32_t lba, void *buf);
int  ata_write_sector(uint32_t lba, const void *buf);

#endif
