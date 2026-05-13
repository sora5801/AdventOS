# Session 65 — MCP server support in agentd

**Goal:** turn `agentd` into a Model Context Protocol server. After this session, a Claude Desktop / Claude Code / Cline / any MCP-aware client can talk to AdventOS directly over the existing TCP loopback socket — no custom adapter, no shell shim, no JSON-RPC bridge in front. The agent does `initialize`, walks `tools/list`, fires `tools/call`, and gets MCP-shaped responses back.

MCP is a thin layer on top of JSON-RPC 2.0. Session 64 already shipped the wire format (newline-framed `{"jsonrpc":"2.0",...}` on `127.0.0.1:7000`) and an eight-method tool surface. This session is the wrapper that names those methods as MCP tools.

Shipped:

- `initialize` handler — returns `protocolVersion`, `capabilities.tools`, `serverInfo`.
- `tools/list` handler — serves the cached `tools` array from `/etc/agent.tools.json`.
- `tools/call` handler — looks up the tool by name, runs the existing emit_fn into an inner buffer, wraps it in MCP's `content[]` + `isError` envelope.
- Refactored existing handlers into reusable `emit_fn(json_w*, params)` functions that both the legacy direct path and `tools/call` share.
- Boot-time manifest loader that reads `/etc/agent.tools.json`, parses it, and re-emits the `tools` array into a BSS-resident buffer for instant serving.
- `[t48]` selftest that walks `initialize` → `tools/list` → `tools/call "time"` on a single connection.

Selftest result: **141 PASS, 0 FAIL** under `-smp 1` (up from 134 in session 64).

---

## Why a separate session

Session 64 chose JSON-RPC 2.0 as the wire format precisely because it's MCP-compatible. The session 64 deep dive was explicit:

> Implementing all of [MCP] would inflate this session twice over without changing what agents actually need from a CLI-only OS. So I built the underlying wire format faithfully ... and packaged tool discovery as a static file at /etc/agent.tools.json rather than a runtime handshake. If we want full MCP later, the dispatcher adds two methods (initialize, tools/list) and serves the manifest synthetically.

This session is exactly that "later." Three handlers plus a manifest loader, no kernel changes, no new syscalls, no new dependencies.

## What MCP actually is

The wire is JSON-RPC 2.0. Each request is `{"jsonrpc":"2.0","id":N,"method":"...","params":{...}}` and each response is `{"jsonrpc":"2.0","id":N,"result":{...}}` or `{"jsonrpc":"2.0","id":N,"error":{...}}`. Framing in the canonical spec is Content-Length headers (LSP-style); over a TCP socket plain newline-delimited works and is what we use.

MCP's contribution is **what the methods are** and **what their shapes look like**. The three this session implements:

| Method        | Purpose                                                    |
| ------------- | ---------------------------------------------------------- |
| `initialize`  | Handshake. Server announces its protocol version, capabilities, and identity. |
| `tools/list`  | Returns the list of tools the server exposes — each tool has a name, description, and JSON Schema for its input. |
| `tools/call`  | Invokes a specific tool. Returns a `content[]` array of one or more content blocks (text / image / resource ref) and an `isError` boolean. |

Sessions, prompts, resources, sampling, logging, completion, notifications — all out of scope for this session and explicitly so. Most agents work fine with tools alone.

## Architecture

```
                    ┌──────────────────────────────────┐
                    │ Top-level dispatch                │
                    │                                   │
   { method:"X" } ──▶│   "initialize"  → handle_init     │── envelope
                    │   "tools/list"  → handle_t_list   │── envelope
                    │   "tools/call"  → handle_t_call   │── envelope
                    │   default       → direct method   │── envelope
                    │                    (legacy path)  │
                    └─────────┬────────────────────────┘
                              │
                              ▼
                    ┌──────────────────────────────────┐
                    │ Tool table — emit_fn per tool     │
                    │   { "time",        emit_time }    │
                    │   { "getuid",      emit_getuid }  │
                    │   ... 8 tools total               │
                    └─────────┬────────────────────────┘
                              │
                              ▼
                    ┌──────────────────────────────────┐
                    │ emit_fn(json_w*, params)          │
                    │   writes the result body          │
                    │   {"key":"value", ...}            │
                    └──────────────────────────────────┘
```

