# AdventOS agent cookbook

Paste-able recipes for the agent surface. One section per scenario,
each with: setup, the JSON-RPC calls, the expected responses, what
to grep for. No narrative beyond what's needed to read the calls;
deeper explanations live in `docs/agent-api.md`.

All examples assume `agentd` is reachable at `127.0.0.1:7000`. From
the host with the QEMU hostfwd, `nc 127.0.0.1 7000`. From the guest,
`agentctl` or `nc 127.0.0.1 7000`. Requests are line-delimited
JSON-RPC 2.0.

Two dispatch shapes mean the same thing for any tool here — `tools/
call` wraps the result in an MCP content block, the direct method
returns the bare result. Recipes use the direct shape for readability;
swap in the MCP wrapper when talking to a real MCP client.

---

## Recipe 1: list a directory under a sandbox, observe denials

**What:** spawn `/ls.elf /etc` under the `readfs` policy + a 1 s
CPU cap, then peek at the sandbox state of the child to see whether
any denied syscalls accumulated.

```jsonc
// 1. Sandboxed exec — runs ls.elf, returns when it does.
{"jsonrpc":"2.0","id":1,"method":"shell.exec_sandboxed",
 "params":{"cmd":"/ls.elf","args":["/etc"],
           "policy":"readfs",
           "limits":{"max_cpu_ms":1000,"max_fds":16}}}

// Response shape (truncated):
// {"jsonrpc":"2.0","id":1,"result":{
//    "exit_code":0,
//    "stdout":"inittab\npasswd\nresolv.conf\nssh_keys\nagent.tools.json\nssl\n",
//    "stderr":"",
//    "policy":"readfs",
//    "child_pid":42,
//    "sandbox_log":"Active:    1\nDenials:   0\nMask:      ...\n",
//    "limits_state":"RssPages:   12 / -\nCpuTicks:   3 / 100\n..."}}
```

To verify the policy actually restricted something, grep the
`sandbox_log` field for `Denials:` — `readfs` allows read I/O but
not write or socket. A sandbox-violating call would show up as e.g.:

```
Recent:
  tick=12345 sc=14 SYS_SOCKET
```

`/proc/<child_pid>/sandbox` carries the same content and is readable
post-exit via `resources/read uri=file:///proc/42/sandbox`.

---

## Recipe 2: long-running tool with live output

**What:** spawn a wget that takes seconds, subscribe to its stdout
on the same conn, drain notifications until the job exits.

```jsonc
// On conn A: launch the job.
{"jsonrpc":"2.0","id":1,"method":"shell.exec_background",
 "params":{"cmd":"/wget.elf","args":["http://10.0.2.2:8080/big"]}}
// -> {"jsonrpc":"2.0","id":1,"result":{"job_id":0,"pid":42}}

// On conn B (persistent): subscribe.
{"jsonrpc":"2.0","id":2,"method":"shell.job.subscribe",
 "params":{"job_id":0}}
// -> {"jsonrpc":"2.0","id":2,"result":{"job_id":0,"subscribed":true,
//                                       "stdout_total":0,"stderr_total":0}}

// Conn B then receives a stream of:
// {"jsonrpc":"2.0","method":"notifications/job.output",
//  "params":{"job_id":0,"stream":"stdout","data":"<bytes>"}}
// ...
// {"jsonrpc":"2.0","method":"notifications/job.exit",
//  "params":{"job_id":0,"exit_code":0}}
```

Each `notifications/job.output` carries up to 512 bytes of newly-
appended pipe data. The ring buffer caps at 4 KiB per stream; if a
subscriber falls behind and bytes get overwritten, the daemon
silently drops the lost notifications (poll via `shell.job.read`
with the previous `stdout_next` offset to backfill).

To stop watching mid-stream:

```jsonc
{"jsonrpc":"2.0","id":99,"method":"shell.job.unsubscribe",
 "params":{"job_id":0}}
```

Closing the connection also unsubscribes — see session 76's conn-
cleanup pass.

---

## Recipe 3: persistent memory across calls

**What:** stash a value, retrieve it, watch for changes, clean up.

```jsonc
// Write — namespace is auto-created on first put.
{"jsonrpc":"2.0","id":1,"method":"kv.put",
 "params":{"namespace":"agent.demo","key":"last_seen","value":"42"}}
// -> {"jsonrpc":"2.0","id":1,"result":{"ok":true}}

// Read back.
{"jsonrpc":"2.0","id":2,"method":"kv.get",
 "params":{"namespace":"agent.demo","key":"last_seen"}}
// -> {"jsonrpc":"2.0","id":2,"result":{"found":true,"value":"42"}}

// List everything under a prefix.
{"jsonrpc":"2.0","id":3,"method":"kv.list",
 "params":{"namespace":"agent.demo","prefix":"last_"}}
// -> {"jsonrpc":"2.0","id":3,"result":{"keys":["last_seen"]}}

// Watch for prefix changes — fires on every matching kv.put / kv.del.
{"jsonrpc":"2.0","id":4,"method":"kv.watch",
 "params":{"namespace":"agent.demo","prefix":""}}
// -> {"jsonrpc":"2.0","id":4,"result":{"watch_id":1}}

// Now the watcher conn receives:
// {"jsonrpc":"2.0","method":"notifications/kv/changed",
//  "params":{"namespace":"agent.demo","key":"last_seen","op":"put"}}
// for every subsequent kv.put / kv.del under "agent.demo".

// Clean up.
{"jsonrpc":"2.0","id":5,"method":"kv.unwatch",
 "params":{"watch_id":1}}
// -> {"jsonrpc":"2.0","id":5,"result":{"ok":true}}

{"jsonrpc":"2.0","id":6,"method":"kv.del",
 "params":{"namespace":"agent.demo","key":"last_seen"}}
// -> {"jsonrpc":"2.0","id":6,"result":{"ok":true,"existed":true}}
```

