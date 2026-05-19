/*
 * AdventOS libc — export table.
 *
 * The first 0x400 bytes of libc.bin are reserved for:
 *   +0x000  struct libc_header (16 bytes)
 *   +0x010  void *libc_exports[LIBC_EXPORT_COUNT]   (0x100 bytes for 64 ptrs)
 *
 * Both go into section ".exports", which the linker script places at
 * the very start of the output binary. The header uses the LIBC_MAGIC
 * value as a sanity check user programs (and the kernel's dyld layer)
 * verify on load.
 *
 * To add a new exported function:
 *   1. Add a LIBC_FN_FOO enum value in libc.h
 *   2. Add &foo to the libc_exports table below (use designated init)
 *   3. Add a trampoline in user/libuser.h
 *   4. Bump LIBC_VERSION
 */
#include "libc.h"

/* libc_info(out[3]) -> magic, version, export_count. Useful as a
 * boot-time sanity check for user code. */
void libc_info(uint32_t out[3]) {
    out[0] = LIBC_MAGIC;
    out[1] = LIBC_VERSION;
    out[2] = LIBC_EXPORT_COUNT;
}

/* Header — placed first in the output binary by the linker script. */
__attribute__((section(".exports.header"), used))
const struct libc_header libc_hdr = {
    .magic        = LIBC_MAGIC,
    .version      = LIBC_VERSION,
    .export_count = LIBC_EXPORT_COUNT,
    .reserved     = 0,
};

/* Function pointer table — placed right after the header. Designated
 * initializers leave unused slots NULL; the table is fixed at 64
 * entries so user-program hard-coded offsets are stable across
 * libc rebuilds (subject to the version check). */
__attribute__((section(".exports.table"), used))
void *const libc_exports[LIBC_EXPORT_COUNT] = {
    [LIBC_FN_LIBC_INFO]    = libc_info,

    [LIBC_FN_STRLEN]       = strlen,
    [LIBC_FN_STRCMP]       = strcmp,
    [LIBC_FN_STRNCMP]      = strncmp,
    [LIBC_FN_STRCPY]       = strcpy,
    [LIBC_FN_STRNCPY]      = strncpy,
    [LIBC_FN_STRCAT]       = strcat,
    [LIBC_FN_STRCHR]       = strchr,
    [LIBC_FN_STRRCHR]      = strrchr,
    [LIBC_FN_STRSTR]       = strstr,
    [LIBC_FN_MEMSET]       = memset,
    [LIBC_FN_MEMCPY]       = memcpy,
    [LIBC_FN_MEMMOVE]      = memmove,
    [LIBC_FN_MEMCMP]       = memcmp,
    [LIBC_FN_MEMCHR]       = memchr,
    [LIBC_FN_STRERROR]     = strerror,

    [LIBC_FN_ATOI]         = atoi,
    [LIBC_FN_ATOL]         = atol,
    [LIBC_FN_STRTOL]       = strtol,
    [LIBC_FN_ABS]          = abs,
    [LIBC_FN_MALLOC]       = malloc,
    [LIBC_FN_FREE]         = free,
    [LIBC_FN_CALLOC]       = calloc,
    [LIBC_FN_REALLOC]      = realloc,
    [LIBC_FN_QSORT]        = qsort,
    [LIBC_FN_STRTOLL]      = strtoll,

    [LIBC_FN_ISALPHA]      = isalpha,
    [LIBC_FN_ISDIGIT]      = isdigit,
    [LIBC_FN_ISSPACE]      = isspace,
    [LIBC_FN_ISALNUM]      = isalnum,
    [LIBC_FN_ISUPPER]      = isupper,
    [LIBC_FN_ISLOWER]      = islower,
    [LIBC_FN_TOUPPER]      = toupper,
    [LIBC_FN_TOLOWER]      = tolower,
    [LIBC_FN_ISXDIGIT]     = isxdigit,

    [LIBC_FN_PUTCHAR]      = putchar_,
    [LIBC_FN_PUTS]         = puts_,
    [LIBC_FN_VPRINTF]      = vprintf_,
    [LIBC_FN_VSPRINTF]     = vsprintf_,
    [LIBC_FN_VSNPRINTF]    = vsnprintf_,

    /* Session 134 — FILE * + stdio family for the tcc port. */
    [LIBC_FN_VFPRINTF]     = vfprintf_,
    [LIBC_FN_FOPEN]        = fopen_,
    [LIBC_FN_FCLOSE]       = fclose_,
    [LIBC_FN_FREAD]        = fread_,
    [LIBC_FN_FWRITE]       = fwrite_,
    [LIBC_FN_FSEEK]        = fseek_,
    [LIBC_FN_FTELL]        = ftell_,
    [LIBC_FN_FPUTS]        = fputs_,
    [LIBC_FN_FPUTC]        = fputc_,
    [LIBC_FN_FERROR]       = ferror_,
    [LIBC_FN_FEOF]         = feof_,
    [LIBC_FN_REMOVE]       = remove_,
    [LIBC_FN_FFLUSH]       = fflush_,
    [LIBC_FN_FGETS]        = fgets_,
    [LIBC_FN_FGETC]        = fgetc_,

    [LIBC_FN_MALLOC_BRK]   = malloc_brk_,
    [LIBC_FN_MALLOC_USED]  = malloc_used_,
    [LIBC_FN_MALLOC_FREE_] = malloc_free_bytes_,
    [LIBC_FN_MALLOC_TOTAL] = malloc_total_,
};
