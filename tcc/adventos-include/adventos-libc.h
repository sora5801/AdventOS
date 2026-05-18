/*
 * tcc cross-compile to AdventOS — single freestanding-libc header
 * pre-included via gcc's `-include` flag.  Every system-header stub
 * in this directory just re-includes the libuser surface to keep the
 * declarations consistent.
 *
 * This header gives tcc:
 *   - everything in user/libuser.h (FILE *, fopen, malloc, str*, etc.)
 *   - common typedefs not in libuser.h (ptrdiff_t, ssize_t, off_t,
 *     int64_t, uint64_t, ...)
 *   - <limits.h> macros (INT_MAX etc.) that tcc references
 *   - errno value names tcc references (ENOENT, EIO, ...)
 *   - PATH_MAX, NAME_MAX, PAGESIZE
 *   - a minimal `struct stat` that tcc's portability layer references
 *     (we don't actually call stat — fopen + fclose are sufficient
 *     for file-existence checks).
 *
 * Anything tcc requests that ISN'T in here will produce a compile-time
 * error, which is the desired behavior — better than silently linking
 * against a missing symbol and crashing at runtime.
 */
#ifndef TCC_ADVENTOS_LIBC_H
#define TCC_ADVENTOS_LIBC_H

#include "libuser.h"

#ifndef NULL
#define NULL ((void *)0)
#endif

/* Common typedefs tcc expects from various stdint/stddef/sys/types.
 * time_t comes from libuser.h (must match for the `time()` prototype). */
typedef long          ptrdiff_t;
typedef int           ssize_t;
typedef long          off_t;
typedef long long     int64_t;
typedef unsigned long long uint64_t;
typedef int           pid_t;
typedef int           mode_t;
typedef unsigned int  uintptr_t;
typedef int           intptr_t;

/* struct tm — tcc's __DATE__/__TIME__ predef computation uses it.
 * We provide a no-op localtime() that fills these with fixed values
 * since AdventOS has no real wall-clock RTC. */
struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};
static inline struct tm *localtime(const time_t *t) {
    static struct tm g_tm;
    (void)t;
    g_tm.tm_sec = 0; g_tm.tm_min = 0; g_tm.tm_hour = 0;
    g_tm.tm_mday = 1; g_tm.tm_mon = 0; g_tm.tm_year = 126;  /* 2026 */
    g_tm.tm_wday = 0; g_tm.tm_yday = 0; g_tm.tm_isdst = 0;
    return &g_tm;
}

/* <limits.h> */
#define INT_MAX     2147483647
#define INT_MIN     (-INT_MAX - 1)
#define UINT_MAX    0xFFFFFFFFu
#define LONG_MAX    2147483647L
#define LONG_MIN    (-LONG_MAX - 1L)
#define ULONG_MAX   0xFFFFFFFFul
#define CHAR_MAX    127
#define CHAR_MIN    (-128)
#define UCHAR_MAX   0xFFu
#define SCHAR_MAX   127
#define SCHAR_MIN   (-128)
#define SHRT_MAX    32767
#define SHRT_MIN    (-32768)
#define USHRT_MAX   0xFFFFu
#define LLONG_MAX   0x7FFFFFFFFFFFFFFFLL
#define LLONG_MIN   (-LLONG_MAX - 1LL)
#define ULLONG_MAX  0xFFFFFFFFFFFFFFFFULL
#define CHAR_BIT    8
#define PATH_MAX    256
#define NAME_MAX    64
#define MB_LEN_MAX  1
#define PAGESIZE    4096
#define _SC_PAGESIZE 30
/* tcc uses PATH_SEPARATOR for argv assembly — Unix-flavored. */
#define PATH_SEPARATOR ':'

/* <errno.h> */
#define ENOENT      2
#define EIO         5
#define EBADF       9
#define ENOMEM     12
#define EACCES     13
#define EEXIST     17
#define EINVAL     22
#define ENOSPC     28
#define ERANGE     34
#define ENOTSUP    95

/* <sys/stat.h> — tcc references the bits but never reads the result. */
struct stat {
    long st_dev;
    long st_ino;
    long st_mode;
    long st_nlink;
    long st_uid;
    long st_gid;
    long st_rdev;
    long st_size;
    long st_mtime;
};
#define S_IFREG    0100000
#define S_IFDIR    0040000
#define S_IFMT     0170000
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
static inline int stat(const char *path, struct stat *st) {
    int sz = sys_fs_size(path);
    if (sz < 0) return -1;
    if (st) { st->st_size = sz; st->st_mode = S_IFREG | 0644; }
    return 0;
}

/* <signal.h> — used in tcc's -bt backtrace code which we don't enable.
 * Provide stubs so the headers parse. */
