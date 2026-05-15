# AdventOS agent API reference

One place to look up everything an external agent (or in-guest tool)
can do with AdventOS through `agentd`, `kvctl`, the new syscalls,
and procfs. Consolidates the surfaces added across sessions 70–77;
session-by-session deep dives remain in `docs/64-agent-rpc.md`,
`docs/65-mcp-server.md`, and the inline comments in `user/agentd.c`.

> **Contributor note.** Every code change that touches an
> agent-facing surface (a tool, a notification, a capability flag,
> a syscall, a procfs file) must update this document in the same
> commit. If the doc lags reality, agents reading it write broken
> code; if it leads reality, they write code that doesn't run yet.
> Both are worse than no doc. Companion: `docs/agent-cookbook.md`
> for paste-able recipes.

## Overview

`agentd` is a JSON-RPC 2.0 + MCP server on TCP `127.0.0.1:7000`.
Newline-delimited framing. Loopback only — uid 0 inside the guest,
no auth; perimeter is whatever's reaching the loopback (sshd inside
the guest, or QEMU's hostfwd from outside).

Two coexisting dispatch shapes share the same socket:

- **Direct**: `{"method":"shell.exec", "params":{...}}` →
  `{"result":{...}}`. The session-64 path; still works.
- **MCP**: `{"method":"tools/call", "params":{"name":"shell.exec",
  "arguments":{...}}}` → `{"result":{"content":[{"type":"text",
  "text":"..."}], "isError":false}}`. What real Claude / Cline / etc
  clients speak after handshaking via `initialize` + `tools/list`.

Inside the guest, three shell binaries front-end the same surface:

- `agentctl` — JSON-RPC client (session 74). `agentctl demo` walks an
  end-to-end exercise of the background-job tools.
- `kvctl` — direct libuser path to the KV store (session 73). Bypasses
  agentd; useful when paste-into-shell is unreliable.
- `sandbox` — wrapper that installs a syscall-allow mask then execs
  its target (session 70).

## JSON-RPC tools

Every tool here is reachable two ways:

- `{"method":"<name>","params":{...}}` (direct)
- `{"method":"tools/call","params":{"name":"<name>","arguments":{...}}}` (MCP)

`<name>` and `params`/`arguments` are identical in both forms.

| Tool | Input | Output | Added |
|---|---|---|---|
| `time` | — | `{epoch}` | session 64 |
| `getuid` | — | `{uid, gid}` | session 64 |
| `dns_resolve` | `{host}` | `{ip}` | session 64 |
| `dhcp_info` | — | `{have_lease, ip, gateway, dns_server, ...}` | session 64 |
| `dns_cache_stats` | — | `{lookups, hits, misses, evictions}` | session 64 |
| `fbinfo` | — | `{enabled, width, height, bpp, pitch}` | session 64 |
| `smp_stats` | — | `{nr_cpus, ticks[]}` | session 64 |
| `shell.exec` | `{cmd, args[]}` | `{exit_code, stdout, stderr}` | session 64 |
| `shell.exec_sandboxed` | `{cmd, args[], policy, limits?}` | `{..., policy, child_pid, sandbox_log, limits_state}` | session 70+71 |
| `kv.get` | `{namespace, key}` | `{found, value}` | session 73 |
| `kv.put` | `{namespace, key, value}` | `{ok}` | session 73 |
| `kv.del` | `{namespace, key}` | `{ok, existed}` | session 73 |
| `kv.list` | `{namespace, prefix?}` | `{keys:[...]}` | session 73 |
| `kv.stat` | `{namespace, key}` | `{exists, size}` | session 73 |
| `kv.watch` | `{namespace, prefix?}` | `{watch_id}` | session 76 |
| `kv.unwatch` | `{watch_id}` | `{ok}` | session 76 |
| `shell.exec_background` | `{cmd, args[], policy?, limits?}` | `{job_id, pid}` | session 74 |
| `shell.job.list` | — | `{jobs:[...]}` | session 74 |
| `shell.job.status` | `{job_id}` | `{job_id, pid, cmd, state, done, exit_code?, ...}` | session 74 |
| `shell.job.read` | `{job_id, stdout_offset?, stderr_offset?, max?}` | `{stdout, stderr, stdout_next, stderr_next, ..., done, exit_code?}` | session 74 |
| `shell.job.wait` | `{job_id, timeout_ms?}` | `{job_id, exit_code?, done, timed_out}` | session 74 |
| `shell.job.cancel` | `{job_id}` | `{job_id, killed}` | session 74 |
| `shell.job.delete` | `{job_id}` | `{job_id, removed, reason?}` | session 74 |
| `shell.job.subscribe` | `{job_id}` | `{job_id, subscribed, stdout_total, stderr_total}` | session 74 |
| `shell.job.unsubscribe` | `{job_id}` | `{job_id, subscribed}` | session 74 |
| `cron.create` | `{kind, fire_at? \| delay_sec?, interval_sec?, max_runs?, concurrent?, cmd, args?, policy?, limits?}` | `{entry_id, fire_at}` | session 77 |
| `cron.list` | `{filter?: {state?}}` | `{entries:[...]}` | session 77 |
| `cron.get` | `{entry_id}` | `{entry:{...}}` | session 77 |
| `cron.cancel` | `{entry_id}` | `{ok, was_scheduled}` | session 77 |
| `cron.delete` | `{entry_id}` | `{ok, removed}` | session 77 |
| `cron.subscribe` | `{entry_id}` | `{ok}` | session 77 |
| `cron.unsubscribe` | `{entry_id}` | `{ok}` | session 77 |

