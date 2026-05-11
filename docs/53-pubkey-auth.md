# Session 53 — Public-key authentication (RFC 4252 §7)

**Goal:** add `publickey` userauth to AdventOS sshd. The shape that matters for SSH-2 in practice: a client presents an ed25519 public key, the server checks it's in the user's authorized list, the client proves possession by signing a canonical auth-blob, the server verifies the signature with `ed25519_verify`. Same primitives we already have from session 39's TLS Ed25519 work.

End state — **loopback selftest `[t35]` passes** all three assertions:

```
[t35] sshd: pubkey auth (ed25519 probe + signed auth-blob, RFC 4252 §7)
  PASS  transport handshake completed (KEX + host-key)
  PASS  client reported 'authenticated (pubkey)'
  PASS  remote `id.elf` ran as guest (uid=1000) via pubkey-auth'd session
```

**Plus real OpenSSH 10.3 interop**, confirmed end-to-end:

```
$ ssh-keygen -t ed25519 -N "" -f /tmp/aos_test_key -C interop-test
$ cat /tmp/aos_test_key.pub
ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIMcnolU/V5GZ... interop-test

# Add `guest ssh-ed25519 AAAAC3...` to fs/ssh_keys, rebuild.

$ ssh -i /tmp/aos_test_key -o PreferredAuthentications=publickey \
      -o Ciphers=aes128-gcm@openssh.com -p 2222 guest@127.0.0.1 'id.elf'
uid=1000 gid=1000 pid=16 pgid=16
```

No prompts, no password — pure key auth, an OpenSSH client talking to our 320-KB sshd over the wire.

## What's in scope

In:

- **`user/sshd.c`** — a `publickey` branch in `do_userauth` that:
  - Parses `(has_signature, algorithm, pubkey_blob, [signature_blob])` from the USERAUTH_REQUEST.
  - For `has_sig=FALSE` (probe), looks up the key in the authorized list and replies `SSH_MSG_USERAUTH_PK_OK` (60) with the same algo + blob if accepted.
  - For `has_sig=TRUE`, parses the signature blob, builds the canonical auth-blob from RFC 4252 §7, and runs `ed25519_verify`.
- **`user/sshd.c` boot path** — derives a demo ed25519 pubkey from a fixed seed, registers it for `guest` in an in-memory `g_auth_keys[]` table; then parses `/etc/ssh_keys` (if present) for additional user-supplied keys via a tiny base64 decoder.
- **`fs/ssh_keys`** — new shipped file. Empty except for comments and a template that documents the line format. Users append their own `<user> ssh-ed25519 <base64> comment` lines.
- **`user/ssh.c`** — gain `do_pubkey_auth` for the matching client side: derive the demo keypair from the same seed, send the probe, on PK_OK build the canonical auth-blob and sign it, send the signed USERAUTH_REQUEST. Triggered by `@key` as the password-slot placeholder.
- **`libcrypto/ssh.h`** — add `SSH_MSG_USERAUTH_PK_OK = 60`.
- **`mkfs.py`** — ship `fs/ssh_keys` as `/etc/ssh_keys`.
- **`user/sh.c`** — `[t35]` selftest driving the full pubkey loopback.

Out:

- **Algorithms other than `ssh-ed25519`.** A modern OpenSSH client offers `ssh-ed25519-cert-v01`, `sk-ssh-ed25519`, `rsa-sha2-256`, etc. as well. We reject anything that isn't bare `ssh-ed25519` — server-sig-algs negotiation and certificate keys are future work.
- **Encrypted private key files.** OpenSSH client handles `-i` with its own decryption; sshd never sees the private key, so this isn't a server-side concern.
- **`AuthorizedKeysCommand` or LDAP-style external lookups.** The list lives in a static file plus an in-memory demo key.
- **Hostbased / kerberos / keyboard-interactive auth methods.** Only `publickey` and `password` (from session 51) are advertised in USERAUTH_FAILURE.

## The two-step userauth dance

OpenSSH (and our `ssh.elf @key`) do the userauth in two USERAUTH_REQUEST round-trips:

