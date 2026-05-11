/*
 * Session 60 — SNTP (RFC 4330) client + tiny test responder.
 *
 * SNTP is the simplified protocol everybody actually uses in place of
 * "full" NTP: a single UDP request/response round-trip on port 123,
 * 48 bytes each way. No clock-discipline algorithm, no peer
 * negotiation, no stratum-aware filtering. Suitable for sub-second-
 * but-not-microsecond accuracy, which is what a hobby OS wants.
 *
 * Wire format (RFC 4330 §4):
 *
 *     0       1       2       3
 *     +-------+-------+-------+-------+
 *     |LI|VN|M|Stratum|Poll  |Precis. |   1 byte each
 *     +-------+-------+-------+-------+
 *     |   Root Delay (signed 16.16)   |
 *     +-------+-------+-------+-------+
 *     |   Root Dispersion (16.16)     |
 *     +-------+-------+-------+-------+
 *     |   Reference Identifier        |
 *     +-------+-------+-------+-------+
 *     |   Reference Timestamp (64-bit)|
 *     +-+ - - - - - - - - - - - - - + +
 *     |   Originate Timestamp (64-bit)|
 *     +-+ - - - - - - - - - - - - - + +
 *     |   Receive Timestamp (64-bit)  |
 *     +-+ - - - - - - - - - - - - - + +
 *     |   Transmit Timestamp (64-bit) |   ← what we care about
 *     +-+ - - - - - - - - - - - - - + +
 *
 * Each 64-bit timestamp is high-32-bits = seconds since 1900-01-01,
 * low-32-bits = fractional seconds (2^-32 sec). We need to subtract
 * 2208988800 (seconds 1900 → 1970) to get Unix epoch.
 *
 * The test responder lives in this same file behind an enable flag,
 * lets t43 do a loopback-only test of the round-trip without needing
 * a real NTP server reachable through QEMU's SLIRP.
 */
#ifndef ADVENTOS_NTP_H
#define ADVENTOS_NTP_H

#include "../include/types.h"
#include "ip.h"

/* Synchronous SNTP query. Sends one packet to (server, 123), waits up
 * to ~2 s for a reply, parses the Transmit Timestamp, returns Unix
 * epoch in *out_epoch. Returns 0 on success, -1 on timeout or
 * malformed reply. */
int ntp_sync(const struct ip_addr *server, uint32_t *out_epoch);

/* Register an in-kernel UDP-123 responder that replies to incoming
 * SNTP queries with `epoch_to_return` as the Transmit Timestamp. Used
 * only by the t43 selftest to drive ntp_sync against a known answer
 * without needing the public internet.
 *
 * `enable = 1` registers the listener; `enable = 0` clears it.
 *
 * Callers that just want NTP-client behavior should NOT touch this. */
void ntp_test_responder(int enable, uint32_t epoch_to_return);

#endif
