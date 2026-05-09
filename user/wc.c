/*
 * wc — count lines, words, and bytes.
 *
 *   echo hello world | wc      ->  1 2 12  (the 12th byte is the \n)
 *   wc /etc/inittab            ->  10 28 301 /etc/inittab
 *
 * Flags: -l (lines), -w (words), -c (bytes). Default (no flag) prints
 * all three. Multiple flags may combine (`-lw` shows lines+words).
 *
 * Word counting follows POSIX: a "word" is a maximal run of
 * non-whitespace characters. Whitespace is space / tab / newline.
 */

#include "libuser.h"

static void count_fd(int fd, uint32_t *l, uint32_t *w, uint32_t *b) {
    char     buf[256];
    int      in_word = 0;
    int      n;
    while ((n = sys_read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            (*b)++;
            if (c == '\n')                                    (*l)++;
            if (c == ' ' || c == '\t' || c == '\n')           in_word = 0;
            else if (!in_word)                                { in_word = 1; (*w)++; }
        }
    }
}

static void emit(uint32_t l, uint32_t w, uint32_t b,
                 int show_l, int show_w, int show_c, const char *name) {
    int first = 1;
    if (show_l) { if (!first) putchar(' '); printf("%u", l); first = 0; }
    if (show_w) { if (!first) putchar(' '); printf("%u", w); first = 0; }
    if (show_c) { if (!first) putchar(' '); printf("%u", b); first = 0; }
    if (name && *name) { putchar(' '); puts(name); }
    else putchar('\n');
}

int main(int argc, char **argv) {
    int show_l = 0, show_w = 0, show_c = 0;
    int argi   = 1;

    /* Parse flags: -l / -w / -c, possibly grouped (`-lw` etc.). */
    while (argi < argc && argv[argi][0] == '-' && argv[argi][1]) {
        for (int k = 1; argv[argi][k]; k++) {
            switch (argv[argi][k]) {
                case 'l': show_l = 1; break;
                case 'w': show_w = 1; break;
                case 'c': case 'm': show_c = 1; break;
                default:
                    sys_write(2, "wc: bad flag\n", 13);
                    return 2;
            }
        }
        argi++;
    }
    if (!show_l && !show_w && !show_c) {
        show_l = show_w = show_c = 1;
    }

    if (argi >= argc) {
        uint32_t l = 0, w = 0, b = 0;
        count_fd(0, &l, &w, &b);
        emit(l, w, b, show_l, show_w, show_c, "");
        return 0;
    }

    uint32_t tl = 0, tw = 0, tb = 0;
    int      nfiles = 0;
    for (int i = argi; i < argc; i++) {
        int fd = sys_open(argv[i]);
        if (fd < 0) {
            sys_write(2, "wc: ", 4);
            sys_write(2, argv[i], (int)strlen(argv[i]));
            sys_write(2, ": cannot open\n", 14);
            continue;
        }
        uint32_t l = 0, w = 0, b = 0;
        count_fd(fd, &l, &w, &b);
        sys_close(fd);
        emit(l, w, b, show_l, show_w, show_c, argv[i]);
        tl += l; tw += w; tb += b;
        nfiles++;
    }
    if (nfiles > 1) emit(tl, tw, tb, show_l, show_w, show_c, "total");
    return 0;
}