```
client → server:  USERAUTH_REQUEST
                    user="guest"  service="ssh-connection"
                    method="publickey"
                    has_sig=FALSE
                    algo="ssh-ed25519"
                    pubkey_blob=<string("ssh-ed25519") || string(pk_32)>

server → client:  USERAUTH_PK_OK (60)
                    algo="ssh-ed25519"
                    pubkey_blob=<same as above>
```

This first round is the **probe**: the client says "I'd like to authenticate with this key — is it on your list?" The server answers without committing to authentication; it only confirms the key is in scope. The client only computes a signature if the server says yes.

The point of the probe: a client may have several keys (`~/.ssh/id_ed25519`, `~/.ssh/id_rsa`, etc.). Probing avoids producing real signatures for every key against every server — useful for both privacy (signatures over the session_id reveal which key was used) and CPU (ed25519 sign isn't free in pre-quantum-secure contexts).

After PK_OK arrives, the client builds the **canonical auth-blob** (RFC 4252 §7, this exact byte layout):

```
string    session_id              ← H from the first KEX, 32 bytes
byte      SSH_MSG_USERAUTH_REQUEST  (50)
string    user
string    "ssh-connection"
string    "publickey"
boolean   TRUE                      ← (has_signature)
string    "ssh-ed25519"
string    pubkey_blob               ← the same nested-string thing
```

The signature is `ed25519_sign(sk, auth_blob)`. Wrapped as another SSH-format blob:

```
sig_blob = string("ssh-ed25519") || string(sig_64)
```

Then the client sends:

```
client → server:  USERAUTH_REQUEST
                    user="guest"  service="ssh-connection"
                    method="publickey"
                    has_sig=TRUE
                    algo="ssh-ed25519"
                    pubkey_blob=<...>
                    sig_blob=<...>

server → client:  USERAUTH_SUCCESS  (52)
```

The server re-computes the same auth-blob (it knows session_id and all the public fields), calls `ed25519_verify(sig, blob, pk)`, and on success replies `USERAUTH_SUCCESS`.

**Why session_id is in there:** it binds the signature to *this* SSH session. An attacker who captures a signed auth-blob can't replay it against a different session (different KEX → different session_id → signature won't verify). It's the SSH equivalent of TLS's channel-binding.

## The authorized list — in-memory + /etc/ssh_keys

`struct auth_key { char user[32]; uint8_t pubkey[32]; }` lives in a fixed-size table (`AUTH_KEYS_MAX = 16`). Two sources fill it at boot:

1. **Hardcoded demo key.** `DEMO_USER_SEED` is a 32-byte constant duplicated in both `sshd.c` and `ssh.c`. At startup, sshd does:

   ```c
   uint8_t demo_pk[32], demo_sk[64];
   ed25519_keypair_from_seed(demo_pk, demo_sk, DEMO_USER_SEED);
   add_auth_key("guest", demo_pk);
   ```

   This is what makes the in-OS loopback selftest work without any filesystem state — both sides derive the same keypair, the server has the pubkey in its list, the client signs with the private key.

2. **`/etc/ssh_keys`** — text file, one entry per line, OpenSSH `authorized_keys` shape with a leading username:

   ```
   guest ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAI... my-laptop-2026
   root  ssh-ed25519 AAAAC3...                  ops-jumpbox
   ```

   The parser is `~70 lines`: walk lines, skip `#`-comments, split on whitespace, base64-decode the blob, verify it's the expected `string("ssh-ed25519") || string(pubkey)` shape, register the inner 32 bytes.

   The base64 decoder is a 25-line standard-alphabet implementation (no URL-safe variant; `=` padding is consumed, whitespace tolerated):

   ```c
   static int b64decode(const char *in, int len, uint8_t *out) {
       static const char alpha[] =
           "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
       int o = 0, bits = 0, val = 0;
       for (int i = 0; i < len; i++) {
           char c = in[i];
           if (c == '=') break;
           if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
           int v = -1;
           for (int j = 0; j < 64; j++) if (alpha[j] == c) { v = j; break; }
           if (v < 0) return -1;
           val = (val << 6) | v;
           bits += 6;
           if (bits >= 8) {
               bits -= 8;
               out[o++] = (uint8_t)((val >> bits) & 0xff);
           }
       }
       return o;
   }
   ```

   Why a single file and not per-user `~/.ssh/authorized_keys`: AdventFS's filenames cap at 16 chars (per session 25); per-user dot-paths run past that. The single-file format is functionally equivalent and easier to parse.

