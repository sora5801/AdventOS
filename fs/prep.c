/*
 * prep.c — session 95 smoke test for the cc preprocessor.
 *
 * Exercises:
 *   #include   header-guarded /colors.h pulls in 5 macros
 *   #define    object-like macros (constants and a token)
 *   #undef     remove a macro
 *   #ifdef     conditional compile (positive)
 *   #ifndef    conditional compile (negative)
 *   #else      flip-branch
 *   #endif     close
 *
 * Including colors.h TWICE should be safe because of the header
 * guard.
 */

#include "colors.h"
#include "colors.h"   /* re-include, must be a no-op */

#define MSG "preprocessor lives"
#define SHOW_HEX

int main() {
    printf("%s\n", MSG);
    printf("RED=%d  GREEN=%d  BLUE=%d  WHITE=%d\n",
           RED, GREEN, BLUE, WHITE);
    printf("MAX_NAME=%d\n", MAX_NAME);

#ifdef SHOW_HEX
    printf("hex(255) = 0x%x\n", 255);
#endif

#ifndef HIDE_FOOTER
    printf("(no HIDE_FOOTER macro)\n");
#else
    printf("HIDE_FOOTER is on\n");
#endif

#define DBG
#ifdef DBG
    printf("DBG was just defined\n");
#endif

#undef DBG
#ifdef DBG
    printf("you should never see this\n");
#else
    printf("DBG is gone after #undef\n");
#endif

    return WHITE;          /* exit 7 */
}
