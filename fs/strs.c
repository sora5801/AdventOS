/*
 * strs.c — session 91 smoke test for cc string literals.
 *
 * Demonstrates:
 *   - "..." string literals (with \n \t \\ \" escapes)
 *   - puts(s)        write s + '\n' to stdout
 *   - print_str(s)   write s with no newline
 *   - string dedup   the two "hi" literals share one pool entry
 */

int greet() {
    puts("hello, world");
    return 0;
}

int main() {
    puts("hello, world");
    print_str("two ");
    print_str("words, ");
    puts("one line");

    puts("tab\there\tand\there");
    puts("quote: \"yes\" backslash: \\");

    puts("hi");
    puts("hi");

    greet();
    return 7;
}
