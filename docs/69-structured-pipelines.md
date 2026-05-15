# Session 69 — Structured pipelines (JSONL convention)

**Goal.** Give AI agents a way to write shell pipelines that don't break silently when a column moves or a separator changes. Text-format coupling — `ls /etc | grep ssh | head -1` working until ls grows a column — is the #1 reason agent-emitted shell scripts fail without anyone noticing. This session promotes JSONL to a first-class pipeline mode, teaches the common tools to round-trip records, and adds three structured operators (`pluck`, `where`, `count`) that cover the 80% of pipelines agents actually emit.

The cost is small: bare `|` keeps working exactly as today, the kernel pipe layer is untouched, and the operator that activates structured mode (`|>`) is one new token in the shell. Humans typing at the prompt see no behaviour change unless they opt in.

---

## Why JSONL, not protobuf / framed JSON / XML

- **Line-oriented is greppable** — `pluck name` is a fancier `awk '{print $1}'`; the result is still text you can pipe through `cat` or `grep`. A length-prefixed framed format (varint or netstring) would break `head -1` because head can't find the boundaries without parsing.
- **One self-contained record per line** is what `head | tail` already understands. The kernel pipe layer stays a byte stream; the JSONL convention is purely a userspace producer/consumer agreement. Zero kernel changes for this session.
- **Streams unconditionally.** A JSON array of records (`[{...}, {...}, ...]`) doesn't stream — readers have to wait for the closing `]` to start parsing. JSONL flushes each record as a complete unit, so a slow producer feeding a fast consumer behaves the same as a fast-fast pair: the consumer just sees fewer lines per second.
- **Existing libjson works as-is.** No new parser, no new schema language, no new dependency.

The trade-off: no inline arrays of records (each record must be a flat-ish object, nested objects fine, no `{"items": [{...}, {...}]}`). For the agent use case that's not a real restriction — the line *is* the array element. The few schemas where it would have been natural (e.g. ps's per-task `limits` block) get a single nested object rather than an array.

---

## Activation: the `|>` shell operator

The shell (`user/sh.c`) recognises `|>` as a separate operator from `|`. Both split a command line into pipeline stages exactly the same way — only the metadata differs:

| Operator | Stages | Activates JSONL mode | Notes |
|---|---|---|---|
| `\|`  | a \| b \| c | no  | identical to today; full backwards compat |
| `\|>` | a \|> b \|> c | yes | every stage gets `--advjson` injected into argv |

Any `|>` in a pipeline upgrades the whole pipeline to JSONL mode. Mixing `|` and `|>` in one command line still produces a JSONL pipeline — pick whichever reads better; the model is "is there at least one `|>`?" not "which sub-segment".

**Why argv injection instead of an env variable.** AdventOS doesn't propagate environment variables across `sys_exec` today, so the obvious `ADV_JSON=1` approach would have required a kernel change. Argv injection (the shell adds `--advjson` as an extra argv element for every stage) gets the same semantics with zero new plumbing — and aligns with the existing `--json` flag pattern that ls/ps/date already had. The user-visible spelling stays the same: write `ls /etc |>` and the shell handles the rest.

**What `--advjson` means inside a tool.** It's a single flag the shell-spawned children look at:
- **Producers** (ls, ps, date): emit JSONL records on stdout instead of the human-readable table.
- **Consumers** (pluck, where, count, sort, head): parse stdin as JSONL records. head doesn't even need to: counting newlines works the same on text and JSONL.
- **Tools that haven't been ported**: ignore the flag silently (most argv parsers treat unknown flags as files; the harmless side effect is they look for a file named `--advjson` and fail to open it — which mid-pipeline is moot because stdin is the data source). `tr` is the one tool that would actively corrupt JSONL, and it'll error if `--advjson` is set.

**Pretty-printing for humans.** Deliberately omitted. Agents are the primary audience; raw JSONL straight to the TTY is what they want when they ask `shell.run` to capture stdout. Humans typing `ls / |>` see the raw JSONL on their terminal — that's fine, they were opting in. A future session could add a JSONL→table pretty-printer as a shell-injected final stage, but the verification gate doesn't need it.

---

## Per-command schema reference

### Producers

#### `ls [path] [--advjson]`

One record per directory entry:

```json
{"name":"passwd","type":"FILE","size":191,"perm":420,"uid":0,"gid":0}
```

