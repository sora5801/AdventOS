/*
 * echo — print all argv entries separated by spaces, terminated by \n.
 * Demonstrates real argv passing from the kernel into ring 3.
 */

#include "libuser.h"

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) sys_write(1, " ", 1);
        sys_write(1, argv[i], (int)strlen(argv[i]));
    }
    sys_write(1, "\n", 1);
    return 0;
}
