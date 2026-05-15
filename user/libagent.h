/*
 * libagent — minimal in-guest JSON-RPC client for agentd.
 *
 * Used by the session-78 selftests to talk to agentd over loopback
 * without depending on the host-side TCP path (which is flaky under
 * the Windows/MSYS2/QEMU stdio backend). Same code pattern as
 * agentctl, refactored into a static library so each selftest
 * compiles small.
 *
 * Threading: single-conn-at-a-time per process. A test that needs
 * to receive notifications while issuing requests opens a second
 * persistent conn via `agent_open_persistent`.
 *
 * Buffers: the caller owns the request + response buffers. The
 * library never allocates. 4 KiB is the recommended response cap
 * (matches the kernel TCP segment + agentd's framing); 1 KiB is
 * usually enough for requests.
 */
#ifndef ADVENTOS_LIBAGENT_H
#define ADVENTOS_LIBAGENT_H

#include "libuser.h"

#define AGENTD_PORT  7000
#define LIBAGENT_DEFAULT_RESP_CAP 4096

/* One-shot request: open conn, write `json_request` + '\n', read one
 * response line. Closes the conn before returning. Returns bytes
 * placed in `resp_buf` (NUL-terminated; the trailing '\n' from the
 * wire is stripped) or -1 on connect/IO failure.
 *
 * resp_cap should be >= 256 for any meaningful response. */
int agent_call(const char *json_request, char *resp_buf, int resp_cap);

/* Convenience for the tools/call envelope. Builds
 *   {"jsonrpc":"2.0","id":1,"method":"tools/call",
 *    "params":{"name":"<tool>","arguments":<args_json>}}
 * from `tool` and `args_json` (already a valid JSON object string
 * including the surrounding braces, e.g. "{\"cmd\":\"ls.elf\"}").
 * Empty args: pass "{}".
 *
 * Note: the response wraps the tool's inner result in an MCP
 * content/text block where the inner JSON has its quotes
 * backslash-escaped. agent_find_field / agent_get_* won't see
 * those escaped fields because they look for an unescaped `"key":`.
 * If you want to inspect a field by name, use agent_method_call
 * instead (direct dispatch — same tool, unescaped response). */
int agent_tool_call(const char *tool, const char *args_json,
                    char *resp_buf, int resp_cap);

/* Direct-method JSON-RPC call (no tools/call wrapping). Builds
 *   {"jsonrpc":"2.0","id":1,"method":"<method>","params":<args_json>}
 * Response is the raw {"result": {...}} envelope so agent_get_int /
 * agent_get_bool can find fields with their unescaped names.
 * Use this for selftests that need to assert on specific fields.
 * Empty args: pass "{}". */
int agent_method_call(const char *method, const char *args_json,
                      char *resp_buf, int resp_cap);

/* Open a long-lived conn for receiving notifications. Caller does
 * sys_read on the returned fd to receive each '\n'-terminated frame.
 * The conn is marked FD_FL_NONBLOCK so reads return -1 with no data
 * pending. Returns the socket fd, or -1 on failure. Caller MUST
 * sys_close before exit so agentd's per-conn subscriber tables get
 * the cleanup signal (session 76). */
int agent_open_persistent(void);

/* Send a pre-built JSON-RPC request on an existing fd (followed by
 * '\n'). Returns 0 on success, -1 on partial write / closed fd. */
int agent_send_line(int fd, const char *line);

/* Read one '\n'-terminated line from `fd` into `buf` (cap bytes).
 * Returns bytes excluding the newline, 0 on EOF, -1 on error or
 * would-block. Use sys_fd_nb to make the fd non-blocking first. */
int agent_recv_line(int fd, char *buf, int cap);

/* Like agent_recv_line but loops with sys_sleep_ms(10) until either
 * a complete line arrives, EOF, or `timeout_ms` elapses. Returns
 * bytes read on success, 0 on EOF, -1 on timeout / error. Use this
 * on a non-blocking persistent conn when you've just sent a request
 * and want the response. */
int agent_recv_line_timed(int fd, char *buf, int cap, int timeout_ms);

/* Substring-search a JSON response for a literal "\"<key>\":" and
 * return a pointer to the byte immediately after. NULL if the key
 * isn't found. Selftest-only convenience — does NO real parsing,
 * doesn't escape match-context, can't tell apart a top-level "ok"
 * from a nested "ok". Sufficient for assertions like "is the value
 * 'true' near the key 'ok'?". */
const char *agent_find_field(const char *resp, const char *key);

/* Extract a plain integer that follows "<key>": in resp. Returns
 * fallback if the key isn't found or the byte after isn't a digit
 * (possibly preceded by `-`). Stops at the first non-digit. */
int agent_get_int(const char *resp, const char *key, int fallback);

/* Match a JSON boolean value: returns 1 if the bytes after
 * "<key>": start with `true`, 0 if `false`, fallback otherwise. */
int agent_get_bool(const char *resp, const char *key, int fallback);

/* Does `resp` contain the literal substring `needle`? */
int agent_contains(const char *resp, const char *needle);

#endif
