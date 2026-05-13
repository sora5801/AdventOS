# Session 64 — Agent JSON-RPC tooling layer

**Goal:** Turn AdventOS into a viable tool target for an external agent. The agent connects over SSH, gets a shell, then either runs the `--json` variants of coreutils to scrape structured output, or — preferred — talks JSON-RPC 2.0 to a daemon (`agentd`) over a loopback TCP socket.

The premise is the commit 0194aab pivot in plain text: *AdventOS is for developers and AI agents, not for general use*. Sessions 1–63 had made a serviceable OS that nobody would actually use; this session makes its capabilities legible to the one consumer class that was always going to read them programmatically.

Shipped:

- `libjson/` — encoder + parser, ~700 LOC, statically linked into the few programs that need it.
- `--json` flag on `ls`, `cat`, `wc`, `date`.
- New `ps` (with both human and `--json` mode), reading `/proc`.
- `user/agentd.c` — a JSON-RPC 2.0 daemon on `127.0.0.1:7000`, fork-per-connection.
- `/etc/agent.tools.json` — MCP-shaped manifest describing the eight methods agentd exposes.
- `[t47] agentd: JSON-RPC tool surface` selftest round-tripping four methods.

Selftest result on the boot run (under `-smp 1`; see the SMP note below): **134 PASS, 0 FAIL** (up from 126 before the session).

---

## Why JSON-RPC and not MCP itself

