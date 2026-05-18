/*
 * virtio-9p — 9P2000.L client over the virtio legacy transport.
 *
 * Wire format. Every message is:
 *
 *   [size:4 LE][type:1][tag:2 LE][payload]
 *
 * size INCLUDES the 7-byte header. Strings are `[len:2][bytes:len]`.
 * The qid (file identity, 13 bytes) is `[type:1][version:4][path:8]`.
 *
 * Message types we implement (subset of 9P2000.L):
 *
 *   Tversion 100  Rversion 101   -- handshake; tag = NOTAG = 0xFFFF
 *   Tattach  104  Rattach  105   -- attach a fid to the FS root
 *   Twalk    110  Rwalk    111   -- resolve a path, alloc a new fid
 *   Tlopen    12  Rlopen    13   -- open existing fid (Linux flags)
 *   Tread    116  Rread    117
 *   Tclunk   120  Rclunk   121   -- close fid
 *   Tgetattr  24  Rgetattr  25   -- stat-ish
 *   Treaddir  40  Rreaddir  41
 *   Rlerror    7                 -- Linux errno reply (any T-msg)
 *
 * Protocol strategy: completely synchronous. We hold a mutex around
 * each round-trip (build T-msg in scratch, submit on the single
 * virtqueue, wait for R-msg, parse). No request pipelining; tag = 0
 * for every non-version message.
 *
 * VFS strategy: keep root_fid alive forever (attached at mount).
 * For every VFS op, allocate a temp_fid from a small pool, WALK from
 * root to the target path, do the op, CLUNK temp_fid. Slower than a
 * real 9P client (which would cache fids) but matches the VFS shape
 * which has no close() op.
 */
#include "virtio_9p.h"
#include "virtio.h"
#include "pci.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "string.h"
#include "spinlock.h"
#include "vfs.h"
#include "task.h"           /* for FD_9P */
#include "../include/io.h"

/* ---- 9P protocol constants -------------------------------------- */

#define P9_NOFID         0xFFFFFFFFu
#define P9_NOTAG         0xFFFFu

#define P9_Rlerror       7
#define P9_Tlopen        12
#define P9_Rlopen        13
#define P9_Tlcreate      14
#define P9_Rlcreate      15
#define P9_Tgetattr      24
#define P9_Rgetattr      25
#define P9_Treaddir      40
#define P9_Rreaddir      41
#define P9_Tmkdir        72
#define P9_Rmkdir        73
#define P9_Trenameat     74
#define P9_Rrenameat     75
#define P9_Tunlinkat     76
#define P9_Runlinkat     77
#define P9_Tversion      100
#define P9_Rversion      101
#define P9_Tattach       104
#define P9_Rattach       105
#define P9_Twalk         110
#define P9_Rwalk         111
#define P9_Tread         116
#define P9_Rread         117
#define P9_Twrite        118
#define P9_Rwrite        119
#define P9_Tclunk        120
#define P9_Rclunk        121

/* Tunlinkat flags. Same values Linux uses. */
#define P9_AT_REMOVEDIR  0x200

/* Tgetattr request masks (Linux). We always ask for everything. */
#define P9_GETATTR_ALL   0x00003fffu

/* qid.type bits */
#define P9_QTDIR         0x80
#define P9_QTFILE        0x00

/* Tlopen flags (Linux open(2) flags). */
#define P9_O_RDONLY      0
#define P9_O_WRONLY      1
#define P9_O_RDWR        2

/* ---- Driver state ----------------------------------------------- */

#define P9_MSIZE          8192
#define P9_FID_POOL       16
#define P9_MAX_TAG_LEN    64

struct v9p {
    int               in_use;
    struct pci_device pci;
    uint16_t          io;
    struct virtqueue  vq;             /* single request queue */
    spinlock_t        lock;           /* serializes every 9P round-trip */
    uint8_t          *tbuf;           /* P9_MSIZE bytes, host READS */
    uint8_t          *rbuf;           /* P9_MSIZE bytes, host WRITES */
    char              mount_tag[P9_MAX_TAG_LEN + 1];
    uint32_t          msize;          /* negotiated */
    uint32_t          root_fid;       /* fid attached to FS root */
    /* fid pool (excluding root_fid). bit i clear = fid (i + 2) is
     * free; bit i set = fid is allocated. */
    uint16_t          fid_used;
};

static struct v9p g_v9p;

/* ---- Tiny serializer for 9P payloads ---------------------------- */

struct p9w {
    uint8_t *buf;
    int      o;
    int      cap;
};

static void w_u8 (struct p9w *w, uint8_t  v) { w->buf[w->o++] = v; }
static void w_u16(struct p9w *w, uint16_t v) {
    w->buf[w->o++] = (uint8_t)(v);
    w->buf[w->o++] = (uint8_t)(v >> 8);
}
static void w_u32(struct p9w *w, uint32_t v) {
    w_u16(w, (uint16_t)v); w_u16(w, (uint16_t)(v >> 16));
}
static void w_u64(struct p9w *w, uint64_t v) {
    w_u32(w, (uint32_t)v); w_u32(w, (uint32_t)(v >> 32));
}
static void w_str(struct p9w *w, const char *s) {
    int n = 0; while (s[n]) n++;
    w_u16(w, (uint16_t)n);
    for (int i = 0; i < n; i++) w_u8(w, (uint8_t)s[i]);
}

struct p9r {
    const uint8_t *buf;
    int            o;
    int            cap;
};