Namespace `agent.demo` is a convention; the validator accepts
`[A-Za-z0-9_-]` (1..32 chars) — dots aren't valid in namespaces,
so this would actually fail. Use `agent_demo` or `agentdemo`.

Path-traversal attempts (`{"key":"../escape"}`, `{"namespace":".."}`)
are rejected in libuser before any FS call.

---

## Recipe 4: scheduled heartbeat

**What:** every 60 seconds, ping a status URL; receive a
notification each time the heartbeat fires.

```jsonc
// Create — fires every 60 s, runs forever (max_runs:0).
{"jsonrpc":"2.0","id":1,"method":"cron.create",
 "params":{"kind":"recurring","interval_sec":60,
           "cmd":"/wget.elf","args":["http://10.0.2.2:8080/heartbeat"],
           "policy":"netclient",
           "limits":{"max_cpu_ms":5000,"max_wall_ms":10000}}}
// -> {"jsonrpc":"2.0","id":1,"result":{"entry_id":1,"fire_at":1715692800}}

// Subscribe — get pinged each fire.
{"jsonrpc":"2.0","id":2,"method":"cron.subscribe",
 "params":{"entry_id":1}}
// -> {"jsonrpc":"2.0","id":2,"result":{"ok":true}}

// Watcher conn receives, every 60 s:
// {"jsonrpc":"2.0","method":"notifications/cron.fired",
//  "params":{"entry_id":1,"job_id":7,"fire_at":1715692800,"run_count":1}}
// Then job_id=7's stdout/stderr can be inspected via shell.job.read.
```

Inspect the entry's history at any time:

```jsonc
{"jsonrpc":"2.0","id":3,"method":"cron.get","params":{"entry_id":1}}
// -> {"jsonrpc":"2.0","id":3,"result":{"entry":{
//      "id":1,"kind":"recurring","state":"scheduled",
//      "fire_at":1715692860,"interval_sec":60,"max_runs":0,
//      "run_count":1,"last_run_at":1715692800,"last_exit_code":0,
//      "last_job_id":7,"concurrent":false,
//      "cmd":"/wget.elf","args":["http://..."],"policy":"netclient",
//      "limits":{"max_cpu_ms":5000,"max_wall_ms":10000}}}}
```

Stop the heartbeat:

```jsonc
// Cancel — keeps the record around for the agent to query.
{"jsonrpc":"2.0","id":4,"method":"cron.cancel","params":{"entry_id":1}}
// -> {"jsonrpc":"2.0","id":4,"result":{"ok":true,"was_scheduled":true}}

// Delete — wipes the slot and removes /var/cron/1.json.
{"jsonrpc":"2.0","id":5,"method":"cron.delete","params":{"entry_id":1}}
// -> {"jsonrpc":"2.0","id":5,"result":{"ok":true,"removed":1}}
```

The entry's last spawn (in `last_job_id`) keeps running even after
`cron.delete`. Use `shell.job.cancel` to stop it.

The on-disk JSON file at `/var/cron/<id>.json` survives reboot.
Boot recovery rebuilds the in-memory table; past-due entries fire
on the next tick (single fire, no missed-interval replay).

---

## Common pitfalls

- **MCP tools/call escape:** if you wrap a call in `tools/call`,
  the inner result is JSON-string-encoded inside `content[0].text`.
  A simple substring search for `"job_id":` won't find anything
  because the actual bytes are `\"job_id\"`. Use the direct
  dispatch shape (no `tools/call` envelope) for programmatic
  parsing; reserve `tools/call` for the MCP path.
- **Persistent conn ≠ blocking:** `agent_open_persistent` (libagent)
  flips `FD_FL_NONBLOCK`, so `sys_read` returns -1 with no data
  pending. Use `agent_recv_line_timed` with a budget, OR sleep
  between drain attempts.
- **Subscribe-then-write race:** there's a brief window between
  `kv.watch` returning to the client and the watch being committed
  in agentd's table. A `kv.put` issued in the same millisecond may
  fire before the watcher is registered. Insert a ~50 ms pause if
  the test is otherwise hot.
- **Cron entries persist across reboots:** test scripts should
  `cron.delete` what they create — otherwise the table fills up
  over many runs. Same applies to KV entries (`kv.del`).
- **Sandboxed printf:** `sandbox_policy_minimal` includes `SYS_WRITE`
  but NOT `SYS_WRITE_FD`. libc's `printf` goes through `SYS_WRITE_FD`,
  so a minimal-policy child's `printf` calls get silently denied. Add
  the bit explicitly if you want the child to talk on the wire.

For a complete enumeration of tools / notifications / resources /
syscalls / procfs files / capability flags, see `docs/agent-api.md`.