## OpenSSH interop, step by step

The same flow that worked above, written out:

```bash
# 1. Generate a fresh ed25519 keypair on the host. No passphrase
#    so the demo doesn't need ssh-agent.
ssh-keygen -t ed25519 -N "" -f /tmp/aos_test_key

# 2. Cat the .pub file. Format is `ssh-ed25519 <base64> <comment>`.
cat /tmp/aos_test_key.pub

# 3. Drop a line into AdventOS's fs/ssh_keys:
#
#    guest ssh-ed25519 <base64-from-step-2> my-key
#
#    (Prefix the line with the AdventOS user you want it to log in as.)
#    Then rebuild os.img so the new file lands at /etc/ssh_keys.
bash build.sh

# 4. Boot AdventOS with the standard hostfwd:
qemu-system-i386 -drive format=raw,file=os.img -display none \
    -netdev user,id=net0,hostfwd=tcp::2222-:2222 \
    -device rtl8139,netdev=net0  ...

# 5. SSH in with -i pointing at the private key. With -o
#    PreferredAuthentications=publickey, OpenSSH won't fall back
#    to passwords, so a missing/wrong key will produce a clean
#    "Permission denied (publickey,password)".
ssh -i /tmp/aos_test_key \
    -o KexAlgorithms=curve25519-sha256 \
    -o HostKeyAlgorithms=ssh-ed25519 \
    -o Ciphers=aes128-gcm@openssh.com \
    -o PreferredAuthentications=publickey \
    -p 2222 guest@127.0.0.1 'id.elf'
# uid=1000 gid=1000 pid=16 pgid=16
```

## Negative test

Without the registered key, OpenSSH gets a clean refusal with the supported-methods list:

```
$ ssh -i /tmp/unregistered_key -o PreferredAuthentications=publickey \
      -p 2222 guest@127.0.0.1 'id'
guest@127.0.0.1: Permission denied (publickey,password).
```

`(publickey,password)` is the `name_list` in our `USERAUTH_FAILURE` — it's how a real OpenSSH server tells clients which methods to try. We advertise both since session 51 wired up password auth.

## Selftest [t35]

The loopback exercise spawns `ssh.elf 127.0.0.1 guest @key id.elf`. The `@key` placeholder routes through `do_pubkey_auth` (instead of `do_userauth`), which derives the demo keypair, sends the probe, signs the canonical blob on PK_OK, and resends with the signature. Then it's the normal session-50 exec path for the `id.elf` command.

Three assertions check the captured output:

```c
if (i + 22 <= total && captured[i] == 'a' &&
    memcmp(captured + i, "authenticated (pubkey)", 22) == 0) find_pubkey = 1;
```

The `(pubkey)` parenthetical comes from the new `printf("ssh: authenticated (%s)\n", used_pubkey ? "pubkey" : "password")` in `ssh.c`. Three PASSes, no FAILs.

## Files touched

```
libcrypto/ssh.h          +1     SSH_MSG_USERAUTH_PK_OK = 60
user/sshd.c              +180   publickey branch + base64 + auth_keys table + ssh_keys parser
user/ssh.c               +90    DEMO_USER_SEED + do_pubkey_auth, @key dispatch in main
user/sh.c                +60    [t35] pubkey loopback selftest
fs/ssh_keys              +new   default authorized_keys, template + docs
mkfs.py                  +1     ship fs/ssh_keys as /etc/ssh_keys
docs/53-pubkey-auth.md   +new   this file
```