static uint8_t  r_u8 (struct p9r *r) { return r->buf[r->o++]; }
static uint16_t r_u16(struct p9r *r) {
    uint16_t v = (uint16_t)r->buf[r->o] | ((uint16_t)r->buf[r->o + 1] << 8);
    r->o += 2; return v;
}
static uint32_t r_u32(struct p9r *r) {
    uint32_t v = r_u16(r); v |= (uint32_t)r_u16(r) << 16; return v;
}
static uint64_t r_u64(struct p9r *r) {
    uint64_t v = r_u32(r); v |= (uint64_t)r_u32(r) << 32; return v;
}
static int r_str(struct p9r *r, char *out, int cap) {
    uint16_t n = r_u16(r);
    int copy = n < (cap - 1) ? n : (cap - 1);
    for (int i = 0; i < copy; i++) out[i] = (char)r->buf[r->o + i];
    out[copy] = 0;
    r->o += n;
    return n;
}
static void r_skip(struct p9r *r, int n) { r->o += n; }

/* ---- Round-trip ------------------------------------------------- */

/* Common header writer; returns the *position* where the size goes
 * (we patch it at the end). */
static int hdr_begin(struct p9w *w, uint8_t type, uint16_t tag) {
    int size_at = w->o;
    w_u32(w, 0);             /* size placeholder */
    w_u8 (w, type);
    w_u16(w, tag);
    return size_at;
}

static void hdr_finalize(struct p9w *w, int size_at) {
    uint32_t total = (uint32_t)w->o;
    w->buf[size_at + 0] = (uint8_t)(total      );
    w->buf[size_at + 1] = (uint8_t)(total >> 8 );
    w->buf[size_at + 2] = (uint8_t)(total >> 16);
    w->buf[size_at + 3] = (uint8_t)(total >> 24);
}

/* Submit one descriptor chain (tbuf as device-readable T-msg of
 * `t_len` bytes, rbuf as device-writable R-msg buffer of msize bytes)
 * and block until the host writes a complete R-msg. Returns the
 * number of bytes the host wrote into rbuf, or -1 on failure. */
static int p9_round_trip(struct v9p *v, int t_len) {
    if (v->vq.n_free < 2) return -1;
    int d0 = virtio_alloc_desc(&v->vq);
    int d1 = virtio_alloc_desc(&v->vq);
    if (d0 < 0 || d1 < 0) {
        if (d0 >= 0) virtio_free_desc_chain(&v->vq, (uint16_t)d0);
        if (d1 >= 0) virtio_free_desc_chain(&v->vq, (uint16_t)d1);
        return -1;
    }

    /* desc[0] = T-msg, device READS, chain to desc[1] */
    v->vq.desc[d0].addr_lo = (uint32_t)(uintptr_t)v->tbuf;
    v->vq.desc[d0].addr_hi = 0;
    v->vq.desc[d0].len     = (uint32_t)t_len;
    v->vq.desc[d0].flags   = VIRTQ_DESC_F_NEXT;
    v->vq.desc[d0].next    = (uint16_t)d1;

    /* desc[1] = R-msg, device WRITES, end of chain */
    v->vq.desc[d1].addr_lo = (uint32_t)(uintptr_t)v->rbuf;
    v->vq.desc[d1].addr_hi = 0;
    v->vq.desc[d1].len     = v->msize;
    v->vq.desc[d1].flags   = VIRTQ_DESC_F_WRITE;
    v->vq.desc[d1].next    = 0;

    /* Capture the used.ring slot before submit so we can read the
     * actual R-msg length out of it (virtio_wait_used will free the
     * chain and bump last_used, but the slot bytes stay valid). */
    uint16_t slot = (uint16_t)(v->vq.last_used % v->vq.qsize);
    virtio_submit(v->io, &v->vq, (uint16_t)d0);

    int rc = virtio_wait_used(v->io, &v->vq, 5000);
    if (rc != 0) {
        kprintf("9p: round-trip timeout (t_len=%d)\n", t_len);
        return -1;
    }
    int r_len = (int)v->vq.used->ring[slot].len;
    return r_len;
}

/* If the R-msg is Rlerror, log the errno and return -1 so callers
 * can bail. Otherwise return 0. */
static int p9_check_error(struct v9p *v, int r_len, const char *op) {
    if (r_len < 7) {
        kprintf("9p: short R-msg from %s (got %d, want >=7)\n", op, r_len);
        return -1;
    }
    uint8_t type = v->rbuf[4];
    if (type == P9_Rlerror) {
        struct p9r r = { v->rbuf + 7, 0, r_len - 7 };
        uint32_t ecode = r_u32(&r);
        kprintf("9p: %s returned errno=%u\n", op, (unsigned)ecode);
        return -1;
    }
    return 0;
}

/* ---- fid pool --------------------------------------------------- */

static int p9_alloc_fid(struct v9p *v, uint32_t *out) {
    for (int i = 0; i < P9_FID_POOL; i++) {
        if (!(v->fid_used & (1u << i))) {
            v->fid_used |= (uint16_t)(1u << i);
            *out = (uint32_t)(2 + i);
            return 0;
        }
    }
    return -1;
}

static void p9_free_fid(struct v9p *v, uint32_t fid) {
    if (fid < 2 || fid >= (uint32_t)(2 + P9_FID_POOL)) return;
    v->fid_used &= (uint16_t)~(1u << (fid - 2));
}

/* ---- High-level 9P operations ----------------------------------- */

