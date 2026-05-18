/*
 * Session 135 — minimal smoke for the FILE * write path.
 *
 * Replicates the exact sequence tcc uses inside tcc_write_elf_file:
 *   open(path, O_WRONLY|O_CREAT|O_TRUNC, 0666)
 *   fdopen(fd, "wb")
 *   fwrite()s + fputc()s
 *   fclose()
 *
 * Prints a marker before and after each step so the hang is easy to
 * locate inside QEMU.
 */
#include "libuser.h"

int main(void) {
    puts("[fwtest] open()...");
    int fd = open("/fwtest.out", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) { puts("[fwtest] open failed"); return 1; }
    printf("[fwtest] fd = %d\n", fd);

    puts("[fwtest] fdopen()...");
    FILE *f = fdopen(fd, "wb");
    if (!f) { puts("[fwtest] fdopen failed"); return 2; }
    printf("[fwtest] f = %x\n", (unsigned)f);

    puts("[fwtest] fwrite 4 bytes...");
    const char *hdr = "ELF1";
    int n = (int)fwrite(hdr, 1, 4, f);
    printf("[fwtest] fwrite returned %d\n", n);

    puts("[fwtest] 50 fputc(0)s...");
    for (int i = 0; i < 50; i++) fputc(0, f);
    puts("[fwtest] 50 fputc done");

    puts("[fwtest] fwrite 1000 bytes...");
    char buf[1000];
    for (int i = 0; i < 1000; i++) buf[i] = (char)i;
    n = (int)fwrite(buf, 1, 1000, f);
    printf("[fwtest] fwrite 1000 returned %d\n", n);

    puts("[fwtest] fclose()...");
    int rc = fclose(f);
    printf("[fwtest] fclose returned %d\n", rc);

    puts("[fwtest] close(fd)...");
    rc = close(fd);
    printf("[fwtest] close returned %d\n", rc);

    puts("[fwtest] DONE");
    return 0;
}
