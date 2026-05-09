#ifndef ADVENTOS_SOCK_H
#define ADVENTOS_SOCK_H

#include "../include/types.h"

/*
 * Sockets layer. Wraps the single-TCB TCP impl with a small per-fd
 * data structure and a RX ring buffer, exposing a blocking
 * accept/read/write/close API suitable for direct use from the
 * user-side syscall handlers.
 *
 * One listener and one connection coexist at a time — that's all the
 * underlying TCP can support, since `kernel/tcp.c` keeps a single
 * global TCB. Each `sock_create` returns a slot index; the same
 * index is what gets stored in `task_fd.sock_idx` for a FD_SOCK fd.
 */

#define SOCK_MAX     8
#define SOCK_RX_BUF  2048

enum {
    SOCK_FREE = 0,
    SOCK_NEW,           /* socket() returned, not yet bound */
    SOCK_LISTEN,        /* listening for connections        */
    SOCK_CONNECTED,     /* established, read/write OK       */
};

struct sock {
    int               state;
    uint16_t          port;

    /* For LISTEN sockets: the connection-socket index that's been
     * created but not yet accepted. -1 if no pending connection. */
    int               pending_conn;

    /* For CONNECTED sockets: SPSC ring buffer. Single producer is
     * the TCP rx callback (IRQ context); single consumer is the
     * user task in sock_read. head/tail are 32-bit aligned, atomic
     * on x86 — no lock needed. */
    volatile uint32_t rx_head;
    volatile uint32_t rx_tail;
    volatile int      peer_closed;
    uint8_t           rx_buf[SOCK_RX_BUF];
};

void sock_init(void);

int  sock_create(void);                                /* idx or -1 */
int  sock_bind  (int idx, uint16_t port);
int  sock_listen(int idx);
int  sock_accept(int idx);                             /* blocks; returns new idx */
int  sock_read  (int idx, void *buf, int n);           /* blocks; 0 = EOF         */
int  sock_write (int idx, const void *buf, int n);
int  sock_close (int idx);

#endif