Sandbox policy names (used in `shell.exec_sandboxed` /
`shell.exec_background` / `cron.create`): `minimal | compute | readfs
| netclient`, each strictly broader than the prior. `limits` is an
optional object with non-negative integer fields `max_rss_kb`,
`max_cpu_ms`, `max_fds`, `max_wall_ms`; zero or absent = no cap.

### Two dispatch shapes for the same tools

For testing tools where you want to parse the response with a plain
substring search (the in-guest selftests do this via libagent's
`agent_method_call`), use the direct shape:

```jsonc
{"jsonrpc":"2.0","id":1,"method":"shell.exec_background",
 "params":{"cmd":"/ls.elf"}}
// -> {"jsonrpc":"2.0","id":1,"result":{"job_id":0,"pid":42}}
```

For MCP-aware clients (Claude Desktop, Cline, etc.), the same tool
goes through `tools/call`:

```jsonc
{"jsonrpc":"2.0","id":1,"method":"tools/call",
 "params":{"name":"shell.exec_background","arguments":{"cmd":"/ls.elf"}}}
// -> {"jsonrpc":"2.0","id":1,"result":{
//        "content":[{"type":"text","text":"{\"job_id\":0,\"pid\":42}"}],
//        "isError":false}}
```

The MCP envelope JSON-string-encodes the inner result, so the
quotes around `"job_id"` become `\"job_id\"` on the wire. Programs
that parse the response need to either un-escape the inner string
or use the direct shape.

## MCP resources (session 73)

`resources/list`, `resources/templates/list`, `resources/read` —
read-only static and templated URIs. The agent uses these instead of
shell.exec-ing `cat`.

Static (10):

```
file:///proc/cpuinfo            file:///proc/meminfo
file:///proc/uptime             file:///proc/version
file:///proc/mounts             file:///proc/bcache
file:///etc/inittab             file:///etc/passwd  (hashes redacted)
file:///etc/resolv.conf         file:///etc/agent.tools.json
```

Templated (4):

```
file:///proc/{pid}/status
file:///proc/{pid}/sandbox      (session 70)
file:///proc/{pid}/limits       (session 71)
kv://{namespace}/{key}          (mirror of kv.get)
```

`resources/read {uri}` returns `{contents:[{uri, mimeType, text,
truncated?}]}`. 4 KiB read cap per call.

## Notifications (sessions 74 + 76 + 77)

Server-initiated JSON-RPC messages (no `id` field). Each fits in a
single TCP segment (≤ 1280 bytes); writes that would block or fail
are dropped on the floor.

| Notification | When | Subscription path |
|---|---|---|
| `notifications/job.output` | New bytes on a job's stdout/stderr | `shell.job.subscribe` (session 74) |
| `notifications/job.exit` | Job's pid reaped + pipes drained | `shell.job.subscribe` (session 74) |
| `notifications/resources/updated` | Subscribed URI's content hash changed (200 ms tick, FNV-1a) | `resources/subscribe` (session 76) |
| `notifications/kv/changed` | `kv.put`/`kv.del` on a matching (namespace, prefix) | `kv.watch` (session 76) |
| `notifications/cron.fired` | A scheduled cron entry just spawned its job | `cron.subscribe` (session 77) |

