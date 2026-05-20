/*
 * AdventOS libc — stdio FILE * implementation.
 *
 * Session 134 (Path B tcc port phase 2). tcc relies heavily on
 * fopen/fread/fwrite/fprintf/etc. AdventOS doesn't have a kernel-
 * level seek primitive, so we implement seekable FILEs by buffering
 * the entire file in memory.
 *
 * Lifecycle:
 *   - fopen("path", "r"):   sys_fs_size to learn the byte count,
 *                           malloc the buffer, sys_open + sys_read
 *                           the whole file into it, sys_close. All
 *                           subsequent fread/fseek/ftell operate on
 *                           the buffer only.
 *   - fopen("path", "w"):   allocate an initial 4KiB growable buffer.
 *                           fwrite/fputc/fputs/fprintf append; the
 *                           buffer doubles when full.
 *   - fclose:               write-mode flushes via sys_fs_write
 *                           (replaces the file's contents in one
 *                           shot). Then frees the buffer.
 *
 * stdin/stdout/stderr are not real FILE * — they're the sentinel
 * pointer values 1, 2, 3. fputX / fwrite / fprintf detect them up
 * front and dispatch straight to sys_write_fd with the corresponding
 * fd (stdin -> fd 0, stdout -> fd 1, stderr -> fd 2). fread/fseek
 * on stdin currently fail; tcc does not read stdin so that is fine.
 *
 * Limits:
 *   FOPEN_MAX = 16     (per-process open FILE * count)
 *   max read-file size = 4 MiB (cap to keep the heap sane)
 *   max write growth   = unbounded subject to malloc()
 */
#include "libc.h"

#define FOPEN_MAX           16
#define READ_FILE_CAP       (4 * 1024 * 1024)
#define WRITE_INIT_CAP      4096

static FILE g_files[FOPEN_MAX];

/* ---- raw syscall wrappers (internal — same shape as user/libuser.c
 *      ones, inlined here so libc stays self-contained). ---- */

static int sys_open_r(const char *path) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(LIBC_SYS_OPEN), "b"(path)
                      : "memory");
    return ret;
}
static int sys_read_(int fd, void *buf, int n) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(LIBC_SYS_READ), "b"(fd), "c"(buf), "d"(n)
                      : "memory");
    return ret;
}
static int sys_write_fd_(int fd, const void *buf, int n) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(LIBC_SYS_WRITE_FD), "b"(fd), "c"(buf), "d"(n)
                      : "memory");
    return ret;
}
static int sys_close_(int fd) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(LIBC_SYS_CLOSE), "b"(fd)
                      : "memory");
    return ret;
}
static int sys_fs_size_(const char *path) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(LIBC_SYS_FS_SIZE), "b"(path)
                      : "memory");
    return ret;
}
static int sys_fs_write_(const char *path, const void *data, uint32_t n) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(LIBC_SYS_FS_WRITE), "b"(path), "c"(data), "d"(n)
                      : "memory");
    return ret;
}
static int sys_unlink_(const char *path) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(LIBC_SYS_UNLINK), "b"(path)
                      : "memory");
    return ret;
}

/* ---- helpers ---- */

static int is_std_stream(FILE *f) {
    return (uintptr_t)f >= 1 && (uintptr_t)f <= 3;
}
static int fd_for_std_stream(FILE *f) {
    /* stdin sentinel = 1 -> fd 0, stdout = 2 -> fd 1, stderr = 3 -> fd 2. */
    return (int)(uintptr_t)f - 1;
}

static FILE *alloc_slot(void) {
    for (int i = 0; i < FOPEN_MAX; i++) {
        if (!g_files[i].in_use) {
            FILE *f = &g_files[i];
            f->in_use = 1;
            f->mode = 0;
            f->buf = NULL;
            f->cap = 0;
            f->len = 0;
            f->pos = 0;
            f->eof = 0;
            f->err = 0;
            f->path[0] = 0;
            return f;
        }
    }
    return NULL;
}

