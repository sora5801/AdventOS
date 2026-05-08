#ifndef ADVENTOS_PIT_H
#define ADVENTOS_PIT_H

#include "../include/types.h"

void     pit_init(uint32_t hz);
uint32_t pit_ticks(void);
uint32_t pit_seconds(void);
void     pit_sleep(uint32_t ms);

#endif