Same emit_fn is reachable from two paths:

- **Direct** (`{"method":"time"}`): `resp_begin_result` writes the envelope opening, emit_fn writes `{"epoch":N}`, `resp_end` closes the envelope. Same shape as session 64.
- **tools/call** (`{"method":"tools/call","params":{"name":"time","arguments":{}}}`): same emit_fn, but it writes into `g_c.inner` (a separate scratch buffer). The dispatcher then JSON-escapes that buffer's contents as a string inside `{"content":[{"type":"text","text":"<escaped>"}],"isError":false}`.

The double-encoding (result JSON inside an escaped string field) is MCP's text-content shape. Future revisions could pick `application/json` content type if a content-type negotiation lands, but text-content with serialized JSON inside is the universally-supported answer right now.

## The refactor

Session 64's handlers all looked like this:

```c
static int handle_time(const struct json_v *id, const struct json_v *params) {
    struct json_w w;
    resp_begin_result(&w, id);
    json_obj_begin(&w);
      json_key(&w, "epoch"); json_uint(&w, sys_time());
    json_obj_end(&w);
    resp_end(&w);
    return json_w_ok(&w) ? 0 : -32603;
}
```

Each `handle_X` owned its own envelope. That coupled the handler to the JSON-RPC shape and made wrapping in MCP impossible without duplication.

Session 65 split them:

```c
static int emit_time(struct json_w *w, const struct json_v *params) {
    (void)params;
    json_obj_begin(w);
      json_key(w, "epoch"); json_uint(w, sys_time());
    json_obj_end(w);
    return 0;
}
```

The function writes a self-contained `{...}` result body. The caller owns the envelope. Direct path:

```c
struct json_w w;
resp_begin_result(&w, id);
int rc = m->emit(&w, params);
resp_end(&w);
```

tools/call path:

```c
struct json_w iw;
json_w_init(&iw, g_c.inner, sizeof(g_c.inner));
int rc = m->emit(&iw, args_v);
json_w_finish(&iw);
/* Now g_c.inner holds the result as JSON; embed as escaped text. */
emit_tools_call_envelope(id, g_c.inner, json_w_len(&iw), is_error);
```

Same eight emit_fn entries on the table, served by both paths. The legacy direct-method test from session 64 (`[t47]`) keeps passing without modification — agentd is back-compat with session 64 clients.

## The manifest

`/etc/agent.tools.json` was already MCP-shaped after session 64, but with one quirk: it had `outputExample` fields under each tool. Real MCP `tools/list` returns only `name`, `description`, and `inputSchema`. Session 65 strips the example fields. The file's top-level `schemaVersion` was bumped from `"1.0"` to `"2024-11-05"` to match the MCP protocol version we report from `initialize`.

The loader runs once at boot:

```c
static void load_tools_manifest(void) {
    /* read /etc/agent.tools.json into BSS buffer */
    /* parse with libjson */
    /* json_obj_get the "tools" array */
    /* re-emit into g_tools_arr via emit_value() */
}
```

That last step is a parser → writer round-trip: every node gets re-serialised. The output is byte-for-byte regenerated, so we know it's well-formed and there's no leftover whitespace, trailing comma, or stray comment to confuse a strict client.

`tools/list` becomes a one-liner:

```c
json_obj_begin(&w);
  json_key(&w, "tools");
  json_raw(&w, g_tools_arr, g_tools_arr_len);
json_obj_end(&w);
```

`json_raw` was already available in libjson — it splices a pre-rendered fragment in while still updating the writer's comma/depth bookkeeping.

## emit_value — the recursive walker

For the manifest loader (and potentially future uses) I added a recursive `emit_value` that walks a parsed `json_v` tree and re-emits each node through a `json_w`:

```c
static void emit_value(struct json_w *w, const struct json_v *v) {
    switch (v->type) {
        case JSON_NULL: json_null(w); break;
        case JSON_BOOL: json_bool(w, (int)v->num); break;
        case JSON_NUM:  json_int(w,  (int)v->num); break;
        case JSON_STR:  json_str_n(w, v->str, v->str_len); break;
        case JSON_ARR:
            json_arr_begin(w);
            for (struct json_v *c = v->child; c; c = c->next) emit_value(w, c);
            json_arr_end(w);
            break;
        case JSON_OBJ:
            json_obj_begin(w);
            for (struct json_v *k = v->child; k && k->next; k = k->next->next) {
                if (k->type == JSON_STR) {
                    json_key(w, k->str);
                    emit_value(w, k->next);
                }
            }
            json_obj_end(w);
            break;
    }
}
```

The depth is bounded by the manifest's structure (~4 levels), well under libjson's `JSON_W_MAX_DEPTH = 32`. If somebody ever ships a manifest with deeply nested schemas, the writer would just truncate on overflow rather than overrun the stack.

This is the same primitive an MCP server would need for any pass-through behavior — e.g. proxying tools/call results from a subordinate process. Good investment, ~40 LOC.

## Wire example

A real conversation, end to end:

```
client → server:
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{
  "protocolVersion":"2024-11-05",
  "capabilities":{},
  "clientInfo":{"name":"selftest","version":"1.0"}}}

server → client:
{"jsonrpc":"2.0","id":1,"result":{
  "protocolVersion":"2024-11-05",
  "capabilities":{"tools":{}},
  "serverInfo":{"name":"adventos-agentd","version":"1.0.0"}}}

client → server:
{"jsonrpc":"2.0","id":2,"method":"tools/list"}

server → client:
{"jsonrpc":"2.0","id":2,"result":{"tools":[
  {"name":"time","description":"...","inputSchema":{...}},
  {"name":"getuid","description":"...","inputSchema":{...}},
  ... 6 more ...
]}}

client → server:
{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{
  "name":"time","arguments":{}}}

server → client:
{"jsonrpc":"2.0","id":3,"result":{
  "content":[{"type":"text","text":"{\"epoch\":1778704034}"}],
  "isError":false}}
```

A Python client looks roughly like:

```python
import socket, json

s = socket.create_connection(("127.0.0.1", 7000))
def call(method, params=None, id_=[0]):
    id_[0] += 1
    req = {"jsonrpc":"2.0","id":id_[0],"method":method}
    if params: req["params"] = params
    s.sendall(json.dumps(req).encode() + b"\n")
    resp = b""
    while not resp.endswith(b"\n"):
        resp += s.recv(4096)
    return json.loads(resp.decode())

init = call("initialize", {"protocolVersion":"2024-11-05",
                            "capabilities":{},
                            "clientInfo":{"name":"py","version":"1.0"}})
print(init["result"]["serverInfo"])

tools = call("tools/list")
for t in tools["result"]["tools"]:
    print(t["name"], "—", t["description"])

now = call("tools/call", {"name":"time","arguments":{}})
text = now["result"]["content"][0]["text"]
print("epoch =", json.loads(text)["epoch"])
```

That's the contract. From the AI agent's perspective, AdventOS now looks like any other MCP server.

## Selftest

`[t48] agentd: MCP protocol — initialize + tools/list + tools/call`:

1. `initialize` — confirm response carries `protocolVersion:"2024-11-05"`, `serverInfo.name:"adventos-agentd"`, and `capabilities.tools:{}`.
2. `tools/list` — count tool entries (expect 8 to match the manifest), confirm `time` and `shell.exec` appear.
3. `tools/call` invoking `time` — confirm `content[]` array, `type:"text"`, `isError:false`, and that the inner text carries an escaped `\"epoch\":` (the JSON-escaped form, since the result is wrapped as a string).

All ten assertions PASS. Combined with the existing `[t47]` direct-path tests (eight more PASSes), the agentd selftest surface is 18 assertions.

Two implementation gotchas surfaced during this session and are worth flagging:

1. **The kernel's `tcp_send_seg_seq` rejects payloads bigger than the per-call stack buffer of 1500 bytes — there's no automatic segmentation.** A `tools/list` response is ~1.6 KiB and would fail the single `sys_write`. agentd's `send_response` now chunks every reply into ≤1 KiB calls; the receiving end's sock RX buffer (4 KiB) reassembles them. A future kernel fix would do this transparently.