static void copy_path(char *dst, const char *src) {
    int i = 0;
    while (i < 127 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* ---- fopen / fclose ---- */

FILE *fopen_(const char *path, const char *mode) {
    if (!path || !mode) return NULL;
    int want_write = (mode[0] == 'w') || (mode[0] == 'a');
    FILE *f = alloc_slot();
    if (!f) return NULL;

    if (want_write) {
        f->mode = 1;
        f->buf = (char *)malloc(WRITE_INIT_CAP);
        if (!f->buf) { f->in_use = 0; return NULL; }
        f->cap = WRITE_INIT_CAP;
        f->len = 0;
        f->pos = 0;
        copy_path(f->path, path);
        return f;
    }

    /* read mode */
    f->mode = 0;
    int sz = sys_fs_size_(path);
    if (sz < 0) { f->in_use = 0; return NULL; }
    if (sz > READ_FILE_CAP) { f->in_use = 0; return NULL; }
    if (sz == 0) {
        f->buf = NULL;
        f->cap = f->len = f->pos = 0;
        return f;
    }
    int fd = sys_open_r(path);
    if (fd < 0) { f->in_use = 0; return NULL; }
    f->buf = (char *)malloc((size_t)sz);
    if (!f->buf) { sys_close_(fd); f->in_use = 0; return NULL; }
    /* Read in chunks — sys_read may return short. */
    size_t got = 0;
    while ((int)got < sz) {
        int n = sys_read_(fd, f->buf + got, sz - (int)got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    sys_close_(fd);
    f->len = got;
    f->cap = (size_t)sz;
    f->pos = 0;
    return f;
}

int fclose_(FILE *f) {
    if (!f || is_std_stream(f) || !f->in_use) return EOF;
    int rc = 0;
    if (f->mode == 1 && f->path[0]) {
        int wr = sys_fs_write_(f->path, f->buf, (uint32_t)f->len);
        if (wr < 0) rc = EOF;
    }
    if (f->buf) free(f->buf);
    f->buf = NULL;
    f->cap = f->len = f->pos = 0;
    f->in_use = 0;
    return rc;
}

/* ---- fread / fwrite ---- */

size_t fread_(void *ptr, size_t sz, size_t nm, FILE *f) {
    if (is_std_stream(f) || !f || !f->in_use || f->mode != 0) return 0;
    size_t want = sz * nm;
    size_t avail = (f->pos < f->len) ? (f->len - f->pos) : 0;
    size_t take = want < avail ? want : avail;
    if (take == 0) {
        if (f->pos >= f->len) f->eof = 1;
        return 0;
    }
    memcpy(ptr, f->buf + f->pos, take);
    f->pos += take;
    if (f->pos >= f->len) f->eof = 1;
    return (sz > 0) ? (take / sz) : 0;
}

/* Grow buf so it has room for `extra` more bytes past current len. */
static int ensure_write_cap(FILE *f, size_t extra) {
    if (f->len + extra <= f->cap) return 0;
    size_t newcap = f->cap ? f->cap : WRITE_INIT_CAP;
    while (newcap < f->len + extra) {
        size_t doubled = newcap * 2;
        if (doubled < newcap) return -1;     /* overflow */
        newcap = doubled;
    }
    char *nb = (char *)realloc(f->buf, newcap);
    if (!nb) return -1;
    f->buf = nb;
    f->cap = newcap;
    return 0;
}

size_t fwrite_(const void *ptr, size_t sz, size_t nm, FILE *f) {
    if (!f) return 0;
    size_t want = sz * nm;
    if (is_std_stream(f)) {
        int n = sys_write_fd_(fd_for_std_stream(f), ptr, (int)want);
        return (n > 0) ? ((sz > 0) ? ((size_t)n / sz) : 0) : 0;
    }
    if (!f->in_use || f->mode != 1) return 0;
    if (ensure_write_cap(f, want) < 0) { f->err = 1; return 0; }
    memcpy(f->buf + f->len, ptr, want);
    f->len += want;
    f->pos = f->len;
    return nm;
}

/* ---- fputs / fputc ---- */

int fputs_(const char *s, FILE *f) {
    if (!s || !f) return EOF;
    size_t n = strlen(s);
    size_t wrote = fwrite_(s, 1, n, f);
    return wrote == n ? (int)n : EOF;
}

int fputc_(int c, FILE *f) {
    unsigned char ch = (unsigned char)c;
    size_t wrote = fwrite_(&ch, 1, 1, f);
    return wrote == 1 ? (int)ch : EOF;
}

/* ---- fseek / ftell ---- */

int fseek_(FILE *f, long offset, int whence) {
    if (!f || is_std_stream(f) || !f->in_use) return -1;
    long base = 0;
    if      (whence == SEEK_SET) base = 0;
    else if (whence == SEEK_CUR) base = (long)f->pos;
    else if (whence == SEEK_END) base = (long)f->len;
    else return -1;
    long np = base + offset;
    if (np < 0) return -1;
    /* In write mode allow seeking past current len up to cap (tcc
     * sometimes patches header offsets back into the buffer). Grow
     * if needed and zero-fill the gap. */
    if (f->mode == 1 && (size_t)np > f->len) {
        if (ensure_write_cap(f, (size_t)np - f->len) < 0) return -1;
        memset(f->buf + f->len, 0, (size_t)np - f->len);
        f->len = (size_t)np;
    }
    f->pos = (size_t)np;
    f->eof = 0;
    return 0;
}

long ftell_(FILE *f) {
    if (!f || is_std_stream(f) || !f->in_use) return -1;
    return (long)f->pos;
}

/* ---- ferror / feof ---- */

int ferror_(FILE *f) {
    if (!f || is_std_stream(f) || !f->in_use) return 0;
    return f->err;
}
int feof_(FILE *f) {
    if (!f || is_std_stream(f) || !f->in_use) return 0;
    return f->eof;
}

/* ---- remove / fflush ---- */

int remove_(const char *path) {
    return sys_unlink_(path);
}

int fflush_(FILE *f) {
    /* For std streams there's nothing to flush (sys_write_fd is
     * synchronous). For write-mode FILEs we don't flush mid-write —
     * the buffer is persisted on fclose. So fflush is a no-op success
     * for all valid streams. */
    if (!f) return 0;
    return 0;
}

/* ---- fgets / fgetc ---- */

char *fgets_(char *s, int n, FILE *f) {
    if (!s || n <= 0 || !f || !f->in_use || f->mode != 0) return NULL;
    if (f->pos >= f->len) { f->eof = 1; return NULL; }
    int i = 0;
    while (i < n - 1 && f->pos < f->len) {
        char c = f->buf[f->pos++];
        s[i++] = c;
        if (c == '\n') break;
    }
    s[i] = 0;
    if (f->pos >= f->len && i == 0) { f->eof = 1; return NULL; }
    return s;
}

int fgetc_(FILE *f) {
    if (!f || !f->in_use || f->mode != 0) return EOF;
    if (f->pos >= f->len) { f->eof = 1; return EOF; }
    return (unsigned char)f->buf[f->pos++];
}

/* ---- vfprintf — feeds the existing printf formatter via a sink ---- */

/* We don't have direct access to do_format from here without
 * exposing it. Simplest: format into a stack buffer via vsnprintf_,
 * then fwrite. Limit per call to 1KiB which is sufficient for tcc's
 * error messages and asm comment lines. For larger writes the caller
 * should chunk. */
int vfprintf_(FILE *f, const char *fmt, va_list ap) {
    char tmp[1024];
    int n = vsnprintf_(tmp, sizeof(tmp), fmt, ap);
    if (n <= 0) return n;
    int capn = (n < (int)sizeof(tmp)) ? n : (int)sizeof(tmp) - 1;
    fwrite_(tmp, 1, (size_t)capn, f);
    return n;
}