static int p9_version(struct v9p *v) {
    struct p9w w = { v->tbuf, 0, P9_MSIZE };
    int s = hdr_begin(&w, P9_Tversion, P9_NOTAG);
    w_u32(&w, P9_MSIZE);
    w_str(&w, "9P2000.L");
    hdr_finalize(&w, s);

    int rl = p9_round_trip(v, w.o);
    if (rl < 0 || p9_check_error(v, rl, "Tversion") != 0) return -1;

    struct p9r r = { v->rbuf, 7, rl };
    uint32_t srv_msize = r_u32(&r);
    char srv_ver[16];
    r_str(&r, srv_ver, sizeof(srv_ver));
    v->msize = srv_msize < P9_MSIZE ? srv_msize : P9_MSIZE;
    kprintf("9p: negotiated msize=%u version=%s\n",
            (unsigned)v->msize, srv_ver);
    return 0;
}

static int p9_attach(struct v9p *v, uint32_t fid) {
    struct p9w w = { v->tbuf, 0, P9_MSIZE };
    int s = hdr_begin(&w, P9_Tattach, 0);
    w_u32(&w, fid);
    w_u32(&w, P9_NOFID);          /* afid (no auth) */
    w_str(&w, "root");            /* uname */
    w_str(&w, "");                /* aname */
    w_u32(&w, 0);                 /* n_uname (uid hint) */
    hdr_finalize(&w, s);

    int rl = p9_round_trip(v, w.o);
    if (rl < 0 || p9_check_error(v, rl, "Tattach") != 0) return -1;
    /* Rattach payload = qid (13 bytes); we don't bother parsing. */
    return 0;
}

static int p9_clunk(struct v9p *v, uint32_t fid) {
    struct p9w w = { v->tbuf, 0, P9_MSIZE };
    int s = hdr_begin(&w, P9_Tclunk, 0);
    w_u32(&w, fid);
    hdr_finalize(&w, s);

    int rl = p9_round_trip(v, w.o);
    /* We don't care about errors here — caller should drop the fid
     * either way. */
    (void)rl;
    return 0;
}

/* Walk from `start_fid` to `path` (slash-separated), allocating
 * `new_fid` for the destination. Returns 0 on success (caller must
 * eventually clunk new_fid), -1 on failure (new_fid is freed for us).
 *
 * Empty path = clone of start_fid (walk with 0 names — useful to
 * make a working copy of a fid before opening it).
 *
 * The is_dir out-param reports whether the destination is a directory
 * (read from the final qid's type bit). May be NULL.
 */
static int p9_walk(struct v9p *v, uint32_t start_fid, const char *path,
                   uint32_t new_fid, int *is_dir)
{
    struct p9w w = { v->tbuf, 0, P9_MSIZE };
    int s = hdr_begin(&w, P9_Twalk, 0);
    w_u32(&w, start_fid);
    w_u32(&w, new_fid);

    /* Count + emit path components. */
    int nwname_at = w.o;
    w_u16(&w, 0);                 /* nwname placeholder */
    int nwname = 0;

    int i = 0;
    while (path[i]) {
        if (path[i] == '/') { i++; continue; }
        int start = i;
        while (path[i] && path[i] != '/') i++;
        int len = i - start;
        if (len == 0) continue;
        if (nwname >= 16) return -1;     /* 9P caps walk depth */
        /* Inline write: [u16 len][bytes] */
        w_u16(&w, (uint16_t)len);
        for (int j = 0; j < len; j++) w_u8(&w, (uint8_t)path[start + j]);
        nwname++;
    }
    /* Patch nwname. */
    w.buf[nwname_at + 0] = (uint8_t)nwname;
    w.buf[nwname_at + 1] = (uint8_t)(nwname >> 8);

    hdr_finalize(&w, s);

    int rl = p9_round_trip(v, w.o);
    if (rl < 0 || p9_check_error(v, rl, "Twalk") != 0) return -1;

    struct p9r r = { v->rbuf, 7, rl };
    uint16_t nwqid = r_u16(&r);
    /* If the walk fell short of all requested names, the destination
     * doesn't exist — return failure. */
    if (nwqid != nwname) {
        return -1;
    }
    if (is_dir) {
        if (nwqid == 0) {
            /* Cloned from start_fid — caller should infer type from
             * the source; we don't know here. Pessimistically default
             * to "not a directory". */
            *is_dir = 0;
        } else {
            /* Read the final qid.type byte. Each qid is 13 bytes;
             * qid.type is the first byte. */
            int last_qid = r.o + (nwqid - 1) * 13;
            *is_dir = (r.buf[last_qid] & P9_QTDIR) ? 1 : 0;
        }
    }
    return 0;
}

static int p9_lopen(struct v9p *v, uint32_t fid, uint32_t flags) {
    struct p9w w = { v->tbuf, 0, P9_MSIZE };
    int s = hdr_begin(&w, P9_Tlopen, 0);
    w_u32(&w, fid);
    w_u32(&w, flags);
    hdr_finalize(&w, s);

    int rl = p9_round_trip(v, w.o);
    if (rl < 0 || p9_check_error(v, rl, "Tlopen") != 0) return -1;
    return 0;
}

/* Read up to `count` bytes from `fid` at `offset` into `out`.
 * Returns bytes actually read (0 on EOF, -1 on error). */