2. **The selftest's previous byte-by-byte read pattern hit edge cases when chunks arrived spaced out.** The READ_LINE macro now reads up to 256 bytes per `sys_read` until it finds a newline. Same wire result, fewer syscalls, and no scheduler-timing footgun.

## Error handling

MCP's `tools/call` convention is that **invocation errors** should surface as `isError:true` content rather than JSON-RPC `error` envelopes. The reasoning: an agent that issued `tools/call` always wants to see the result — even a failed result — so it can show the model what went wrong. A JSON-RPC error code at the transport level would be hidden behind library plumbing.

agentd implements that pattern:

```c
if (rc != 0 || !json_w_ok(&iw)) {
    const char *msg = err_msg_for(rc ? rc : -32603);
    return emit_tools_call_envelope(id, msg, (int)strlen(msg), 1);  /* isError */
}
return emit_tools_call_envelope(id, g_c.inner, json_w_len(&iw), 0);
```

Wrong tool name? `{"content":[{"type":"text","text":"no such tool"}],"isError":true}`. Bad args to `dns_resolve`? `{"content":[...,"text":"Invalid params"...],"isError":true}`. The transport-level JSON-RPC error codes (`-32601`, `-32602`) are reserved for *protocol* errors: bad JSON, missing `method`, etc.

## Touched files

- `user/agentd.c` — full refactor. `handle_X` → `emit_X` (returns int, writes body only); new `handle_initialize`, `handle_tools_list`, `handle_tools_call`; new `load_tools_manifest`; new `emit_value` recursive walker; chunked `send_response` so multi-KiB responses don't trip the kernel's per-segment cap. Buffer sizes match session 64 except for `MANIFEST_MAX` (stack-local boot buffer) and `g_tools_arr` (4 KiB cache of the re-emitted manifest).
- `fs/etc/agent.tools.json` — `schemaVersion` bumped to `"2024-11-05"`, `mcp:true` flag added under `server`, `outputExample` fields removed from each tool.
- `user/sh.c` — new `[t48]` selftest (10 EXPECT assertions, 3 round-trips on one connection).

No kernel changes. No new syscalls. No new dependencies beyond libjson (session 64).

## What's still out of scope

- **Prompts, resources, sampling, logging, completion, notifications.** MCP defines all of these as separate top-level method namespaces (`prompts/list`, `prompts/get`, `resources/list`, etc.). agentd reports `capabilities:{"tools":{}}` so a client knows we're tools-only and won't offer the others.
- **MCP notifications.** Many clients send `notifications/initialized` after the handshake. agentd silently ignores requests with no `id` (JSON-RPC's signal for notifications). Future revisions could log them; the wire is fine as-is.
- **Per-tool error structured content.** When `dns_resolve` fails we return a plain `"Invalid params"` text. A more polished server would return `{"content":[{"type":"text","text":"DNS lookup timeout for example.invalid"}],"isError":true}` with a specific failure reason. Same shape, richer text.
- **Multiple content blocks per tools/call.** Today every result is one text block. MCP supports interleaving text + image + resource refs in a single response. For tools that return both stdout and binary attachments (we don't have any) this would matter.
- **AF_UNIX transport.** Still TCP loopback. An AF_UNIX server would close the only remaining "what if a non-loopback listener got smuggled in" attack surface. Nothing about MCP precludes it — most MCP servers run over stdio.
- **stdio transport.** The reference MCP transport for local servers is parent-spawned-with-stdio. Implementing that would let Claude Desktop launch agentd directly without configuring a network endpoint. Trivial wrapper, just hasn't been written.
- **InputSchema validation.** Agentd's emit_fn dispatchers each do their own arg checking. A proper schema validator (`required` enforcement, type checks) could share `/etc/agent.tools.json` as ground truth and reject malformed `arguments` uniformly.

## What this unlocks

A Claude agent — running on a developer's laptop or in a CI pipeline — can now treat AdventOS as a peer MCP server. The mental model is the same as connecting to a filesystem MCP server or a GitHub MCP server: a session of `tools/call` invocations against a named server with a known capability set.

For the AdventOS roadmap, this also means future "expose feature X to agents" sessions become trivial — they add an emit_fn and a manifest entry, and the new tool shows up automatically in `tools/list`. The framing problem is solved.