| Field | Type   | Meaning                          |
|-------|--------|----------------------------------|
| name  | string | basename, up to 16 chars         |
| type  | string | "FILE" or "DIR"                  |
| size  | integer| bytes (0 for directories)        |
| perm  | integer| AdventFS mode bits (0..0777)     |
| uid   | integer| owner uid                        |
| gid   | integer| owner gid                        |

`perm`, `uid`, `gid` are omitted if the FS doesn't have metadata for an entry.

#### `ps [--advjson]`

One record per task:

```json
{"pid":3,"name":"init","state":"running"}
```

| Field | Type   | Meaning                                |
|-------|--------|----------------------------------------|
| pid   | integer| task id                                |
| name  | string | task command name (TASK_NAME_MAX=16)   |
| state | string | "running" / "ready" / "blocked" / ...  |

(Spec mentioned `ppid`, `uid`, `gid`, `rss_pages`, `cpu_ticks`, `limits` — those fields are slated for a follow-up session that exposes them through procfs first. This session ships the three fields ps already had.)

#### `date [--advjson]`

A single record:

```json
{"iso":"2026-05-15T14:23:01Z","unix":1778859781,"year":2026,"month":5,"day":15,"hour":14,"min":23,"sec":1,"tz":"UTC"}
```

| Field         | Type   | Meaning                          |
|---------------|--------|----------------------------------|
| iso           | string | ISO 8601, always UTC, always Z   |
| unix          | integer| seconds since epoch              |
| year/month/day| integer| Gregorian calendar (month 1..12) |
| hour/min/sec  | integer| 24h time, all UTC                |
| tz            | string | always "UTC" in AdventOS         |

### Consumer + emitter (filter/transform)

#### `pluck <field>... [--advjson]`

