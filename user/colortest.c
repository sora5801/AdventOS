/*
 * colortest — emit a short ANSI-colored string for the session 166
 * smoke + casual visual inspection.  Each colour name is printed in
 * the colour it names, followed by a reset, so the eye can tell at
 * a glance whether wmterm's SGR parser maps the codes right.
 */

#include "libuser.h"

static void put(const char *s) {
    int n = 0; while (s[n]) n++;
    sys_write(1, s, n);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    put("\033[31mRED\033[0m "
        "\033[32mGREEN\033[0m "
        "\033[33mYELLOW\033[0m "
        "\033[34mBLUE\033[0m "
        "\033[35mMAGENTA\033[0m "
        "\033[36mCYAN\033[0m\n");
    put("\033[1;31mBRED\033[0m "
        "\033[1;32mBGREEN\033[0m "
        "\033[44;37mWBLUE\033[0m\n");
    return 0;
}