static int p9_read(struct v9p *v, uint32_t fid, uint64_t offset,
                   uint32_t count, void *out)
{
    /* Cap count so the response fits in msize (account for header +
     * Rread's count field). */
    uint32_t max = v->msize - 11;
    if (count > max) count = max;

    struct p9w w = { v->tbuf, 0, P9_MSIZE };
    int s = hdr_begin(&w, P9_Tread, 0);
    w_u32(&w, fid);
    w_u64(&w, offset);
    w_u32(&w, count);
    hdr_finalize(&w, s);

    int rl = p9_round_trip(v, w.o);
    if (rl < 0 || p9_check_error(v, rl, "Tread") != 0) return -1;

    struct p9r r = { v->rbuf, 7, rl };
    uint32_t n = r_u32(&r);
    if (n > count) n = count;
    if ((int)(r.o + n) > rl) return -1;
    for (uint32_t i = 0; i < n; i++) ((uint8_t *)out)[i] = r.buf[r.o + i];
    return (int)n;
}

/* Get attributes for `fid`. Fills out_size + out_is_dir. */
static int p9_getattr(struct v9p *v, uint32_t fid,
                      uint64_t *out_size, int *out_is_dir)
{
    struct p9w w = { v->tbuf, 0, P9_MSIZE };
    int s = hdr_begin(&w, P9_Tgetattr, 0);
    w_u32(&w, fid);
    w_u64(&w, P9_GETATTR_ALL);
    hdr_finalize(&w, s);

    int rl = p9_round_trip(v, w.o);
    if (rl < 0 || p9_check_error(v, rl, "Tgetattr") != 0) return -1;

    struct p9r r = { v->rbuf, 7, rl };
    r_u64(&r);                              /* valid mask */
    uint8_t qid_type = r_u8(&r);
    r_skip(&r, 12);                         /* rest of qid */
    r_u32(&r);                              /* mode */
    r_u32(&r);                              /* uid */
    r_u32(&r);                              /* gid */
    r_u64(&r);                              /* nlink */
    r_u64(&r);                              /* rdev */
    uint64_t size = r_u64(&r);
    if (out_size)   *out_size   = size;
    if (out_is_dir) *out_is_dir = (qid_type & P9_QTDIR) ? 1 : 0;
    return 0;
}

/* Read one directory entry at `offset`. Fills name (caller buffer),
 * sets *out_next_offset to the offset to use for the NEXT entry.
 * Returns 1 if an entry was returned, 0 on end-of-directory,
 * -1 on error. */
static int p9_readdir_one(struct v9p *v, uint32_t fid, uint64_t offset,
                          char *name, int name_cap,
                          uint64_t *out_next_offset)
{
    struct p9w w = { v->tbuf, 0, P9_MSIZE };
    int s = hdr_begin(&w, P9_Treaddir, 0);
    w_u32(&w, fid);
    w_u64(&w, offset);
    w_u32(&w, v->msize - 11);
    hdr_finalize(&w, s);

    int rl = p9_round_trip(v, w.o);
    if (rl < 0 || p9_check_error(v, rl, "Treaddir") != 0) return -1;

    struct p9r r = { v->rbuf, 7, rl };
    uint32_t n = r_u32(&r);
    if (n == 0) return 0;                   /* end of directory */

    /* Each entry: [qid:13][offset:8][type:1][name:string]. */
    r_skip(&r, 13);                         /* qid */
    uint64_t next_off = r_u64(&r);
    r_skip(&r, 1);                          /* dir entry type */
    r_str(&r, name, name_cap);
    if (out_next_offset) *out_next_offset = next_off;
    return 1;
}

/* ---- write-side operations ------------------------------------- */

/* Tlcreate — create a new file under directory `dfid`. After success
 * the fid REUSED for `dfid` now refers to the newly-created file,
 * opened with `flags` (Linux open(2) flags). `mode` is the POSIX
 * permission bits (e.g. 0644). */
static int p9_lcreate(struct v9p *v, uint32_t dfid, const char *name,
                      uint32_t flags, uint32_t mode)
{
    struct p9w w = { v->tbuf, 0, P9_MSIZE };
    int s = hdr_begin(&w, P9_Tlcreate, 0);
    w_u32(&w, dfid);
    w_str(&w, name);
    w_u32(&w, flags);
    w_u32(&w, mode);
    w_u32(&w, 0);                 /* gid (we don't track POSIX groups) */
    hdr_finalize(&w, s);

    int rl = p9_round_trip(v, w.o);
    if (rl < 0 || p9_check_error(v, rl, "Tlcreate") != 0) return -1;
    /* Response is [qid:13][iounit:4]; we don't bother parsing. */
    return 0;
}

/* Twrite — write `count` bytes from `data` to `fid` at `offset`.
 * Returns bytes actually written (<= count) or -1 on error. */
static int p9_write(struct v9p *v, uint32_t fid, uint64_t offset,
                    uint32_t count, const void *data)
{
    /* Cap count so the request fits in msize (header + Twrite fields
     * = 7 + 4 + 8 + 4 = 23 bytes of overhead). */
    uint32_t max = v->msize - 23;
    if (count > max) count = max;

    struct p9w w = { v->tbuf, 0, P9_MSIZE };
    int s = hdr_begin(&w, P9_Twrite, 0);
    w_u32(&w, fid);
    w_u64(&w, offset);
    w_u32(&w, count);
    /* Append the data bytes directly. */
    for (uint32_t i = 0; i < count; i++) w_u8(&w, ((const uint8_t *)data)[i]);
    hdr_finalize(&w, s);

    int rl = p9_round_trip(v, w.o);
    if (rl < 0 || p9_check_error(v, rl, "Twrite") != 0) return -1;

    struct p9r r = { v->rbuf, 7, rl };
    uint32_t n = r_u32(&r);
    if (n > count) n = count;
    return (int)n;
}

