/*
 * AdventOS libc — string.h implementation.
 *
 * Plain freestanding C, no syscalls. Lives in libc.bin's .text;
 * accessible to user programs via the export table at LIBC_BASE.
 */
#include "libc.h"

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (!n) return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strcpy(char *d, const char *s) {
    char *r = d;
    while ((*d++ = *s++)) ;
    return r;
}

char *strncpy(char *d, const char *s, size_t n) {
    char *r = d;
    while (n && *s) { *d++ = *s++; n--; }
    while (n--) *d++ = 0;
    return r;
}

char *strcat(char *d, const char *s) {
    char *r = d;
    while (*d) d++;
    while ((*d++ = *s++)) ;
    return r;
}

const char *strchr(const char *s, int c) {
    char ch = (char)c;
    while (*s) {
        if (*s == ch) return s;
        s++;
    }
    /* C says strchr matches the trailing NUL when c == '\0'. */
    return (ch == 0) ? s : NULL;
}

const char *strrchr(const char *s, int c) {
    char ch = (char)c;
    const char *last = NULL;
    for (;;) {
        if (*s == ch) last = s;
        if (!*s) break;
        s++;
    }
    return last;
}

const char *strstr(const char *h, const char *n) {
    if (!*n) return h;
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return h;
    }
    return NULL;
}

void *memset(void *p, int c, size_t n) {
    unsigned char *d = p;
    while (n--) *d++ = (unsigned char)c;
    return p;
}

void *memcpy(void *d, const void *s, size_t n) {
    unsigned char       *dd = d;
    const unsigned char *ss = s;
    while (n--) *dd++ = *ss++;
    return d;
}

void *memmove(void *d, const void *s, size_t n) {
    unsigned char       *dd = d;
    const unsigned char *ss = s;
    if (dd < ss) {
        while (n--) *dd++ = *ss++;
    } else if (dd > ss) {
        dd += n; ss += n;
        while (n--) *--dd = *--ss;
    }
    return d;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *aa = a;
    const unsigned char *bb = b;
    while (n--) {
        if (*aa != *bb) return (int)*aa - (int)*bb;
        aa++; bb++;
    }
    return 0;
}

const void *memchr(const void *p, int c, size_t n) {
    const unsigned char *pp = p;
    unsigned char ch = (unsigned char)c;
    while (n--) {
        if (*pp == ch) return pp;
        pp++;
    }
    return NULL;
}

/* Session 134 — strerror.  AdventOS has no real errno values yet;
 * tcc only calls strerror to format messages it then prints, so a
 * one-size-fits-all string is enough. (Could grow later if more
 * specific messages are useful — kernel doesn't currently set a
 * per-syscall errno.) */
const char *strerror(int errnum) {
    (void)errnum;
    return "I/O error";
}
