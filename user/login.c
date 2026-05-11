/*
 * login — authenticate against /etc/passwd, drop privileges, exec
 * the user's shell.
 *
 * Flow:
 *   1. Print "login: " in canonical (line-edit, echo-on) mode and
 *      read a username.
 *   2. Print "password: " in canonical mode with echo OFF and read
 *      a password.
 *   3. Read /etc/passwd, look up the username's entry.
 *   4. Compute SHA-256(salt || password) using libcrypto and compare
 *      lowercase-hex to the entry's stored hash.
 *   5. On match: sys_setgid(gid) then sys_setuid(uid) — drops root
 *      if we were root (login.elf gets spawned by init as uid 0).
 *      Exec the user's shell (passwd field 7).
 *   6. On mismatch: print "Login incorrect" and re-prompt.
 *
 * /etc/passwd format (one user per line):
 *     name:salt$sha256hex:uid:gid:home:shell
 *
 *     - salt: 8-char ASCII (e.g. "ABCDef01")
 *     - sha256hex: 64 chars lowercase hex of sha256(salt || password)
 *     - uid, gid: decimal ints
 *     - home: ignored at this layer (no chdir yet)
 *     - shell: ELF name to exec on success
 *
 * Demo creds (set by mkfs.py at build time):
 *     root  / root      (uid=0,    gid=0)
 *     guest / guest     (uid=1000, gid=1000)
 */
#include "libuser.h"
#include "../libcrypto/crypto.h"

#define PASSWD_PATH   "/etc/passwd"
#define LINE_MAX      256
#define USERS_MAX     16

struct user_entry {
    char name[32];
    char salt[16];
    char hash[68];        /* 64 hex + NUL */
    int  uid;
    int  gid;
    char home[32];
    char shell[32];
};

static struct user_entry g_users[USERS_MAX];
static int               g_n_users;

/* ---- Helpers --------------------------------------------------- */

static int my_strncmp(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        if (a[i] == 0) return 0;
    }
    return 0;
}