/* Tmkdir — create a directory `name` under `dfid` with `mode` perms. */
static int p9_mkdir(struct v9p *v, uint32_t dfid, const char *name,
                    uint32_t mode)
{
    struct p9w w = { v->tbuf, 0, P9_MSIZE };
    int s = hdr_begin(&w, P9_Tmkdir, 0);
    w_u32(&w, dfid);
    w_str(&w, name);
    w_u32(&w, mode);
    w_u32(&w, 0);                 /* gid */
    hdr_finalize(&w, s);

    int rl = p9_round_trip(v, w.o);
    if (rl < 0 || p9_check_error(v, rl, "Tmkdir") != 0) return -1;
    return 0;
}

/* Tunlinkat — remove `name` from `dfid`. If `is_dir`, pass the
 * AT_REMOVEDIR flag so the host treats it as rmdir; otherwise this
 * is an unlink. */
static int p9_unlinkat(struct v9p *v, uint32_t dfid, const char *name,
                       int is_dir)
{
    struct p9w w = { v->tbuf, 0, P9_MSIZE };
    int s = hdr_begin(&w, P9_Tunlinkat, 0);
    w_u32(&w, dfid);
    w_str(&w, name);
    w_u32(&w, is_dir ? P9_AT_REMOVEDIR : 0);
    hdr_finalize(&w, s);

    int rl = p9_round_trip(v, w.o);
    if (rl < 0 || p9_check_error(v, rl, "Tunlinkat") != 0) return -1;
    return 0;
}

/* Trenameat — atomic rename of `oldname` under `olddir_fid` to
 * `newname` under `newdir_fid`. Both fids must already refer to
 * directories. The fids themselves are NOT consumed by this
 * operation. */
static int p9_renameat(struct v9p *v,
                       uint32_t olddir_fid, const char *oldname,
                       uint32_t newdir_fid, const char *newname)
{
    struct p9w w = { v->tbuf, 0, P9_MSIZE };
    int s = hdr_begin(&w, P9_Trenameat, 0);
    w_u32(&w, olddir_fid);
    w_str(&w, oldname);
    w_u32(&w, newdir_fid);
    w_str(&w, newname);
    hdr_finalize(&w, s);

    int rl = p9_round_trip(v, w.o);
    if (rl < 0 || p9_check_error(v, rl, "Trenameat") != 0) return -1;
    return 0;
}

/* ---- VFS adapter ------------------------------------------------ */

/* Path-walked-and-opened state we hand back through vfs_inode. Since
 * vfs_inode has no place for a fid, we store the path in a per-mount
 * slot table and stash the slot index in obj_idx. open() captures
 * the path; read() / readdir() re-walk via the path on every call. */
#define V9P_PATH_MAX     128
#define V9P_INODE_SLOTS  16

struct v9p_inode {
    int  in_use;
    char path[V9P_PATH_MAX];
    uint64_t size;
    int  is_dir;
};
/* Heap-allocated at init to keep .bss small. The kernel image is
 * already close to the VGA-RAM ceiling at 0xA0000; statically
 * reserving ~2.3 KiB here would overflow. */
static struct v9p_inode *g_inodes;

static int v9p_inode_alloc(const char *path, uint64_t size, int is_dir) {
    for (int i = 0; i < V9P_INODE_SLOTS; i++) {
        if (!g_inodes[i].in_use) {
            g_inodes[i].in_use = 1;
            g_inodes[i].size   = size;
            g_inodes[i].is_dir = is_dir;
            int j = 0;
            while (path[j] && j < V9P_PATH_MAX - 1) {
                g_inodes[i].path[j] = path[j]; j++;
            }
            g_inodes[i].path[j] = 0;
            return i;
        }
    }
    return -1;
}

/* Walk + getattr a path under the share root. Returns 0 on success,
 * fills *out_size + *out_is_dir; caller-clunk-free (we manage the
 * fid lifetime here). */
static int v9p_lookup(struct v9p *v, const char *path,
                      uint64_t *out_size, int *out_is_dir)
{
    spin_lock(&v->lock);
    uint32_t tmp_fid;
    if (p9_alloc_fid(v, &tmp_fid) != 0) { spin_unlock(&v->lock); return -1; }
    int rc = p9_walk(v, v->root_fid, path, tmp_fid, out_is_dir);
    if (rc != 0) {
        p9_free_fid(v, tmp_fid);
        spin_unlock(&v->lock);
        return -1;
    }
    rc = p9_getattr(v, tmp_fid, out_size, out_is_dir);
    p9_clunk(v, tmp_fid);
    p9_free_fid(v, tmp_fid);
    spin_unlock(&v->lock);
    return rc;
}

static int v9p_vfs_open(void *fs_data, const char *rel_path,
                        struct vfs_inode *out)
{
    struct v9p *v = (struct v9p *)fs_data;
    if (!v->in_use) return -1;
    uint64_t size;
    int is_dir;
    if (v9p_lookup(v, rel_path, &size, &is_dir) != 0) return -1;

    int slot = v9p_inode_alloc(rel_path, size, is_dir);
    if (slot < 0) return -1;

    out->kind     = FD_9P;
    out->obj_idx  = slot;
    out->size     = (uint32_t)(size > 0xFFFFFFFFu ? 0xFFFFFFFFu : size);
    out->is_dir   = is_dir;
    out->fs_data  = fs_data;
    return 0;
}