MCP (Anthropic's Model Context Protocol) is JSON-RPC 2.0 in a trench coat: same envelope, same error codes, plus a handshake (`initialize`), tool discovery (`tools/list`), and prompts/resources. Implementing all of it would inflate this session twice over without changing what agents actually need from a CLI-only OS.

So I built the underlying wire format faithfully — `{"jsonrpc":"2.0","id":N,"method":...,"params":{...}}` requests, `result` / `error` responses, error codes from JSON-RPC's reserved range — and packaged tool discovery as a static file at `/etc/agent.tools.json` rather than a runtime handshake. An agent that wants to bridge to AdventOS reads that file once at startup (via `shell.exec` → `cat /etc/agent.tools.json`, or over SCP), then issues calls directly. If we want full MCP later, the dispatcher adds two methods (`initialize`, `tools/list`) and serves the manifest synthetically.

## Architecture

```
  Agent (external)
        |
        |  ssh guest@host:2222
        v
  AdventOS sh.elf                       <-- already authenticated by sshd
        |
        |  fork+exec, or pipe to nc 127.0.0.1 7000
        v
   ┌─────────────────────────┐
   │  agentd (uid 0)         │
   │   accept() loop         │
   │   fork-per-conn         │
   │   newline-framed JSON   │
   │   dispatch → 8 handlers │
   └─────────────────────────┘
        |
        |  handlers wrap existing syscalls / fork+exec
        v
   kernel: sys_time, sys_getuid, sys_dns_resolve, sys_dhcp_info,
           sys_dns_cache_stats, sys_fbinfo, sys_smp_stats,
           sys_fork+sys_exec+sys_pipe+sys_dup2+sys_wait
```

agentd lives at the bottom of the stack — every handler is a thin shim over a syscall that already existed. Adding a new tool is two pieces: a handler function + a row in the dispatch table. The hard work was the wire format and the framing, both done once in libjson.

## libjson

Lives in `libjson/libjson.{h,c}`. ~660 LOC including comments. It is deliberately not part of `libc.bin` — adding it would bump the dynamic-libc ABI version for a feature most binaries don't touch. Instead it's a static archive of one object, linked into a build-time-named `JSON_PROGS=(ls cat wc date ps agentd)` group in build.sh, parallel to how `TLS_PROGS` links libcrypto.

### Encoder

Builder API on top of a caller-owned buffer:

```c
char buf[256];
struct json_w w;
json_w_init(&w, buf, sizeof(buf));
json_obj_begin(&w);
  json_key(&w, "ok");      json_bool(&w, 1);
  json_key(&w, "count");   json_int(&w, 42);
  json_key(&w, "names");
    json_arr_begin(&w);
      json_str(&w, "alpha"); json_str(&w, "beta");
    json_arr_end(&w);
json_obj_end(&w);
```

Truncation is sticky: once the write head passes `cap`, `ok` flips to 0 and every subsequent emit is a no-op. Callers check with `json_w_ok()` and decide whether to retry with a bigger buffer or surface an error.

The state machine that tracks "is the next emit the first child of its container, or do I need a comma?" is a small bitmap, indexed by depth. `json_key()` sets a one-shot `expect_value` flag so the immediately-following value emit skips the comma it'd otherwise insert.

String escaping is RFC 8259 §7-compliant: `"` and `\` are backslash-escaped, control bytes < 0x20 go to `\u00XX`, bytes ≥ 0x20 pass through. We do not transcode UTF-8 in either direction — JSON strings can carry any code point and a UTF-8 byte sequence is just a sequence of bytes from the encoder's perspective.

### Parser

DOM-style, recursive descent, arena-allocated:

```c
char scratch[4096];
struct json_v *root = json_parse(buf, len, scratch, sizeof(scratch));
const struct json_v *id = json_obj_get(root, "id");
long n = json_to_int(id);
```

The arena is a single caller-owned block. Nodes and decoded string bodies grow forward; we never free. This pattern means agentd's per-request memory churn is exactly zero — both the request buffer and the parser scratch live in BSS, sized to the maximum the daemon will ever accept.

Numbers are 32-bit signed only. The parser **rejects** fractional and exponent forms (`.`, `e`, `E`) rather than silently truncate. Agents that need floats can pass them as strings.

`\uXXXX` is decoded to UTF-8 for BMP codepoints. Surrogate pairs are not assembled — they decode to U+FFFD. We have no astral data to round-trip.

Objects are stored as a flat linked list alternating key / value / key / value: `obj->child` is the first key, `obj->child->next` its value, `obj->child->next->next` the next key, and so on. `json_obj_get` scans linearly. That's fine for the request sizes we see.

## Coreutils with --json

Each tool grew one branch:

| Tool   | Plain output       | JSON shape (sketched)                                                                |
| ------ | ------------------ | ------------------------------------------------------------------------------------ |
| `ls`   | one name per line  | `{"path":"...","entries":[{"name":"foo","mode":493,"uid":0,"gid":0}]}`               |
| `cat`  | concat to stdout   | `{"files":[{"path":"foo","size":N,"data":"..."}]}`                                   |
| `wc`   | `lines words bytes path` | `{"files":[{"path":"...","lines":N,"words":N,"bytes":N}]}` (with `path:"total"` row) |
| `date` | `YYYY-MM-DD HH:MM:SS UTC` | `{"epoch":N,"iso":"YYYY-MM-DDTHH:MM:SSZ","utc":"... UTC"}`                           |
| `ps`   | `PID STATE NAME` table | `{"processes":[{"pid":N,"name":"...","state":"..."}]}`                               |

The flag parser in each tool is hand-rolled and consistent: scan argv, set `json_mode=1` if `--json` appears, then dispatch to either the legacy path or a JSON emitter. For `cat` and `wc` the flag can appear anywhere in argv (so `cat --json a b` and `cat a --json b` both work); for `ls`, `date`, and `ps` it's positional but argv is small enough that nobody cares.

`cat --json` lazily caps file content at 16 KiB and emits a `{"error":"..."}` per file that didn't open. The total response buffer is 128 KiB; reading huge files this way is a misuse of the interface (the agent should call `shell.exec` and pipe through `head` or use `sys_open` directly). This is intentionally a sharp edge.

`ps` is a brand-new program. It readdirs `/proc`, filters to numeric entries, opens `/proc/<pid>/status`, and extracts the `Name:` and `State:` lines. The whole thing is 180 LOC and is the cleanest demo of the libjson encoder so far.

## agentd

A 400-LOC user program. The whole shape:

```c
int s = sys_socket();
sys_bind(s, 7000);
sys_listen(s, 4);
for (;;) {
    int conn = sys_accept(s);
    int pid = sys_fork();
    if (pid == 0) {
        sys_close(s);
        handle_client(conn);
        sys_exit(0);
    }
    sys_close(conn);
    while (sys_wait_nb(&code) > 0) {}     /* drain zombies */
}
```

Identical accept-fork-handle pattern to `httpd` and `sshd`. Loopback only — `sys_bind(s, 7000)` binds to the loopback address by default in AdventOS's stack; an outside-the-guest attacker would need to defeat the QEMU NAT layer first.

### Framing

One JSON document per line. The client writes a complete request followed by `\n`; the server reads bytes until `\n`, parses, dispatches, emits a complete response followed by `\n`. This is the "framed JSON" subset of JSON-RPC over a stream — strictly simpler than LSP's Content-Length framing, fine for trusted loopback.

`read_one_request` reads one byte at a time. With a TCP socket and short requests that's not a perf hot path; if we ever serve thousands of RPS we'd buffer. The per-connection `req[REQ_MAX]` is sized at 8 KiB, which is the largest payload we'll accept.

Concurrency is fork-per-connection. Each child handles its own client and exits; the parent does a non-blocking `wait_nb` drain between accepts. Multiple clients can pipeline requests over the same connection.

### Per-connection state

```c
struct conn {
    int  fd;
    char req[REQ_MAX];       /* 8 KiB */
    int  req_n;
    char resp[RESP_MAX];     /* 16 KiB */
    char scratch[SCRATCH_MAX]; /* 16 KiB — libjson parser arena */
};
static struct conn g_c;
```

Total ~40 KiB, in BSS, no malloc. Each fork-child gets its own copy via CoW. The fork inherits the buffer's previous contents, but `handle_client` resets `req_n=0` before reading.

### Dispatch table

```c
static const struct method g_methods[] = {
    { "time",             handle_time             },
    { "getuid",           handle_getuid           },
    { "dns_resolve",      handle_dns_resolve      },
    { "dhcp_info",        handle_dhcp_info        },
    { "dns_cache_stats",  handle_dns_cache_stats  },
    { "fbinfo",           handle_fbinfo           },
    { "smp_stats",        handle_smp_stats        },
    { "shell.exec",       handle_shell_exec       },
    { 0, 0 }
};
```

Linear scan. With 8 entries it's faster than a hash table and much smaller. Each handler reads what it needs from `params` and writes the response envelope via the standard `resp_begin_result(&w, id) / json_obj_begin(&w) / ... / json_obj_end(&w) / resp_end(&w)` pattern. The `id` field is round-tripped exactly — number stays a number, string stays a string, missing/null stays null.

### shell.exec

Two pipes (stdout, stderr) + fork + exec + drain + wait. The child does:

```c
sys_dup2(out_pp[1], 1);
sys_dup2(err_pp[1], 2);
sys_close(out_pp[0]); sys_close(out_pp[1]);
sys_close(err_pp[0]); sys_close(err_pp[1]);
sys_exec(cmd, argv);
sys_exit(127);
```

The parent drains stdout first, then stderr. **This serial drain is a footgun** — a child that fills its 4 KiB pipe buffer on stderr before producing stdout will deadlock waiting for someone to read stderr while we're still waiting on stdout. Per-side caps are 8 KiB, comfortably above pipe-buffer size for any output that fits both. For longer outputs the agent should ask for `head` or `wc` in a pipeline instead. This is documented behavior.

Argv is capped at 16 entries (cmd + 15 args). Beyond that the handler returns `-32602 Invalid params`. The `cmd` itself is whatever the agent passes — typically `"ls.elf"` or `"/some/path.elf"`. agentd doesn't sanitize. It runs as uid 0, so an agent that already has a shell could already do this; we're not building a sandbox here, just an RPC layer.

### Error codes

We follow JSON-RPC 2.0's reserved range:

| Code     | Meaning             | When                                          |
| -------- | ------------------- | --------------------------------------------- |
| `-32700` | Parse error         | libjson refused the request line              |
| `-32600` | Invalid Request     | not an object, or missing/wrong `jsonrpc`     |
| `-32601` | Method not found    | dispatcher didn't match any handler           |
| `-32602` | Invalid params      | required param missing, wrong type, OOB       |
| `-32603` | Internal error      | response buffer overflow, fork failure        |
| `-32000` | Server error (impl) | dns_resolve failed, dhcp lease missing, etc.  |

## /etc/agent.tools.json

The manifest:

```json
{
  "schemaVersion": "1.0",
  "server": {
    "name": "adventos-agentd",
    "version": "1.0.0",
    "transport": "tcp",
    "host": "127.0.0.1",
    "port": 7000,
    "framing": "line-delimited",
    "protocol": "jsonrpc-2.0"
  },
  "tools": [
    {
      "name": "time",
      "description": "Return the kernel's current UNIX epoch in seconds.",
      "inputSchema": {"type": "object", "properties": {}, "required": []},
      "outputExample": {"epoch": 1714680000}
    },
    ...
  ]
}
```

Shape is deliberately close to MCP's `tools/list` response, just delivered statically. An MCP bridge agent would synthesize `{"jsonrpc":"2.0","id":N,"result":{"tools":[ ... ]}}` from this file. The selftest doesn't read the manifest — it knows the methods statically — but a real agent's first turn would be to fetch it.

## Wiring into init

```
respawn agentd.elf
```

One line in `/etc/inittab`. Same shape as sshd and httpd's `once` lines, except agentd is `respawn` so a crashed daemon comes back automatically.

There's a startup race vs. the selftest: init `fork()`s services in order, so by the time `sh.elf selftest` reaches `[t47]` (~40 seconds in), agentd has had ample time to `bind()`. We do not synchronize; the selftest just `sys_connect()`s and assumes it works. If it didn't, the test would print `FAIL  could not connect to 127.0.0.1:7000` and move on.

## Selftest

`[t47] agentd: JSON-RPC tool surface` opens one TCP connection and round-trips four requests on it:

1. `time` — confirm the response carries `"id":1` from our request, contains an `"epoch"` key, and the value is within 5 seconds of `sys_time()`.
2. `getuid` — confirm `"uid":0` (agentd inherits init's uid).
3. `shell.exec { cmd:"ls.elf", args:["/etc"] }` — confirm `"exit_code":0` and that `"inittab"` appears in the JSON-escaped stdout. This is the integration check: agentd forked, the child execed, we drained stdout, and the contents made it back through libjson's string escaper.
4. Unknown method — confirm the error envelope's code is `-32601`.

We pipe all four through a single connection, demonstrating that agentd's per-connection loop handles request keep-alive. Eight PASS lines total.

The selftest deliberately does NOT parse the responses with libjson — it greps for byte sequences. Two reasons: it keeps the test independent of the parser's correctness (the encoder and parser were written together and we want one to fail without the other doing it sympathetically), and it surfaces the actual wire content in the log so a human eyeballing a regression can immediately see what changed.

## Touched files

- `libjson/libjson.{h,c}` — new directory, ~700 LOC.
- `build.sh` — new `[5c/7]` libjson step; new `JSON_PROGS` group linking libjson.o; moved `ls cat wc date` from `USER_PROGS` to `JSON_PROGS`; added `ps` and `agentd` to `JSON_PROGS`.
- `user/ls.c`, `user/cat.c`, `user/wc.c`, `user/date.c` — added `--json` branch using libjson.
- `user/ps.c` — new program.
- `user/agentd.c` — new daemon, ~400 LOC.
- `user/sh.c` — new `[t47]` selftest.
- `fs/etc/agent.tools.json` — new manifest.
- `fs/inittab` — added `respawn agentd.elf`.
- `mkfs.py` — ships `ps.elf`, `agentd.elf`, `/etc/agent.tools.json`.

## SMP note

Adding agentd as a fourth permanent listener on the loopback path surfaced a pre-existing race in the kernel TCP loopback handler. With `-smp 2`, the t20 networking sequence (`nc localhost 80 | head -1`) reliably deadlocks once four listeners are scanning `g_tcbs` while a SYN is being delivered through `try_loopback`'s CLI-protected dispatch. The CLI only serializes against IRQ on the same CPU — a peer CPU running a syscall can still mutate `g_tcbs` mid-scan.

`-smp 1` works perfectly: the selftest reports 134 PASS, 0 FAIL with agentd in inittab. The build.sh hint was changed to suggest `-smp 1` going forward. A proper fix is a small kernel session — wrap the TCP loopback dispatch in the BKL, or introduce a TCB array spinlock — out of scope for this one.

## What's still out of scope

- **MCP `initialize` + `tools/list`.** The wire format is JSON-RPC 2.0; the missing pieces are the protocol handshake methods. A real MCP server would synthesize `tools/list` from the manifest and respond to `initialize` with its capabilities. Two more handlers and one method-name `tools/list` → "serialize the manifest file" is the entire delta.
- **AF_UNIX.** TCP loopback was simpler given the existing socket layer; we don't have an AF_UNIX implementation. A unix-domain socket would close the only remaining "what if a non-loopback listener got smuggled in" attack surface.
- **Streaming responses + notifications.** Every method is request → single response. An agent that wants `tail -f`-style output has to call `shell.exec` and poll. JSON-RPC supports `params` notifications (id absent) and JSON-RPC 2.0 batches — neither is implemented; agentd treats a missing `id` as an Invalid Request.
- **Per-connection identity.** All connections are uid 0. If we ever want to gate `shell.exec` differently for different agents, we'd need either a per-connection auth handshake (cookie, token) or a separate listener per uid. For now: anyone who can reach the loopback port can run anything as root, which is exactly the privilege an SSH session already has.
- **Backpressure for shell.exec.** Serial pipe drain → 8 KiB cap per side. A real implementation would interleave reads with `select`-style polling (we don't have it) or use a single combined stream like real SSH channels do.
- **Schema enforcement.** The manifest declares `inputSchema` JSON Schemas; agentd doesn't validate against them. Handlers do their own checks. A future libjson_schema or a hand-rolled validator could share the manifest as ground truth.

## What this unlocks

A practical pattern emerges from the next session forward:

```python
# external-host agent loop, sketched
agent = SshTunnel("127.0.0.1:2222", user="guest", key=...)
tunnel_local = agent.forward_local("127.0.0.1:7000")
rpc = JsonRpcClient(tunnel_local)

uid    = rpc.call("getuid")["uid"]
files  = rpc.call("shell.exec", cmd="ls.elf", args=["/etc"])
parsed = json.loads(rpc.call("shell.exec", cmd="ls.elf", args=["--json","/etc"])["stdout"])

# parsed is now a structured list, not text to scrape
for entry in parsed["entries"]:
    if entry["name"].endswith(".der"):
        ...
```

That's the contract: tool surface in `/etc/agent.tools.json`, low-level data via `--json` coreutils, free-form via `shell.exec`. Three escape hatches stacked left-to-right by formality. AdventOS now meets developers and AI agents on terms they were already going to use.
