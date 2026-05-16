# Session 69 — Structured pipelines (JSONL convention)

**Goal.** Give AI agents a way to write shell pipelines that don't break silently when a column moves or a separator changes. Text-format coupling — `ls /etc | grep ssh | head -1` working until ls grows a column — is the #1 reason agent-emitted shell scripts fail without anyone noticing. Session 81 introduced the `|>` operator, the `pluck`/`where`/`count` tools, and JSONL emitters for ls/ps/date. Session 82 widened that surface to cat/wc/grep/uniq/tee/tr — every coreutils-class tool an agent reaches for on autopilot is now either structured-mode-aware or refuses to corrupt the stream.

The cost is small: bare `|` keeps working exactly as today, the kernel pipe layer is untouched, and the operator that activates structured mode (`|>`) is one new token in the shell. Humans typing at the prompt see no behaviour change unless they opt in.

## Agent learning surface

AdventOS is positioned as a training ground for AI agents — an environment where agents learn to write shell scripts that actually work. That framing shapes every design choice in this surface:

- **Composition is pure.** Every `|>` stage transforms records to records (or aggregates records to one summary record). No hidden state, no flag dependencies between stages. An agent that reasons about a pipeline can predict its output from the schemas alone.
- **Schemas are stable.** Field names and types don't change between invocations. Adding new fields is backwards-compatible (consumers ignore unknown fields). Removing or renaming a field is a breaking change — sessions document it explicitly.
- **Failures are diagnosable, never silent.** Missing field → predicate fails (consistent with `where`). Type mismatch → match fails. Malformed line → skipped silently for filter tools (`grep`/`where`) so a single bad record doesn't poison the stream, but the record count drops, which the agent can observe. `tr --advjson` refuses outright rather than silently corrupting JSON — *loud failure beats silent corruption.*
- **Field-by-name beats column-by-position.** `pluck size` and `where size>1000` reference the schema; an agent that knows `ls` outputs `size` can write working code without parsing column widths or word offsets. When the schema grows, old pipelines still work.

When extending this surface, optimise for these properties. The Non-goals section at the bottom names features that an agent might wish existed but that would compromise predictability — they stay out for a reason.

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

#### `cat [file...] [--advjson]`     *(session 82)*

One record per `\n`-terminated input line:

```json
{"file":"/etc/passwd","line":"root:x:0:0:System Administrator:/:/sh.elf"}
```

| Field | Type   | Meaning                                                 |
|-------|--------|---------------------------------------------------------|
| file  | string | source filename. Present only when cat got >1 file. For `<stdin>` mode it's the literal string "<stdin>". |
| line  | string | line content with the trailing `\n` stripped            |

**Binary files are refused** — the first 4 KiB is scanned for `\0` or any control byte outside `\t \r \n`, and the file is rejected with `cat: --advjson rejects binary file FOO` on stderr (exit code 1). This is the loud-failure principle: a JSONL pipeline corrupted by binary bytes would silently mis-parse downstream; the loud rejection lets the agent diagnose and route around. Files without a trailing `\n` still emit a final record for the partial last line.

#### `wc [-l|-w|-c] [file...] [--advjson]`     *(session 82)*

One record per file, plus a `TOTAL` aggregate for multi-file invocations:

```json
{"file":"/etc/passwd","lines":5,"words":5,"bytes":191}
{"file":"/etc/inittab","lines":7,"words":10,"bytes":219}
{"file":"TOTAL","lines":12,"words":15,"bytes":410}
```

| Field | Type   | Meaning                                                           |
|-------|--------|-------------------------------------------------------------------|
| file  | string | source path; "TOTAL" for the aggregate. "<stdin>" for stdin input |
| lines | integer| count of `\n` characters (matches text-mode wc)                   |
| words | integer| maximal runs of non-whitespace                                    |
| bytes | integer| byte count                                                        |

**Field omission is deliberate:** `wc -l` populates only `lines`; `words` and `bytes` are simply *absent* from the record (not zero). An agent doing `wc -l |> pluck lines` never accidentally gets `0` from a record that didn't compute lines — missing-field is diagnosable, silent-zero is not.

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

#### `grep [-v] [-f <field>] <pattern> [--advjson]`     *(session 82)*

Filter records by substring match against a field's stringified value.

- **With `-f <field>`** the pattern matches the named field. `JSON_STR` matches directly; `JSON_NUM` is stringified first (`191` → `"191"`); `JSON_BOOL` becomes `"true"`/`"false"`; `JSON_NULL`/`JSON_OBJ`/`JSON_ARR` never match (no usable string). Missing field never matches (same as `where`).
- **Without `-f`** the pattern matches the first `JSON_STR`-typed field in the record (iteration order = encoding order). Most schemas in this doc lead with a string field (`ls` → `name`, `ps` → `name`, `cat` → either `file` or `line`), so this does what an agent intuitively expects.
- **No string field anywhere?** Falls back to matching the raw line bytes (text-mode grep on the JSON encoding). Documented as a fallback; agents shouldn't rely on it.