Wire shapes:

```jsonc
// session 74
{"jsonrpc":"2.0","method":"notifications/job.output",
 "params":{"job_id":0,"stream":"stdout","data":"..."}}
{"jsonrpc":"2.0","method":"notifications/job.exit",
 "params":{"job_id":0,"exit_code":0}}

// session 76 — MCP-standard, uri only (clients call resources/read to fetch)
{"jsonrpc":"2.0","method":"notifications/resources/updated",
 "params":{"uri":"file:///proc/3/status"}}

// session 76 — AdventOS extension, op = "put" | "del"
{"jsonrpc":"2.0","method":"notifications/kv/changed",
 "params":{"namespace":"smoke","key":"counter","op":"put"}}

// session 77 — AdventOS extension
{"jsonrpc":"2.0","method":"notifications/cron.fired",
 "params":{"entry_id":5,"job_id":42,"fire_at":1715692800,"run_count":3}}
```

The `initialize` handshake advertises all three feature groups:

```jsonc
"capabilities": {
  "resources": {"subscribe": true, "listChanged": false},   // session 76 turned subscribe on
  "experimental": {
    "adventos.jobs":      {"version":1, "max_jobs":8, "ring_bytes":4096,
                           "notifications":["notifications/job.output",
                                            "notifications/job.exit"]},
    "adventos.kv_watch":  {"version":1, "max_concurrent_watches":16,
                           "notifications":["notifications/kv/changed"]},
    "adventos.cron":      {"version":1, "max_entries":32,
                           "min_interval_sec":1,
                           "kinds":["oneshot","recurring"],
                           "notifications":["notifications/cron.fired"]}
  }
}
```

### Subscribing to volatile URIs

`file:///proc/uptime` changes every PIT tick — subscribing means
the daemon emits a `notifications/resources/updated` for it every
200 ms forever. Same for `/proc/meminfo` and any `/proc/<pid>/status`
of a busy process. Agents that don't want the firehose should poll
via `resources/read` instead, OR subscribe to URIs whose contents
change only occasionally (KV files, `/etc/*`, `/proc/bcache`).

### `kv.watch` vs `resources/subscribe` on `kv://`

Both can deliver KV-change notifications, but with different
trade-offs:

| Mechanism | Latency | Precision | Notification carries |
|---|---|---|---|
| `kv.watch` | Immediate (same tick as the write) | Prefix match across many keys | `{namespace, key, op}` |
| `resources/subscribe kv://NS/KEY` | Up to 200 ms | Single key | `{uri}` |

Use `kv.watch` when watching a class of keys or when latency matters.
Use `resources/subscribe` when watching one specific key and you're
already using the MCP-standard subscribe path for everything else.

## KV store layout (session 73)

Persistent key/value store backed by AdventFS:

```
/var/kv/<namespace>/<key>
```

- Namespace: 1..32 chars, `[A-Za-z0-9_-]`
- Key:       1..64 chars, `[A-Za-z0-9_.-]`, no leading dot, no `/`
- Value:     up to 65 536 bytes per key

Validation lives in `user/libuser.c` (`kv_validate_ns`,
`kv_validate_key`). Path-traversal attempts (e.g.
`kv.put("..","x","y")` or `kv.put("ns","../escape","y")`) return -1
before any FS call. Namespace directories are created on first
`kv.put` (no explicit `kv.mkns` call). On the host, use `kvctl get /
put / del / list / stat` for paste-free testing.

## Cron layout (session 77)

Durable scheduler. Each entry persisted as one JSON file:

```
/var/cron/<entry_id>.json
```

Two kinds:

- **oneshot** — fires once at `fire_at` (or `now + delay_sec`), then
  transitions to `expired`.
- **recurring** — fires every `interval_sec` until cancelled or
  `max_runs` is hit. `max_runs: 0` means unlimited.

The event loop runs a 1 Hz cron tick. Each due entry spawns a
session-74 background job; the entry's `last_job_id` points at the
slot, so an agent can immediately `shell.job.read` / `shell.job.wait`
on the spawned run.