static int v9p_vfs_read(void *fs_data, struct vfs_inode *ino,
                        uint32_t offset, void *buf, uint32_t n)
{
    struct v9p *v = (struct v9p *)fs_data;
    if (!v->in_use) return -1;
    if (ino->obj_idx < 0 || ino->obj_idx >= V9P_INODE_SLOTS) return -1;
    struct v9p_inode *it = &g_inodes[ino->obj_idx];
    if (!it->in_use) return -1;

    spin_lock(&v->lock);
    uint32_t tmp_fid;
    if (p9_alloc_fid(v, &tmp_fid) != 0) { spin_unlock(&v->lock); return -1; }

    int got = -1;
    int is_dir_check = 0;
    if (p9_walk(v, v->root_fid, it->path, tmp_fid, &is_dir_check) != 0) goto out;
    if (p9_lopen(v, tmp_fid, P9_O_RDONLY) != 0) goto out;
    got = p9_read(v, tmp_fid, offset, n, buf);

out:
    p9_clunk(v, tmp_fid);
    p9_free_fid(v, tmp_fid);
    spin_unlock(&v->lock);
    return got;
}

/* readdir: VFS iterates by integer index (*iter). We translate that
 * to a 9P offset by re-reading from offset 0 and skipping (*iter)
 * entries. Quadratic for large dirs, but each dir is typically tiny
 * and this matches the VFS API exactly. */
static int v9p_vfs_readdir(void *fs_data, const char *rel_dir,
                           int *iter, char *name_buf)
{
    struct v9p *v = (struct v9p *)fs_data;
    if (!v->in_use) return -1;

    spin_lock(&v->lock);
    uint32_t tmp_fid;
    if (p9_alloc_fid(v, &tmp_fid) != 0) { spin_unlock(&v->lock); return -1; }

    int found = -1;
    int is_dir = 0;
    if (p9_walk(v, v->root_fid, rel_dir, tmp_fid, &is_dir) != 0) goto done;
    if (p9_lopen(v, tmp_fid, P9_O_RDONLY) != 0) goto done;

    uint64_t off = 0;
    int idx = 0;
    char tmp_name[64];
    for (;;) {
        uint64_t next;
        int rc = p9_readdir_one(v, tmp_fid, off, tmp_name, sizeof(tmp_name), &next);
        if (rc <= 0) break;
        /* Skip "." and ".." — VFS callers expect bare entry names. */
        if (tmp_name[0] == '.' && tmp_name[1] == 0) { off = next; continue; }
        if (tmp_name[0] == '.' && tmp_name[1] == '.' && tmp_name[2] == 0) {
            off = next; continue;
        }
        if (idx == *iter) {
            int j = 0;
            while (tmp_name[j] && j < 15) { name_buf[j] = tmp_name[j]; j++; }
            name_buf[j] = 0;
            *iter = idx + 1;
            found = idx;
            break;
        }
        idx++;
        off = next;
    }

done:
    p9_clunk(v, tmp_fid);
    p9_free_fid(v, tmp_fid);
    spin_unlock(&v->lock);
    return found;
}

/* Split `path` into a parent directory and a basename. Both are
 * written into caller buffers. Returns 0 on success, -1 if the
 * basename would be empty or too long. The parent buffer receives
 * an empty string when there is no slash (= sibling of share root). */
static int split_parent_basename(const char *path,
                                 char *parent, int parent_cap,
                                 char *base,   int base_cap)
{
    /* Find the last '/'. */
    int last_slash = -1;
    int i = 0;
    while (path[i]) {
        if (path[i] == '/') last_slash = i;
        i++;
    }
    int total = i;
    int base_start = last_slash + 1;
    int base_len = total - base_start;
    if (base_len <= 0 || base_len >= base_cap) return -1;
    /* Copy basename. */
    for (int j = 0; j < base_len; j++) base[j] = path[base_start + j];
    base[base_len] = 0;
    /* Copy parent. */
    int p_len = last_slash > 0 ? last_slash : 0;
    if (p_len >= parent_cap) return -1;
    for (int j = 0; j < p_len; j++) parent[j] = path[j];
    parent[p_len] = 0;
    return 0;
}

static int v9p_vfs_mkdir(void *fs_data, const char *rel_path) {
    struct v9p *v = (struct v9p *)fs_data;
    if (!v->in_use) return -1;

    char parent[V9P_PATH_MAX], base[64];
    if (split_parent_basename(rel_path, parent, sizeof(parent),
                              base, sizeof(base)) != 0) return -1;

    spin_lock(&v->lock);
    uint32_t dfid;
    if (p9_alloc_fid(v, &dfid) != 0) { spin_unlock(&v->lock); return -1; }
    int rc = -1;
    int is_dir;
    if (p9_walk(v, v->root_fid, parent, dfid, &is_dir) != 0) goto done;
    if (p9_mkdir(v, dfid, base, 0755) != 0)                 goto done;
    rc = 0;
done:
    p9_clunk(v, dfid);
    p9_free_fid(v, dfid);
    spin_unlock(&v->lock);
    return rc;
}

