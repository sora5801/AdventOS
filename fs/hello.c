/*
 * hello.c — smoke test for the AdventOS cc compiler (session 90).
 *
 * Demonstrates the subset the compiler accepts:
 *   - int locals, function definitions with int params/return
 *   - if / while / return
 *   - +  -  *  /  %  comparison ops
 *   - function calls (user-defined and built-in intrinsics)
 *
 * Built-in intrinsics the compiler recognizes by name:
 *   print_int(n)       — write decimal n + newline to stdout
 *   sys_exit(code)     — terminate with status `code`
 *   sys_write, sys_getpid — raw syscalls
 *
 * Compile and run:
 *     cc hello.c              -> writes /hello.elf
 *     /hello.elf              -> runs it
 *     echo $?                 -> 42
 */

int fact(int n) {
    if (n <= 1) return 1;
    return n * fact(n - 1);
}

int sum_to(int n) {
    int s;
    int i;
    s = 0;
    i = 1;
    while (i <= n) {
        s = s + i;
        i = i + 1;
    }
    return s;
}

int gcd(int a, int b) {
    while (b != 0) {
        int t;
        t = a % b;
        a = b;
        b = t;
    }
    return a;
}

int main() {
    print_int(fact(10));         /* 3628800 */
    print_int(sum_to(100));      /* 5050    */
    print_int(gcd(462, 1071));   /* 21      */
    return 42;
}
