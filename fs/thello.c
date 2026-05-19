/* Session 134 smoke test target for the in-AdventOS tcc.elf.
 * No #include, no libc — just enough to test tcc's parser + codegen
 * + ELF-output path. The resulting ELF won't be loadable by the
 * AdventOS kernel loader yet (tcc emits Linux multi-PT_LOAD ELFs;
 * AdventOS wants single-PT_LOAD); the goal here is to confirm tcc
 * runs the full compile + link pipeline without crashing inside the
 * OS. Loader-format adaptation is the next session's work. */
int factorial(int n) {
    if (n <= 1) return 1;
    return n * factorial(n - 1);
}

int main(void) {
    int x = factorial(5);   /* 120 */
    return x;
}