typedef int sig_atomic_t;
#define SA_SIGINFO  0
#define SIGSEGV     11
#define SIGABRT      6

/* assert.h — tcc uses assert() in a few places. Make it abort. */
#define assert(expr)  ((void)((expr) || (abort(), 0)))

/* tcc.h's non-Windows branch declares these as `extern float strtof(...);
 * extern long double strtold(...);` so the user code can parse 1.0f /
 * 1.0L literals. AdventOS has no floating-point runtime; provide stubs
 * that return 0. Linker pulls these in only if the build actually
 * references them, which the i386 backend mostly does not at compile
 * time. Sufficient to silence missing-symbol errors. */
static inline float        strtof (const char *s, char **e) { (void)s; if (e) *e=(char*)s; return 0.0f; }
static inline long double  strtold(const char *s, char **e) { (void)s; if (e) *e=(char*)s; return 0.0L; }
static inline double       strtod (const char *s, char **e) { (void)s; if (e) *e=(char*)s; return 0.0; }

/* strtoul / strtoull — cast through the signed versions; AdventOS
 * code never feeds these values past INT_MAX in practice. */
static inline unsigned long      strtoul (const char *s, char **e, int b)
    { return (unsigned long)strtol(s, e, b); }
static inline unsigned long long strtoull(const char *s, char **e, int b)
    { return (unsigned long long)strtoll(s, e, b); }

/* strpbrk — span until first char in accept; tcc uses for opt parsing. */
static inline char *strpbrk(const char *s, const char *accept) {
    for (; *s; s++) {
        for (const char *a = accept; *a; a++) {
            if (*s == *a) return (char *)s;
        }
    }
    return (char *)0;
}

/* tcc's tccdbg.c does `char *p = strchr(name, ':'); *p = 0;` — wants
 * a non-const result. libuser declares strchr/strrchr/strstr returning
 * `const char *` (matches C standard). Override with macros that cast. */
#define strchr(s,c)   ((char *)strchr ((s),(c)))
#define strrchr(s,c)  ((char *)strrchr((s),(c)))
#define strstr(h,n)   ((char *)strstr ((h),(n)))

/* ldexpl — tcc uses for float-literal scaling; we don't emit floats. */
static inline long double ldexpl(long double x, int exp) { (void)x; (void)exp; return 0.0L; }

/* getcwd — wrap sys_getcwd. */
static inline char *getcwd(char *buf, int sz) {
    if (sys_getcwd(buf, sz) < 0) return (char *)0;
    return buf;
}

/* realpath — AdventOS has a flat /, no symlinks, no /.. resolution.
 * Return a copy of the input. tcc uses this for include-path canonical-
 * ization; a no-op is harmless. */
static inline char *realpath(const char *path, char *resolved) {
    if (!path) return (char *)0;
    if (resolved) {
        int i = 0;
        while (i < PATH_MAX - 1 && path[i]) { resolved[i] = path[i]; i++; }
        resolved[i] = 0;
        return resolved;
    }
    return (char *)path;
}

/* freopen — tcc only calls it for tccrun/-run output redirection,
 * which we disable. Return NULL. */
static inline FILE *freopen(const char *path, const char *mode, FILE *f) {
    (void)path; (void)mode; (void)f; return (FILE *)0;
}

/* execvp — tcc uses for the `tcc -ar` archive driver. Stub fails. */
static inline int execvp(const char *file, char *const argv[]) {
    (void)file; (void)argv; return -1;
}

/* environ — tcc references for printenv-style logging.  Empty by
 * convention. */
extern char **environ;

/* mmap/mprotect stubs — only referenced by tccrun.c (-run mode) which
 * we disable via CONFIG_TCC_RUN below. */
#define PROT_NONE   0
#define PROT_READ   1
#define PROT_WRITE  2
#define PROT_EXEC   4
#define MAP_PRIVATE 2
#define MAP_ANON    0x20
#define MAP_FAILED  ((void *)-1)
static inline int mprotect(void *addr, size_t len, int prot) {
    (void)addr; (void)len; (void)prot; return 0;
}
static inline void *mmap(void *addr, size_t len, int prot, int flags, int fd, long off) {
    (void)addr; (void)len; (void)prot; (void)flags; (void)fd; (void)off;
    return MAP_FAILED;
}
static inline int munmap(void *addr, size_t len) { (void)addr; (void)len; return 0; }

/* stdbool.h equivalent for files that include <stdbool.h>.  tcc's own
 * include/stdbool.h serves this purpose at runtime, but during host
 * cross-compile we use whichever is first on the path. */

/* fcntl-flag values match what's already in libuser.h via O_*. */

#endif