`concurrent: false` (the default) skips a fire when the previous
run is still RUNNING; `fire_at` advances anyway so the schedule
doesn't drift. `concurrent: true` overlaps fires.

Boot recovery scans `/var/cron` and rebuilds `g_cron[]`. Past-due
entries fire on the next tick (single fire, no missed-interval
replay). The `cron_next_id` counter advances past the highest
loaded id so future entries stay monotonic.

States:

| State | Meaning |
|---|---|
| `scheduled` | due to fire (or recurring with `run_count < max_runs`) |
| `cancelled` | user called `cron.cancel`; no further fires; record stays for inspection |
| `expired` | oneshot fired OR recurring hit `max_runs` |

Agents are expected to `cron.delete` entries they no longer care
about; there's no auto-GC of `expired` records.

## Syscalls added since session 70

All match the kernel/syscall.h numbering.

| # | Name | Args | Returns | Session |
|---|---|---|---|---|
| 82 | `SYS_SANDBOX_INSTALL` | `mask[4]` ptr | 0 / -1 | 70 |
| 83 | `SYS_SETLIMIT` | `struct sys_limits *` | 0 / -1 | 71 |
| 84 | `SYS_UNLINK` | path | 0 / -1 | 73 |

Semantics:

- **`sys_sandbox_install`**: first call installs `mask` as the
  task's syscall allow-bitmap, sets `sandbox_active=1`. Subsequent
  calls AND-in (tighten only). Children inherit verbatim across fork
  AND across exec. Clearing the `SYS_SANDBOX_INSTALL` bit before a
  later call freezes the policy permanently.
- **`sys_setlimit`**: tightens caps to `min(current, new)`.
  Zero-in-a-field means "leave alone". Caps survive fork+exec.
  `max_cpu_ms` is checked from the PIT tick handler;
  exhaustion pends `SIGKILL`. `max_rss_kb` is enforced at mmap-fault
  + `sys_brk` page-alloc time. `max_fds` is enforced inside
  `alloc_fd`. `max_wall_ms` is converted to an absolute deadline
  tick at `sys_setlimit` time and checked from `pit_tick`.
- **`sys_unlink`**: removes a regular file. Refuses directories and
  files held open by any task. Owner-or-root check.

The libuser-side helpers cover the common shapes:
`sandbox_policy_{minimal,compute,readfs,netclient}` populate a mask
for the named template; `limits_default(&l)` zeros all fields.

## procfs files

| Path | What | Session |
|---|---|---|
| `/proc/<pid>/sandbox` | active bit, denial count, mask, last 16 denied calls | 70 |
| `/proc/<pid>/limits` | per-task caps + current usage (RSS / CPU / fds / wall) | 71 |

Both regenerate on every read into a 2 KiB stack buffer (session 75
bump from 1 KiB; near-overflow `kprintf` tripwire on the read path).

Format of `/proc/<pid>/sandbox`:

```
Active:    1
Denials:   3
Mask:      00000001 00000000 00000000 00000000
Recent:
  tick=12345 sc=10 SYS_OPEN
  tick=12346 sc=11 SYS_READ
  tick=12346 sc=200 SYS_???
```

Format of `/proc/<pid>/limits`:

```
RssPages:   42 / 256
CpuTicks:   100 / -
Fds:        4 / 16
WallTicks:  1234 / -
```

`-` in the cap half means "no cap set".

## Cookbook: first agent session

Open a TCP connection to `127.0.0.1:7000` and write line-delimited
JSON. Replace `id` with whatever your client uses.

```jsonc
// 1. Handshake.
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}

// 2. Discover tools.
{"jsonrpc":"2.0","id":2,"method":"tools/list"}

// 3. Discover resources.
{"jsonrpc":"2.0","id":3,"method":"resources/list"}
{"jsonrpc":"2.0","id":4,"method":"resources/templates/list"}

// 4. Run a sandboxed command.
{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{
  "name":"shell.exec_sandboxed",
  "arguments":{"cmd":"ls.elf","args":["/etc"],
               "policy":"readfs",
               "limits":{"max_cpu_ms":1000,"max_fds":16}}}}

// 5. Inspect what just happened.
{"jsonrpc":"2.0","id":6,"method":"resources/read","params":{
  "uri":"file:///proc/47/sandbox"}}
```

For a long-running command, use background jobs:

```jsonc
// Spawn.
{"jsonrpc":"2.0","id":10,"method":"shell.exec_background",
 "params":{"cmd":"wget.elf","args":["http://10.0.2.2:8080/big"]}}
// -> {"result":{"job_id":0,"pid":42}}

// Subscribe to push updates.
{"jsonrpc":"2.0","id":11,"method":"shell.job.subscribe",
 "params":{"job_id":0}}

// ... server pushes notifications/job.output as bytes arrive,
// then a notifications/job.exit when the child reaps.

// Or pull on a schedule.
{"jsonrpc":"2.0","id":12,"method":"shell.job.read",
 "params":{"job_id":0,"stdout_offset":0}}
// -> {"result":{"stdout":"...","stdout_next":128,"done":false,...}}
```

## Selftests (sessions 75 + 78)

Six standalone binaries verify the sessions 70–77 surfaces end-to-end
without an external client. Run from the in-guest shell:

```
advent$ sbx-selftest        # session 70 — sandbox
advent$ lim-selftest        # session 71 — resource limits
advent$ kv-selftest         # session 73 — KV store
advent$ job-selftest        # session 74 — background jobs
advent$ sub-selftest        # session 76 — resources/subscribe + kv.watch
advent$ crn-selftest        # session 77 — cron scheduler
advent$ selftest            # meta-runner: forks each in turn, prints summary
```

The short prefixes are because `FS_NAME_MAX` is 16 chars and the
shell auto-appends `.elf`; `sbx-selftest.elf` fits exactly. Source
files keep the descriptive `sandbox-selftest.c` etc names.

The agentd-talking tests (jobs / sub / crn) link against
`user/libagent.{c,h}` — a thin TCP+JSON-RPC client (`agent_call`,
`agent_method_call`, `agent_tool_call`, `agent_open_persistent`).
Each prints one PASS/FAIL per check and exits 0 only on full pass.
The meta-runner fork+execs each test (so sandbox/limits selftests'
self-installed policies don't leak forward) and reports a summary.

## Known limits

Counted with how-tight-is-the-ceiling for honest planning:

- 4 simultaneous TCP connections to agentd (`MAX_CONN`)
- 8 simultaneous background jobs (`JOB_MAX`)
- 4 KiB per stream per job ring buffer (`JOB_RING_SZ`)
- 16 unique subscribed resource URIs (`MAX_RES_URIS`)
- 32 total `(conn, uri)` subscription pairs (`MAX_RES_SUBS`)
- 16 simultaneous `kv.watch` registrations (`MAX_KV_WATCHES`)
- 32 cron entries (`MAX_CRON_ENTRIES`)
- 4 subscribers per cron entry (`CRON_MAX_SUBSCRIBERS`)
- 65 536 bytes per KV value
- 128 file slots on the boot fs (`FS_MAX_FILES`)
- 8 192 sectors visible to the FS bitmap (~4 MiB usable)
- 200 ms polling latency for `resources/subscribe`
- 1 Hz tick granularity for cron

## Quirks worth knowing

- Subscribing to a volatile resource (e.g. `file:///proc/uptime`,
  which changes every PIT tick) means the daemon emits a
  `notifications/resources/updated` for it every 200 ms forever.
  Document not a limit — there's no per-URI rate-limit.
- Job stdout/stderr rings cap at 4 KiB; older bytes drop. The
  `stdout_skipped` / `stderr_skipped` fields on `shell.job.read`
  tell the caller how many bytes were lost since their last
  `*_offset`.
- `kv.put` writing the same value as before still triggers
  `notifications/kv/changed`. The op happened — let the watcher
  decide whether to care; cheaper than diffing on every write.
- `kv.del` of a nonexistent key also triggers a notification with
  `op: "del"`. The response's `{existed:false}` field distinguishes
  no-op from real-delete.
- `shell.job.wait` may defer its response across event-loop ticks;
  it parks the conn and the response arrives when the job exits or
  the timeout elapses. The dispatch is transparent to the client —
  recv on the same socket returns the response whenever it lands.
- Cron entries with `concurrent: false` (the default) skip a fire
  when the previous run's job is still RUNNING. `fire_at` advances
  regardless; `run_count` does NOT — the skipped slot is dropped.
- Boot-recovered cron entries fire on the very next tick if their
  `fire_at` is already past. No multi-fire catch-up for missed
  intervals.
