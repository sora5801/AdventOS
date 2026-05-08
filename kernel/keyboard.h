#ifndef ADVENTOS_KEYBOARD_H
#define ADVENTOS_KEYBOARD_H

#include "../include/types.h"

void keyboard_init(void);

/* Returns 0 if no key is buffered, otherwise the next ASCII char (or special). */
char keyboard_getc(void);
int  keyboard_has_char(void);

/* Blocks until a key arrives (uses HLT between checks). */
char keyboard_wait_char(void);

#endif