static int my_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static int parse_int(const char *s, int n) {
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') break;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

static void hex_lower(const unsigned char *in, int n, char *out) {
    static const char *d = "0123456789abcdef";
    for (int i = 0; i < n; i++) {
        out[2*i  ] = d[in[i] >> 4];
        out[2*i+1] = d[in[i] & 0xF];
    }
    out[2*n] = 0;
}

/* ---- /etc/passwd parser --------------------------------------- */

/* Parse one line "name:salt$hash:uid:gid:home:shell" into g_users[*]. */
static int parse_passwd_line(const char *line, int len) {
    if (g_n_users >= USERS_MAX) return -1;
    struct user_entry *u = &g_users[g_n_users];

    /* Field bounds in `line`. */
    int starts[6] = {0}, ends[6] = {0};
    int field = 0;
    int s = 0;
    for (int i = 0; i <= len && field < 6; i++) {
        if (i == len || line[i] == ':') {
            starts[field] = s;
            ends[field]   = i;
            field++;
            s = i + 1;
        }
    }
    if (field < 6) return -1;

    /* Field 0: name. */
    int nl = ends[0] - starts[0];
    if (nl <= 0 || nl >= (int)sizeof(u->name)) return -1;
    for (int i = 0; i < nl; i++) u->name[i] = line[starts[0] + i];
    u->name[nl] = 0;

    /* Field 1: salt$hash. Split on '$'. */
    int dollar = -1;
    for (int i = starts[1]; i < ends[1]; i++) {
        if (line[i] == '$') { dollar = i; break; }
    }
    if (dollar < 0) return -1;
    int sl = dollar - starts[1];
    int hl = ends[1] - dollar - 1;
    if (sl <= 0 || sl >= (int)sizeof(u->salt) || hl != 64) return -1;
    for (int i = 0; i < sl; i++) u->salt[i] = line[starts[1] + i];
    u->salt[sl] = 0;
    for (int i = 0; i < hl; i++) u->hash[i] = line[dollar + 1 + i];
    u->hash[hl] = 0;

    /* Field 2: uid. Field 3: gid. */
    u->uid = parse_int(line + starts[2], ends[2] - starts[2]);
    u->gid = parse_int(line + starts[3], ends[3] - starts[3]);

    /* Field 4: home. Field 5: shell. */
    int hml = ends[4] - starts[4];
    if (hml >= (int)sizeof(u->home)) hml = (int)sizeof(u->home) - 1;
    for (int i = 0; i < hml; i++) u->home[i] = line[starts[4] + i];
    u->home[hml] = 0;

    int shl = ends[5] - starts[5];
    if (shl >= (int)sizeof(u->shell)) shl = (int)sizeof(u->shell) - 1;
    for (int i = 0; i < shl; i++) u->shell[i] = line[starts[5] + i];
    u->shell[shl] = 0;

    g_n_users++;
    return 0;
}

static int load_passwd(void) {
    int fd = sys_open(PASSWD_PATH);
    if (fd < 0) return -1;

    static char buf[2048];
    int n = sys_read(fd, buf, (int)sizeof(buf));
    sys_close(fd);
    if (n <= 0) return -1;

    int line_start = 0;
    for (int i = 0; i < n; i++) {
        if (buf[i] == '\n' || i == n - 1) {
            int line_end = (buf[i] == '\n') ? i : i + 1;
            if (line_end > line_start) {
                parse_passwd_line(buf + line_start, line_end - line_start);
            }
            line_start = i + 1;
        }
    }
    return 0;
}

static struct user_entry *find_user(const char *name) {
    for (int i = 0; i < g_n_users; i++) {
        if (my_strcmp(g_users[i].name, name) == 0) return &g_users[i];
    }
    return 0;
}

/* ---- Password prompt + verify --------------------------------- */

/* Read a line of input from stdin, terminated by '\n'. Returns
 * length (excluding the '\n'); the line is NUL-terminated.
 * `echo`: 0 to suppress echo (for password entry), 1 for normal. */
static int read_line(char *out, int cap, int echo) {
    uint32_t prev = tty_get_mode();
    /* Canonical for line editing; echo conditional. */
    tty_set_mode(echo ? (TTY_ICANON | TTY_ECHO) : TTY_ICANON);
    int n = sys_read_line(out, cap);
    if (n < 0) n = 0;
    if (n > 0 && out[n - 1] == '\n') n--;
    out[n] = 0;
    tty_set_mode(prev);
    if (!echo) putchar('\n');
    return n;
}

static int verify_password(struct user_entry *u, const char *password) {
    /* digest = SHA-256(salt || password) — same construction mkfs.py
     * used to fill the passwd file. */
    struct sha256 s;
    sha256_init(&s);
    sha256_update(&s, u->salt, (int)my_strncmp(u->salt, u->salt, 16)
                             ? 0 : 0);   /* unused — pacify warning */
    /* Lengths via tiny inline strlen — libuser exposes strlen but
     * libcrypto's static link doesn't pull it in cleanly. */
    int sl = 0; while (u->salt[sl])     sl++;
    int pl = 0; while (password[pl])    pl++;
    sha256_init(&s);
    sha256_update(&s, u->salt, sl);
    sha256_update(&s, password, pl);
    uint8_t digest[32];
    sha256_final(&s, digest);

    char got[68];
    hex_lower(digest, 32, got);
    return my_strcmp(got, u->hash) == 0;
}

/* ---- Main login loop ------------------------------------------ */

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    if (load_passwd() < 0) {
        puts("login: cannot read /etc/passwd\n");
        return 1;
    }

    char username[64];
    char password[64];

    for (;;) {
        puts("\nAdventOS login: ");
        if (read_line(username, sizeof(username), 1) <= 0) continue;

        puts("password: ");
        read_line(password, sizeof(password), 0);

        struct user_entry *u = find_user(username);
        if (!u || !verify_password(u, password)) {
            puts("Login incorrect\n");
            sys_sleep_ms(500);
            continue;
        }

        /* Authenticated. Drop privileges + exec the user's shell. */
        printf("Welcome, %s (uid=%d gid=%d)\n", u->name, u->uid, u->gid);
        sys_setgid(u->gid);
        if (sys_setuid(u->uid) < 0) {
            puts("login: sys_setuid failed\n");
            continue;
        }

        const char *shell = u->shell[0] ? u->shell : "sh.elf";
        const char *sh_argv[] = { shell, 0 };
        sys_exec(shell, sh_argv);
        puts("login: exec failed\n");
        return 1;
    }
}