Matching records pass through **unchanged** — record passthrough, not re-serialised. Key order, nested objects, all preserved. `-v` inverts.

```sh
$ ps |> grep -f name init |> pluck pid
3
```

Engine is the same naïve substring scan as text-mode grep (no regex character classes / backreferences). Same library, same behaviour.

#### `uniq [-f <field>] [--advjson]`     *(session 82)*

Collapse adjacent duplicate records.

- **With `-f <field>`** dedup by that field's value (stringified for numbers, same as grep's rule). Missing field is the empty-string sentinel — all missing-field records collapse together, mirroring `grep -f`'s missing-field semantics.
- **Without `-f`** dedup by byte-for-byte line content. Two records with the same fields in different key-order are *not* equal (we don't re-canonicalise — too expensive for a stream filter). For deterministic order, run `|> sort -k <field>` first.

Adjacent-only, same as POSIX uniq. For global dedup:
```
ls / |> sort -k name |> uniq -f name
```

### Dual-mode (text + JSONL)

#### `sort [-k <field>] [files...] [--advjson]`

With `--advjson` and `-k <field>`, sorts by the named field from each line's JSON record. Numeric values compare numerically; strings compare lexically; missing keys sort first (so the "no data" bucket is consistent). Without `--advjson`, behaves exactly as before.

#### `head -N`, `tail -N`

Pass through unchanged. Newlines bound records the same way they bound text lines; head/tail just count newlines and pass bytes through. No code changes needed in either tool.

### Transparent

#### `tee [file...] [--advjson]`     *(session 82)*

Byte-transparent fan-out: every input byte is written to stdout AND to every named file. The `--advjson` flag is recognised but has no behavioural effect — tee neither parses nor emits records, it just copies bytes. The flag is silently consumed (not treated as a filename) so the shell's argv injection never surprises an agent with `tee: cannot open --advjson`.

Tee'd files become valid JSONL files when the input stream is JSONL:
```
$ ls / |> tee /saved.jsonl |> count
{"count":56}
$ cat /saved.jsonl |> count
{"count":56}
```

### Explicit-error

#### `tr <set1> <set2> [--advjson]`     *(session 82, was text-only in session 81)*

Refuses to run if `--advjson` is in argv. Prints:

```
tr: refusing to corrupt JSONL stream — use pluck/where/grep instead
```

to stderr, exits with code `2`. The downstream stage of a `|> tr a A |>` pipeline sees zero records (tr never wrote any), and `count` reports `{"count":0}` — that's both the correct semantics (no records survived) and a clear signal to the agent that something was wrong.

This is the *only* tool in the surface that breaks the "ignore the flag silently" convention. The reason is the loud-failure principle from the agent-learning-surface section: tr's character substitution would silently corrupt JSON quoting, brace boundaries, and key/value separators, and downstream tools would either drop the malformed records or — much worse — mis-parse them as something they're not. Erroring out is the only honest option.

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

Both session 81 (original 3 commands) and session 82 (the wider surface) pass under `-smp 1` and `-smp 2`:

```sh
# Session 81
$ ls /etc |> where name=passwd |> pluck size
191
$ ps |> count
{"count": 9}
$ agentctl call shell.run '{"cmd":"ls /etc |> count"}'
[{"count": 4}]

# Session 82
$ cat /etc/passwd |> count
{"count":5}
$ wc /etc/passwd |> pluck bytes
191
$ ps |> grep -f name init |> count
{"count":1}
$ ls / |> sort -k name |> uniq -f name |> count |> pluck count
56
$ ls / |> tr a A
tr: refusing to corrupt JSONL stream — use pluck/where/grep instead
[exit 2]
$ ls / | grep elf | head -1
agentctl.elf
```

And `selftest`'s `pipeline-selftest` block reports green for all **twelve** cases:

**Session 81:**
1. `ls / |> pluck name` → bare names, no JSON braces
2. `ls / |> where type=FILE |> count` → `{count: N}` with N reasonable
3. `ps |> sort -k pid |> head -1` → pid=0 (kmain)
4. `date |> pluck unix` → bare integer ≥ 10⁹
5. `agentctl call shell.run 'ls / |> where name=etc'` → single-element array containing `{name:"etc"}`
6. `ls / | grep etc | head -1` (bare `|`) → unchanged text output

**Session 82:**
7. `cat /etc/passwd |> count` matches `wc -l /etc/passwd`
8. `wc /etc/passwd |> pluck bytes` matches `ls /etc |> where name=passwd |> pluck size`
9. `ps |> grep -f name init |> count` → `{count: 1}` (exactly one task named init)
10. `ls / |> sort -k name |> uniq -f name |> count` matches `ls / |> count` (uniq no-op on already-unique list)
11. `ls / |> tee /saved.jsonl |> count` matches `cat /saved.jsonl |> count` (tee transparent + file is valid JSONL)
12. `ls / |> tr a A |> count` → `{count: 0}` (tr refused, downstream sees empty stream)