/* Create / truncate `rel_path` and write the whole buffer to it.
 *
 * 9p workflow:
 *   1. Walk root -> parent directory dfid.
 *   2. Tlcreate dfid name flags=O_WRONLY|O_TRUNC mode=0644.
 *      After Tlcreate succeeds, dfid refers to the OPENED new file.
 *   3. Twrite in P9_MSIZE-sized chunks until all data is sent.
 *   4. Tclunk dfid.
 *
 * On a name that already exists, Tlcreate fails with EEXIST. We
 * handle that by retrying with Twalk-to-the-file + Tlopen O_TRUNC.
 *
 * Linux Tlcreate flags use the standard open(2) bit values:
 *   O_CREAT  = 0x40, O_TRUNC = 0x200, O_WRONLY = 1
 * We pass `O_TRUNC | O_WRONLY` and rely on Tlcreate's implicit
 * O_CREAT|O_EXCL behavior. */
#define P9_L_O_WRONLY  1
#define P9_L_O_TRUNC   0x200

static int v9p_vfs_write_all(void *fs_data, const char *rel_path,
                             const void *data, uint32_t n)
{
    struct v9p *v = (struct v9p *)fs_data;
    if (!v->in_use) return -1;

    char parent[V9P_PATH_MAX], base[64];
    if (split_parent_basename(rel_path, parent, sizeof(parent),
                              base, sizeof(base)) != 0) return -1;

    spin_lock(&v->lock);
    uint32_t fid;
    if (p9_alloc_fid(v, &fid) != 0) { spin_unlock(&v->lock); return -1; }

    int rc = -1;
    int is_dir;
    if (p9_walk(v, v->root_fid, parent, fid, &is_dir) != 0) goto done;

    /* Try create-new first. If that fails (file exists or other
     * error), drop the dir-fid and retry as walk-to-file + open
     * for write+trunc. */
    if (p9_lcreate(v, fid, base, P9_L_O_WRONLY | P9_L_O_TRUNC, 0644) != 0) {
        /* Re-walk: parent fid is now in a weird state, clunk it and
         * try walking directly to the existing file. */
        p9_clunk(v, fid);
        if (p9_walk(v, v->root_fid, rel_path, fid, &is_dir) != 0) goto done;
        if (p9_lopen(v, fid, P9_L_O_WRONLY | P9_L_O_TRUNC) != 0)  goto done;
    }

    /* Loop Twrite calls until the whole buffer is sent. */
    uint32_t off = 0;
    const uint8_t *p = (const uint8_t *)data;
    while (off < n) {
        int w = p9_write(v, fid, off, n - off, p + off);
        if (w <= 0) goto done;
        off += (uint32_t)w;
    }
    rc = 0;

done:
    p9_clunk(v, fid);
    p9_free_fid(v, fid);
    spin_unlock(&v->lock);
    return rc;
}

/* SYS_UNLINK + SYS_RMDIR dispatch path. Walks to the parent and
 * issues Tunlinkat. Public so syscall.c can call it for paths under
 * /mnt/9p. Returns 0 on success, -1 on any error. */
int virtio_9p_unlink_path(const char *rel_path, int is_dir) {
    struct v9p *v = &g_v9p;
    if (!v->in_use) return -1;

    char parent[V9P_PATH_MAX], base[64];
    if (split_parent_basename(rel_path, parent, sizeof(parent),
                              base, sizeof(base)) != 0) return -1;

    spin_lock(&v->lock);
    uint32_t dfid;
    if (p9_alloc_fid(v, &dfid) != 0) { spin_unlock(&v->lock); return -1; }
    int rc = -1;
    int is_dir_check;
    if (p9_walk(v, v->root_fid, parent, dfid, &is_dir_check) != 0) goto done;
    if (p9_unlinkat(v, dfid, base, is_dir) != 0)                   goto done;
    rc = 0;
done:
    p9_clunk(v, dfid);
    p9_free_fid(v, dfid);
    spin_unlock(&v->lock);
    return rc;
}

/* Rename `old_rel` -> `new_rel`, both relative to the 9p mount point.
 * Implemented via 9P Trenameat: walks olddir + newdir fids, issues
 * one atomic Trenameat call, clunks both. Returns 0 / -1. */
int virtio_9p_rename_path(const char *old_rel, const char *new_rel) {
    struct v9p *v = &g_v9p;
    if (!v->in_use) return -1;

    char old_parent[V9P_PATH_MAX], old_base[64];
    char new_parent[V9P_PATH_MAX], new_base[64];
    if (split_parent_basename(old_rel, old_parent, sizeof(old_parent),
                              old_base, sizeof(old_base)) != 0) return -1;
    if (split_parent_basename(new_rel, new_parent, sizeof(new_parent),
                              new_base, sizeof(new_base)) != 0) return -1;

    spin_lock(&v->lock);
    uint32_t old_fid, new_fid;
    if (p9_alloc_fid(v, &old_fid) != 0) {
        spin_unlock(&v->lock); return -1;
    }
    if (p9_alloc_fid(v, &new_fid) != 0) {
        p9_free_fid(v, old_fid); spin_unlock(&v->lock); return -1;
    }
    int rc = -1;
    int is_dir;
    if (p9_walk(v, v->root_fid, old_parent, old_fid, &is_dir) != 0) goto done;
    if (p9_walk(v, v->root_fid, new_parent, new_fid, &is_dir) != 0) goto done;
    if (p9_renameat(v, old_fid, old_base, new_fid, new_base) != 0)  goto done;
    rc = 0;
done:
    p9_clunk(v, old_fid);
    p9_clunk(v, new_fid);
    p9_free_fid(v, old_fid);
    p9_free_fid(v, new_fid);
    spin_unlock(&v->lock);
    return rc;
}

static struct vfs_fs_ops g_v9p_ops;

/* ---- init + mount ----------------------------------------------- */

