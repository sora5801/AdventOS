/*
 * AdventOS stub — dlfcn.h. tcc references dlopen/dlsym for its `-run`
 * path; we ship CONFIG_TCC_STATIC=1 so the dl* calls are #ifdef'd out
 * before any actual symbol use. This stub just satisfies the include.
 */
#ifndef _ADVENTOS_STUB_DLFCN_H
#define _ADVENTOS_STUB_DLFCN_H

#include "adventos-libc.h"

#define RTLD_NOW   2
#define RTLD_LAZY  1

static inline void *dlopen(const char *path, int mode) {
    (void)path; (void)mode; return (void *)0;
}
static inline void *dlsym(void *h, const char *name) {
    (void)h; (void)name; return (void *)0;
}
static inline int dlclose(void *h) { (void)h; return 0; }
static inline char *dlerror(void) { return (char *)"dlfcn unsupported"; }

#endif
