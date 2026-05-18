/*
 * AdventOS libc — internal header used WITHIN libc.bin sources.
 *
 * libc.bin is a position-dependent flat binary linked at virtual
 * address LIBC_BASE (0x70000000). The kernel ELF loader maps a
 * fresh copy of libc.bin into every new user process at that VA.
 * The first page contains an EXPORT TABLE — a fixed-order array
 * of function pointers — which user programs reach by indexing
 * into LIBC_BASE.
 *
 * That export table is the entire ABI between user programs and
 * libc. Adding a new function = adding a new entry to the table
 * AND bumping LIBC_VERSION. Removing or reordering entries is a
 * compatibility break (existing user programs would dispatch via
 * stale indices).
 *
 * Headers user programs include (libuser.h) provide trampolines
 * that load LIBC_TABLE[INDEX] and tail-jump there.
 */

#ifndef ADVENTOS_LIBC_H
#define ADVENTOS_LIBC_H

#include <stdarg.h>

/* Tiny portable typedefs to avoid pulling in newlib/glibc headers. */
typedef unsigned int        size_t;
typedef int                 ssize_t;
typedef unsigned int        uint32_t;
typedef int                 int32_t;
typedef unsigned short      uint16_t;
typedef unsigned char       uint8_t;
typedef unsigned long long  uint64_t;
typedef unsigned int        uintptr_t;
typedef int                 intptr_t;

#define NULL  ((void *)0)

/* Fixed virtual address libc is mapped at in every user process. */
#define LIBC_BASE       0x70000000u

/* libc.bin layout:
 *   +0x000  struct libc_header
 *   +0x010  uint32_t libc_exports[LIBC_EXPORT_COUNT]   (each = function VA)
 *   +0x400  .text begins (after the 1KiB export table area)
 */
#define LIBC_MAGIC      0x434C4441u   /* 'ADLC' little-endian */
/* Session 132 (Path B tcc port phase 2): bumped from 1 -> 2. New
 * exports: isxdigit + the FILE * / stdio family (fopen/fclose/fread/
 * fwrite/fseek/ftell/fputs/fputc/vfprintf/ferror/feof/remove/fflush).
 * No existing indices were reordered. */
#define LIBC_VERSION    2u
#define LIBC_EXPORT_COUNT 64u
#define LIBC_HEADER_BYTES 0x10u

/* Export table indices — wire-level ABI. NEVER reorder, only append.
 * Mirrored in user/libuser.h as LIBC_FN_* macros. */
enum {
    LIBC_FN_LIBC_INFO   = 0,    /* (out[3]) -> magic, version, count */

    /* string.h */
    LIBC_FN_STRLEN      = 1,
    LIBC_FN_STRCMP      = 2,
    LIBC_FN_STRNCMP     = 3,
    LIBC_FN_STRCPY      = 4,
    LIBC_FN_STRNCPY     = 5,
    LIBC_FN_STRCAT      = 6,
    LIBC_FN_STRCHR      = 7,
    LIBC_FN_STRRCHR     = 8,
    LIBC_FN_STRSTR      = 9,
    LIBC_FN_MEMSET      = 10,
    LIBC_FN_MEMCPY      = 11,
    LIBC_FN_MEMMOVE     = 12,
    LIBC_FN_MEMCMP      = 13,
    LIBC_FN_MEMCHR      = 14,
    LIBC_FN_STRERROR    = 15,    /* session 132 */

    /* stdlib.h */
    LIBC_FN_ATOI        = 20,
    LIBC_FN_ATOL        = 21,
    LIBC_FN_STRTOL      = 22,
    LIBC_FN_ABS         = 23,
    LIBC_FN_MALLOC      = 24,
    LIBC_FN_FREE        = 25,
    LIBC_FN_CALLOC      = 26,
    LIBC_FN_REALLOC     = 27,
    LIBC_FN_QSORT       = 28,    /* session 132 */
    LIBC_FN_STRTOLL     = 29,    /* session 132 */

    /* ctype.h */
    LIBC_FN_ISALPHA     = 30,
    LIBC_FN_ISDIGIT     = 31,
    LIBC_FN_ISSPACE     = 32,
    LIBC_FN_ISALNUM     = 33,
    LIBC_FN_ISUPPER     = 34,
    LIBC_FN_ISLOWER     = 35,
    LIBC_FN_TOUPPER     = 36,
    LIBC_FN_TOLOWER     = 37,
    LIBC_FN_ISXDIGIT    = 38,    /* session 132 */

    /* stdio.h */
    LIBC_FN_PUTCHAR     = 40,
    LIBC_FN_PUTS        = 41,
    LIBC_FN_VPRINTF     = 42,    /* va_list version; varargs printf in shim */
    LIBC_FN_VSPRINTF    = 43,
    LIBC_FN_VSNPRINTF   = 44,
    /* session 132 — FILE * + stdio layer for the tcc port. */
    LIBC_FN_VFPRINTF    = 45,    /* va_list version; varargs fprintf in shim */
    LIBC_FN_FOPEN       = 46,
    LIBC_FN_FCLOSE      = 47,
    LIBC_FN_FREAD       = 48,
    LIBC_FN_FWRITE      = 49,

    /* malloc introspection (debug) */
    LIBC_FN_MALLOC_BRK   = 50,
    LIBC_FN_MALLOC_USED  = 51,
    LIBC_FN_MALLOC_FREE_ = 52,
    LIBC_FN_MALLOC_TOTAL = 53,