void virtio_9p_init(void) {
    struct v9p *v = &g_v9p;
    if (v->in_use) return;

    if (pci_find(VIRTIO_VENDOR_ID, VIRTIO_LEGACY_9P, &v->pci) != 0) {
        return;
    }
    v->io = v->pci.io_base;
    spin_lock_init(&v->lock);

    kprintf("virtio-9p: PCI %u:%u.%u  io=0x%x  irq=%u\n",
            v->pci.bus, v->pci.device, v->pci.func,
            (unsigned)v->io, v->pci.irq_line);

    virtio_status_reset(v->io);
    virtio_status_ack(v->io);
    virtio_status_driver(v->io);
    /* virtio-9p has VIRTIO_9P_F_MOUNT_TAG (bit 0) — host advertises a
     * mount tag in config. We just ack feature bit 0 if offered. */
    uint32_t feats = virtio_negotiate(v->io, 0x1);
    (void)feats;

    if (virtio_queue_init(v->io, 0, &v->vq) != 0) {
        kprintf("virtio-9p: queue 0 setup failed\n");
        virtio_status_failed(v->io);
        return;
    }
    virtio_install_irq(v->io, v->pci.irq_line, 0, v);
    virtio_status_driver_ok(v->io);

    /* Read mount tag from device-specific config space:
     *   u16 tag_len
     *   char tag[tag_len] */
    uint16_t tag_len = (uint16_t)(
        inb(v->io + VIRTIO_PCI_CONFIG + 0) |
        ((uint16_t)inb(v->io + VIRTIO_PCI_CONFIG + 1) << 8));
    if (tag_len > P9_MAX_TAG_LEN) tag_len = P9_MAX_TAG_LEN;
    for (uint16_t i = 0; i < tag_len; i++) {
        v->mount_tag[i] = (char)inb(v->io + VIRTIO_PCI_CONFIG + 2 + i);
    }
    v->mount_tag[tag_len] = 0;
    kprintf("virtio-9p: mount_tag=\"%s\" (len=%u)\n",
            v->mount_tag, (unsigned)tag_len);

    /* Allocate per-message buffers + the inode-cache from the
     * kmalloc heap. Doing it here (not as .bss) keeps the kernel
     * image under the 0xA0000 ceiling. */
    v->tbuf = kmalloc(P9_MSIZE);
    v->rbuf = kmalloc(P9_MSIZE);
    g_inodes = (struct v9p_inode *)kmalloc(
        sizeof(struct v9p_inode) * V9P_INODE_SLOTS);
    if (!v->tbuf || !v->rbuf || !g_inodes) {
        kprintf("virtio-9p: scratch alloc failed\n");
        return;
    }
    for (int i = 0; i < V9P_INODE_SLOTS; i++) g_inodes[i].in_use = 0;
    v->msize = P9_MSIZE;

    /* Version handshake. Mandatory before any other request. */
    if (p9_version(v) != 0) {
        kprintf("virtio-9p: Tversion failed\n");
        return;
    }

    /* Attach a persistent root fid. */
    v->root_fid = 1;
    if (p9_attach(v, v->root_fid) != 0) {
        kprintf("virtio-9p: Tattach failed\n");
        return;
    }

    /* Wire up the VFS ops table (fs_data points to our state). */
    g_v9p_ops.fs_data    = v;
    g_v9p_ops.open       = v9p_vfs_open;
    g_v9p_ops.read       = v9p_vfs_read;
    g_v9p_ops.readdir    = v9p_vfs_readdir;
    g_v9p_ops.mkdir      = v9p_vfs_mkdir;
    g_v9p_ops.write_all  = v9p_vfs_write_all;

    v->in_use = 1;
    kprintf("virtio-9p: ready (root_fid=%u)\n", (unsigned)v->root_fid);
}

int virtio_9p_available(void) { return g_v9p.in_use; }

void virtio_9p_fd_close(int inode_slot) {
    if (inode_slot < 0 || inode_slot >= V9P_INODE_SLOTS) return;
    if (g_inodes && g_inodes[inode_slot].in_use) {
        g_inodes[inode_slot].in_use = 0;
    }
}

int virtio_9p_fd_read(int inode_slot, uint32_t offset, void *buf, uint32_t n) {
    if (!g_v9p.in_use) return -1;
    if (inode_slot < 0 || inode_slot >= V9P_INODE_SLOTS) return -1;
    if (!g_inodes || !g_inodes[inode_slot].in_use) return -1;
    /* Reuse the same vfs read path — it does walk + open + read + clunk. */
    struct vfs_inode ino = {
        .kind     = FD_9P,
        .obj_idx  = inode_slot,
        .size     = (uint32_t)(g_inodes[inode_slot].size > 0xFFFFFFFFu
                              ? 0xFFFFFFFFu : g_inodes[inode_slot].size),
        .is_dir   = g_inodes[inode_slot].is_dir,
        .fs_data  = &g_v9p,
    };
    return v9p_vfs_read(&g_v9p, &ino, offset, buf, n);
}

int virtio_9p_mount(const char *mount_point) {
    if (!g_v9p.in_use) return -1;
    /* vfs_mount expects the mount-point directory to exist. */
    extern int vfs_mkdir(const char *path);
    vfs_mkdir(mount_point);            /* idempotent */
    if (vfs_mount(mount_point, "9p", &g_v9p_ops) != 0) {
        kprintf("virtio-9p: vfs_mount(%s) failed\n", mount_point);
        return -1;
    }
    kprintf("virtio-9p: mounted at %s\n", mount_point);
    return 0;
}
