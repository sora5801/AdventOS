/*
 * Session 135 — tcc full-link smoke target.
 *
 * Provides _start so tcc can produce a fully-linked executable
 * without needing an external crt0/start.o. Writes "tcchi\n" to
 * stdout (fd 1) via int $0x80 and exits cleanly.
 *
 * Whether the resulting ELF runs under AdventOS's kernel loader is
 * the question this exists to answer.
 */
static void sys_write_fd(int fd, const char *buf, int n) {
    __asm__ volatile ("int $0x80"
                      :
                      : "a"(12), "b"(fd), "c"(buf), "d"(n)
                      : "memory");
}

static void sys_exit(int code) {
    __asm__ volatile ("int $0x80"
                      :
                      : "a"(3), "b"(code)
                      : "memory");
    for (;;) __asm__ volatile ("hlt");
}

void _start(void) {
    const char *msg = "tcchi\n";
    sys_write_fd(1, msg, 6);
    sys_exit(0);
}
