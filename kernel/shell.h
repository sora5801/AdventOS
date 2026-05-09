#ifndef ADVENTOS_SHELL_H
#define ADVENTOS_SHELL_H

#include "../include/types.h"

void shell_run(void) __attribute__((noreturn));

/* Exposed for SYS_READ_LINE and SYS_KCMD: */
int  kshell_read_line(char *buf, int cap);
void kshell_run_line (char *line);

#endif