Project named fields. **Single-field form emits bare scalars on a line** (so the output is text-mode for downstream tools that aren't JSONL-aware):

```
$ ls /etc |> pluck name
inittab
passwd
ssh_keys
resolv.conf
ssl
agent.tools.json
```

**Multi-field form emits smaller records** (so further `|>` stages keep working):

```
$ ls /etc |> pluck name size
{"name":"inittab","size":828}
{"name":"passwd","size":191}
...
```

Missing fields are simply omitted from the multi-field record (no null filler). In single-field mode, missing keys print an empty line.

#### `where <field><op><value> [--advjson]`

Filter. Predicate is one argv triple:

| Op  | Semantics                                                       |
|-----|-----------------------------------------------------------------|
| =   | equality. Numeric if the field is JSON_NUM, else byte-for-byte. |
| !=  | inverse of =                                                    |
| <  > <= >= | numeric only; field must be JSON_NUM                     |
| ~   | substring match (no regex); field must be JSON_STR              |

Records whose field is missing fail the predicate. Compose AND by chaining: `|> where a=1 |> where b=2`. OR isn't supported (intentional — explicit compound logic stays out of shell; agents wanting richer filters call `shell.run` and post-process client-side).

#### `count [--advjson]`

Consume every record, emit a single record `{"count": N}\n` at EOF.

Pure newline counter — doesn't parse the input. Trailing partial line (no `\n`) still counts. Output is itself a JSONL record so further chains work: `|> count |> pluck count` returns a bare integer per "category".

### Dual-mode (text + JSONL)

#### `sort [-k <field>] [files...] [--advjson]`

With `--advjson` and `-k <field>`, sorts by the named field from each line's JSON record. Numeric values compare numerically; strings compare lexically; missing keys sort first (so the "no data" bucket is consistent). Without `--advjson`, behaves exactly as before.

#### `head -N`, `tail -N`

Pass through unchanged. Newlines bound records the same way they bound text lines; head/tail just count newlines and pass bytes through. No code changes needed in either tool.

### Text-only

#### `tr`

Refuses to run when `--advjson` is in argv (will corrupt JSON record boundaries with naive char substitution). Documented; the shell doesn't try to be clever — if you write `... |> tr ...` it's a deliberate mistake and you get an error.

---

## agentd integration: `shell.run`

A new MCP tool on agentd:

```json
{
  "name": "shell.run",
  "description": "Run a shell pipeline; capture JSONL stdout; return as JSON array.",
  "inputSchema": {"type":"object","properties":{"cmd":{"type":"string"}},"required":["cmd"]}
}
```

Implementation: fork+exec `/sh.elf -c "<cmd>"`, capture up to 64 KiB of stdout, parse each newline-terminated chunk as JSON, splice valid objects/arrays into a top-level result array. Returns:

```jsonc
// "result" field of the JSON-RPC response:
[{"name":"etc","type":"DIR","size":0,"perm":493,"uid":0,"gid":0}]
```

The result is **the array itself**, not an object wrapping one. Matches the verification gate's example and is the most common shape agent consumers want — most use cases iterate the records directly with no wrapping object in the way. Lines that fail to parse as JSON objects/arrays are dropped silently (banner / debug output from sub-commands is invisible to the caller). Cap-related truncation isn't signalled in the result shape — if you exceed 64 KiB you're using shell.run wrong; switch to `shell.exec_background` and stream via `shell.job.read`.

---

## Worked examples

```sh
# Find the size of /etc/passwd (the verification gate)
$ ls /etc |> where name=passwd |> pluck size
191

# How many running processes
$ ps |> where state=running |> count
{"count": 6}

# Smallest .elf in / by size
$ ls / |> where type=FILE |> sort -k size |> head -1
{"name":"hello.elf","type":"FILE","size":4744,"perm":493,"uid":0,"gid":0}

# Pipe a JSON record set into agentctl (via shell.run)
$ agentctl call shell.run '{"cmd":"ls /etc |> count"}'
[{"count": 4}]

# Backwards compat — bare | unchanged
$ ls /etc | grep passwd | head -1
passwd
```

---

## What's NOT in scope

- **No full `jq`.** `pluck` + `where` + `count` is the entire transform vocabulary. Computed projections (`{name, size_mib: size/1048576}`) aren't expressible in the shell — agents wanting that should pipe through `shell.run` and post-process client-side.
- **No binary cat.** `cat --advjson` would split text files by newline into `{line: "..."}` records, but binary files (cryptotest output, etc.) stay as raw bytes. Deferred to a follow-up.
- **No type system, no schema validation.** Records are JSON; consumers tolerate missing fields. The per-tool schemas in this doc are descriptive, not enforced.
- **No kernel pipe changes.** Pipes stay byte streams. The JSONL boundary is a userspace convention, end-to-end.
- **No TTY pretty-printer.** Raw JSONL goes to the terminal when a `|>` pipeline ends at a TTY. Agents prefer that; humans can `... |> cat` or pipe through `head`. A pretty-printer is straightforward to add later — inject a `jsonl-fmt` stage at the end of any pipeline whose final stage's stdout is a TTY — but it's not on the regression-critical path.
- **No env-var propagation.** `ADV_JSON=1 ls` doesn't work because AdventOS doesn't propagate env vars through `sys_exec`. The `|>` operator is the activation mechanism; tools never check env. A future session could add real env vars (would also fix `PATH`, `HOME`, etc. propagating) but it's orthogonal.

What a follow-up session would need to add to reach a fully featured agent shell:
- env-var propagation through exec (kernel change to `struct task` + libuser getenv)
- ps record enrichment (ppid, uid, rss_pages, cpu_ticks, limits) via procfs reads
- cat/wc/grep/uniq JSONL modes (mechanical — same pattern as ls/ps)
- a JSONL→table pretty-printer for human-readable terminal output
- richer `where` (multiple predicates, OR via `||`)
- a `--schema` flag for type-checked pipelines

---

## Verification gate

After this session lands, these exact invocations pass under `-smp 1` and `-smp 2`:

```sh
$ ls /etc |> where name=passwd |> pluck size
191
$ ps |> count
{"count": 9}
$ agentctl call shell.run '{"cmd":"ls /etc |> count"}'
[{"count": 4}]
```

And `selftest`'s `pipeline-selftest` block reports green for all six cases:

1. `ls / |> pluck name` → bare names, no JSON braces
2. `ls / |> where type=FILE |> count` → `{count: N}` with N reasonable
3. `ps |> sort -k pid |> head -1` → pid=0 (kmain)
4. `date |> pluck unix` → bare integer ≥ 10⁹
5. `agentctl call shell.run 'ls / |> where name=etc'` → single-element array containing `{name:"etc"}`
6. `ls / | grep etc | head -1` (bare `|`) → unchanged text output