    /* session 132 — rest of the FILE * surface. */
    LIBC_FN_FSEEK       = 54,
    LIBC_FN_FTELL       = 55,
    LIBC_FN_FPUTS       = 56,
    LIBC_FN_FPUTC       = 57,
    LIBC_FN_FERROR      = 58,
    LIBC_FN_FEOF        = 59,
    LIBC_FN_REMOVE      = 60,
    LIBC_FN_FFLUSH      = 61,
    LIBC_FN_FGETS       = 62,
    LIBC_FN_FGETC       = 63,
};

/* Layout of the header at +0x000 of libc.bin / VA LIBC_BASE. */
struct libc_header {
    uint32_t magic;
    uint32_t version;
    uint32_t export_count;
    uint32_t reserved;
};

/* Internal: int 0x80 syscall numbers libc uses. Mirrors kernel/syscall.h.
 * Kept here so libc doesn't need to grow its own syscall.h dependency. */
#define LIBC_SYS_WRITE        1
#define LIBC_SYS_EXIT         3
#define LIBC_SYS_WRITE_FD    12
#define LIBC_SYS_BRK         27
/* Session 132 — needed by the FILE * layer (fopen/fread/fclose etc.). */
#define LIBC_SYS_OPEN        10
#define LIBC_SYS_READ        11
#define LIBC_SYS_CLOSE       13
#define LIBC_SYS_FS_WRITE    31
#define LIBC_SYS_FS_SIZE     85
#define LIBC_SYS_UNLINK      84
#define LIBC_SYS_OPEN_W      23

/* string.h prototypes */
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *d, const char *s);
char  *strncpy(char *d, const char *s, size_t n);
char  *strcat(char *d, const char *s);
const char *strchr(const char *s, int c);
const char *strrchr(const char *s, int c);
const char *strstr(const char *h, const char *n);
void  *memset(void *p, int c, size_t n);
void  *memcpy(void *d, const void *s, size_t n);
void  *memmove(void *d, const void *s, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
const void *memchr(const void *p, int c, size_t n);
const char *strerror(int errnum);

/* stdlib.h prototypes */
int       atoi(const char *s);
long      atol(const char *s);
long      strtol(const char *s, char **end, int base);
long long strtoll(const char *s, char **end, int base);
int       abs(int x);
void     *malloc(size_t n);
void      free(void *p);
void     *calloc(size_t nm, size_t sz);
void     *realloc(void *p, size_t n);
void      qsort(void *base, size_t nm, size_t sz,
                int (*cmp)(const void *, const void *));

/* ctype.h prototypes */
int    isalpha(int c);
int    isdigit(int c);
int    isxdigit(int c);
int    isspace(int c);
int    isalnum(int c);
int    isupper(int c);
int    islower(int c);
int    toupper(int c);
int    tolower(int c);

/* stdio.h prototypes */
void   putchar_(char c);                   /* underscore: name clash with shim */
void   puts_(const char *s);
int    vprintf_(const char *fmt, va_list ap);
int    vsprintf_(char *buf, const char *fmt, va_list ap);
int    vsnprintf_(char *buf, size_t n, const char *fmt, va_list ap);

/* Session 132 — FILE * surface for the tcc port.
 *
 * FILE is a small struct in libc.bin's per-process .data. fopen
 * allocates a slot in a 16-entry static table; fclose releases it.
 *
 * Read-mode files are loaded fully into a malloc'd buffer at fopen
 * time (size pulled from sys_fs_size). Write-mode files accumulate
 * into a growable malloc'd buffer; fclose flushes via sys_fs_write.
 * This sidesteps the lack of a kernel seek primitive — fseek/ftell
 * just move a position cursor inside the buffer.
 *
 * stdin / stdout / stderr are sentinel FILE * values 1, 2, 3 — not
 * real entries in the table. All fputX / fwrite / fprintf paths
 * check the low-valued pointer first and dispatch straight to
 * sys_write_fd with fd = (uintptr_t)stream - 1 (so stdin->fd0,
 * stdout->fd1, stderr->fd2). */
struct __libc_FILE {
    int   in_use;
    int   mode;        /* 0 = read, 1 = write */
    char *buf;
    size_t cap;
    size_t len;
    size_t pos;
    int   eof;
    int   err;
    char  path[128];   /* for write-mode flush via sys_fs_write */
};
typedef struct __libc_FILE FILE;

#define EOF (-1)
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2

/* Sentinel stream values — never dereferenced.  See stdio.c for the
 * recognition logic. */
#define stdin    ((FILE *)1)
#define stdout   ((FILE *)2)
#define stderr   ((FILE *)3)

FILE  *fopen_  (const char *path, const char *mode);
int    fclose_ (FILE *f);
size_t fread_  (void *ptr, size_t sz, size_t nm, FILE *f);
size_t fwrite_ (const void *ptr, size_t sz, size_t nm, FILE *f);
int    fseek_  (FILE *f, long offset, int whence);
long   ftell_  (FILE *f);
int    fputs_  (const char *s, FILE *f);
int    fputc_  (int c, FILE *f);
int    vfprintf_(FILE *f, const char *fmt, va_list ap);
int    ferror_ (FILE *f);
int    feof_   (FILE *f);
int    remove_ (const char *path);
int    fflush_ (FILE *f);
char  *fgets_  (char *s, int n, FILE *f);
int    fgetc_  (FILE *f);

/* malloc introspection */
uint32_t malloc_brk_(void);
uint32_t malloc_used_(void);
uint32_t malloc_free_bytes_(void);
uint32_t malloc_total_(void);

/* Self-info: fills out[0..2] = magic, version, export_count. */
void libc_info(uint32_t out[3]);

#endif
