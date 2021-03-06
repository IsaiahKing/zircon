// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <threads.h>

#include <zircon/device/device.h>
#include <zircon/device/ioctl.h>
#include <zircon/device/vfs.h>
#include <zircon/processargs.h>
#include <zircon/syscalls.h>

#include <fdio/debug.h>
#include <fdio/io.h>
#include <fdio/namespace.h>
#include <fdio/remoteio.h>
#include <fdio/util.h>

#include "private-remoteio.h"

#define ZXDEBUG 0

// POLL_MASK and POLL_SHIFT intend to convert the lower five POLL events into
// ZX_USER_SIGNALs and vice-versa. Other events need to be manually converted to
// an zx_signal_t, if they are desired.
#define POLL_SHIFT  24
#define POLL_MASK   0x1F

static_assert(ZX_USER_SIGNAL_0 == (1 << POLL_SHIFT), "");
static_assert((POLLIN << POLL_SHIFT) == DEVICE_SIGNAL_READABLE, "");
static_assert((POLLPRI << POLL_SHIFT) == DEVICE_SIGNAL_OOB, "");
static_assert((POLLOUT << POLL_SHIFT) == DEVICE_SIGNAL_WRITABLE, "");
static_assert((POLLERR << POLL_SHIFT) == DEVICE_SIGNAL_ERROR, "");
static_assert((POLLHUP << POLL_SHIFT) == DEVICE_SIGNAL_HANGUP, "");

static pthread_key_t rchannel_key;

static void rchannel_cleanup(void* data) {
    if (data == NULL) {
        return;
    }
    zx_handle_t* handles = (zx_handle_t*)data;
    if (handles[0] != ZX_HANDLE_INVALID)
        zx_handle_close(handles[0]);
    if (handles[1] != ZX_HANDLE_INVALID)
        zx_handle_close(handles[1]);
    free(handles);
}

void __fdio_rchannel_init(void) {
    if (pthread_key_create(&rchannel_key, &rchannel_cleanup) != 0)
        abort();
}

static const char* _opnames[] = ZXRIO_OPNAMES;
const char* fdio_opname(uint32_t op) {
    op = ZXRIO_OPNAME(op);
    if (op < ZXRIO_NUM_OPS) {
        return _opnames[op];
    } else {
        return "unknown";
    }
}

static bool is_message_valid(zxrio_msg_t* msg) {
    if ((msg->datalen > FDIO_CHUNK_SIZE) ||
        (msg->hcount > FDIO_MAX_HANDLES)) {
        return false;
    }
    return true;
}

static bool is_message_reply_valid(zxrio_msg_t* msg, uint32_t size) {
    if ((size < ZXRIO_HDR_SZ) || (msg->datalen != (size - ZXRIO_HDR_SZ))) {
        return false;
    }
    return is_message_valid(msg);
}

static void discard_handles(zx_handle_t* handles, unsigned count) {
    while (count-- > 0) {
        zx_handle_close(*handles++);
    }
}

static zx_status_t zxrio_read_msg(zx_handle_t h, zxrio_msg_t* msg) {
    zx_status_t r;
    // NOTE: hcount intentionally received out-of-bound from the message to
    // avoid letting "client-supplied" bytes override the REAL hcount value.
    uint32_t hcount = 0;
    uint32_t dsz = sizeof(zxrio_msg_t);
    if ((r = zx_channel_read(h, 0, msg, msg->handle, dsz, FDIO_MAX_HANDLES,
                             &dsz, &hcount)) != ZX_OK) {
        return r;
    }
    // Now, "msg->hcount" can be trusted once again.
    msg->hcount = hcount;

    if (!is_message_reply_valid(msg, dsz)) {
        discard_handles(msg->handle, msg->hcount);
        return ZX_ERR_INVALID_ARGS;
    }
    return r;
}

zx_status_t zxrio_handle_rpc(zx_handle_t h, zxrio_msg_t* msg, zxrio_cb_t cb, void* cookie) {
    zx_status_t r = zxrio_read_msg(h, msg);
    if (r != ZX_OK) {
        return r;
    }
    bool is_close = (ZXRIO_OP(msg->op) == ZXRIO_CLOSE);

    msg->arg = cb(msg, cookie);
    switch (msg->arg) {
    case ERR_DISPATCHER_INDIRECT:
        // callback is handling the reply itself
        // and took ownership of the reply handle
        return ZX_OK;
    case ERR_DISPATCHER_ASYNC:
        // Same as the indirect case, but also identify that
        // the callback will asynchronously re-trigger the
        // dispatcher.
        return ERR_DISPATCHER_ASYNC;
    }

    r = zxrio_respond(h, msg);
    if (is_close) {
        // signals to not perform a close callback
        return ERR_DISPATCHER_DONE;
    } else {
        return r;
    }
}

zx_status_t zxrio_respond(zx_handle_t h, zxrio_msg_t* msg) {
    if ((msg->arg < 0) || !is_message_valid(msg)) {
        // in the event of an error response or bad message
        // release all the handles and data payload
        discard_handles(msg->handle, msg->hcount);
        msg->datalen = 0;
        msg->hcount = 0;
        // specific errors are prioritized over the bad
        // message case which we represent as ZX_ERR_INTERNAL
        // to differentiate from ZX_ERR_IO on the near side
        // TODO(ZX-974): consider a better error code
        msg->arg = (msg->arg < 0) ? msg->arg : ZX_ERR_INTERNAL;
    }
    zx_status_t s;
    msg->op = ZXRIO_STATUS;
    if ((s = zx_channel_write(h, 0, msg, ZXRIO_HDR_SZ + msg->datalen,
                              msg->handle, msg->hcount)) != ZX_OK) {
        discard_handles(msg->handle, msg->hcount);
    }
    return s;
}

zx_status_t zxrio_handle_close(zxrio_cb_t cb, void* cookie) {
    zxrio_msg_t msg;

    // remote side was closed;
    msg.op = ZXRIO_CLOSE;
    msg.arg = 0;
    msg.datalen = 0;
    msg.hcount = 0;
    cb(&msg, cookie);
    return ZX_OK;
}

zx_status_t zxrio_handler(zx_handle_t h, zxrio_cb_t cb, void* cookie) {
    if (h == ZX_HANDLE_INVALID) {
        return zxrio_handle_close(cb, cookie);
    } else {
        zxrio_msg_t msg;
        return zxrio_handle_rpc(h, &msg, cb, cookie);
    }
}

zx_status_t zxrio_txn_handoff(zx_handle_t srv, zx_handle_t reply, zxrio_msg_t* msg) {
    msg->txid = 0;
    msg->handle[0] = reply;
    msg->hcount = 1;

    zx_status_t r;
    uint32_t dsize = ZXRIO_HDR_SZ + msg->datalen;
    if ((r = zx_channel_write(srv, 0, msg, dsize, msg->handle, msg->hcount)) != ZX_OK) {
        // nothing to do but inform the caller that we failed
        struct {
            zx_status_t status;
            uint32_t type;
        } error = { r, 0 };
        zx_channel_write(reply, 0, &error, sizeof(error), NULL, 0);
        zx_handle_close(reply);
    }
    return r;
}

// on success, msg->hcount indicates number of valid handles in msg->handle
// on error there are never any handles
static zx_status_t zxrio_txn(zxrio_t* rio, zxrio_msg_t* msg) {
    if (!is_message_valid(msg)) {
        return ZX_ERR_INVALID_ARGS;
    }

    msg->txid = atomic_fetch_add(&rio->txid, 1);
    xprintf("txn h=%x txid=%x op=%d len=%u\n", rio->h, msg->txid, msg->op, msg->datalen);

    zx_status_t r;
    zx_status_t rs = ZX_ERR_INTERNAL;
    uint32_t dsize;

    zx_channel_call_args_t args;
    args.wr_bytes = msg;
    args.wr_handles = msg->handle;
    args.rd_bytes = msg;
    args.rd_handles = msg->handle;
    args.wr_num_bytes = ZXRIO_HDR_SZ + msg->datalen;
    args.wr_num_handles = msg->hcount;
    args.rd_num_bytes = ZXRIO_HDR_SZ + FDIO_CHUNK_SIZE;
    args.rd_num_handles = FDIO_MAX_HANDLES;

    r = zx_channel_call(rio->h, 0, ZX_TIME_INFINITE, &args, &dsize, &msg->hcount, &rs);
    if (r < 0) {
        if (r == ZX_ERR_CALL_FAILED) {
            // read phase failed, true status is in rs
            msg->hcount = 0;
            return rs;
        } else {
            // write phase failed, we must discard the handles
            goto fail_discard_handles;
        }
    }

    // check for protocol errors
    if (!is_message_reply_valid(msg, dsize) ||
        (ZXRIO_OP(msg->op) != ZXRIO_STATUS)) {
        r = ZX_ERR_IO;
        goto fail_discard_handles;
    }
    // check for remote error
    if ((r = msg->arg) < 0) {
        goto fail_discard_handles;
    }
    return r;

fail_discard_handles:
    // We failed either writing at all (still have the handles)
    // or after reading (need to abandon any handles we received)
    discard_handles(msg->handle, msg->hcount);
    msg->hcount = 0;
    return r;
}

ssize_t zxrio_ioctl(fdio_t* io, uint32_t op, const void* in_buf,
                    size_t in_len, void* out_buf, size_t out_len) {
    zxrio_t* rio = (zxrio_t*)io;
    const uint8_t* data = in_buf;
    zx_status_t r = 0;
    zxrio_msg_t msg;

    if (in_len > FDIO_IOCTL_MAX_INPUT || out_len > FDIO_CHUNK_SIZE) {
        return ZX_ERR_INVALID_ARGS;
    }

    memset(&msg, 0, ZXRIO_HDR_SZ);
    msg.op = ZXRIO_IOCTL;
    msg.datalen = in_len;
    msg.arg = out_len;
    msg.arg2.op = op;

    switch (IOCTL_KIND(op)) {
    case IOCTL_KIND_GET_HANDLE:
        if (out_len < sizeof(zx_handle_t)) {
            return ZX_ERR_INVALID_ARGS;
        }
        break;
    case IOCTL_KIND_GET_TWO_HANDLES:
        if (out_len < 2 * sizeof(zx_handle_t)) {
            return ZX_ERR_INVALID_ARGS;
        }
        break;
    case IOCTL_KIND_GET_THREE_HANDLES:
        if (out_len < 3 * sizeof(zx_handle_t)) {
            return ZX_ERR_INVALID_ARGS;
        }
        break;
    case IOCTL_KIND_SET_HANDLE:
        msg.op = ZXRIO_IOCTL_1H;
        if (in_len < sizeof(zx_handle_t)) {
            return ZX_ERR_INVALID_ARGS;
        }
        msg.hcount = 1;
        msg.handle[0] = *((zx_handle_t*) in_buf);
        break;
    case IOCTL_KIND_SET_TWO_HANDLES:
        msg.op = ZXRIO_IOCTL_2H;
        if (in_len < 2 * sizeof(zx_handle_t)) {
            return ZX_ERR_INVALID_ARGS;
        }
        msg.hcount = 2;
        msg.handle[0] = *((zx_handle_t*) in_buf);
        msg.handle[1] = *(((zx_handle_t*) in_buf) + 1);
        break;
    }

    memcpy(msg.data, data, in_len);

    if ((r = zxrio_txn(rio, &msg)) < 0) {
        return r;
    }

    size_t copy_len = msg.datalen;
    if (msg.datalen > out_len) {
        copy_len = out_len;
    }

    memcpy(out_buf, msg.data, copy_len);

    int handles = 0;
    switch (IOCTL_KIND(op)) {
        case IOCTL_KIND_GET_HANDLE:
            handles = (msg.hcount > 0 ? 1 : 0);
            if (handles) {
                memcpy(out_buf, msg.handle, sizeof(zx_handle_t));
            } else {
                memset(out_buf, 0, sizeof(zx_handle_t));
            }
            break;
        case IOCTL_KIND_GET_TWO_HANDLES:
            handles = (msg.hcount > 2 ? 2 : msg.hcount);
            if (handles) {
                memcpy(out_buf, msg.handle, handles * sizeof(zx_handle_t));
            }
            if (handles < 2) {
                memset(out_buf, 0, (2 - handles) * sizeof(zx_handle_t));
            }
            break;
        case IOCTL_KIND_GET_THREE_HANDLES:
            handles = (msg.hcount > 3 ? 3 : msg.hcount);
            if (handles) {
                memcpy(out_buf, msg.handle, handles * sizeof(zx_handle_t));
            }
            if (handles < 3) {
                memset(out_buf, 0, (3 - handles) * sizeof(zx_handle_t));
            }
            break;
    }
    discard_handles(msg.handle + handles, msg.hcount - handles);

    return r;
}

static ssize_t write_common(uint32_t op, fdio_t* io, const void* _data, size_t len, off_t offset) {
    zxrio_t* rio = (zxrio_t*)io;
    const uint8_t* data = _data;
    ssize_t count = 0;
    zx_status_t r = 0;
    zxrio_msg_t msg;
    ssize_t xfer;

    while (len > 0) {
        xfer = (len > FDIO_CHUNK_SIZE) ? FDIO_CHUNK_SIZE : len;

        memset(&msg, 0, ZXRIO_HDR_SZ);
        msg.op = op;
        msg.datalen = xfer;
        if (op == ZXRIO_WRITE_AT)
            msg.arg2.off = offset;
        memcpy(msg.data, data, xfer);

        if ((r = zxrio_txn(rio, &msg)) < 0) {
            break;
        }
        discard_handles(msg.handle, msg.hcount);

        if (r > xfer) {
            r = ZX_ERR_IO;
            break;
        }
        count += r;
        data += r;
        len -= r;
        if (op == ZXRIO_WRITE_AT)
            offset += r;
        // stop at short read
        if (r < xfer) {
            break;
        }
    }
    return count ? count : r;
}

static ssize_t zxrio_write(fdio_t* io, const void* _data, size_t len) {
    return write_common(ZXRIO_WRITE, io, _data, len, 0);
}

static ssize_t zxrio_write_at(fdio_t* io, const void* _data, size_t len, off_t offset) {
    return write_common(ZXRIO_WRITE_AT, io, _data, len, offset);
}

static ssize_t read_common(uint32_t op, fdio_t* io, void* _data, size_t len, off_t offset) {
    zxrio_t* rio = (zxrio_t*)io;
    uint8_t* data = _data;
    ssize_t count = 0;
    zx_status_t r = 0;
    zxrio_msg_t msg;
    ssize_t xfer;

    while (len > 0) {
        xfer = (len > FDIO_CHUNK_SIZE) ? FDIO_CHUNK_SIZE : len;

        memset(&msg, 0, ZXRIO_HDR_SZ);
        msg.op = op;
        msg.arg = xfer;
        if (op == ZXRIO_READ_AT)
            msg.arg2.off = offset;

        if ((r = zxrio_txn(rio, &msg)) < 0) {
            break;
        }
        discard_handles(msg.handle, msg.hcount);

        if ((r > (int)msg.datalen) || (r > xfer)) {
            r = ZX_ERR_IO;
            break;
        }
        memcpy(data, msg.data, r);
        count += r;
        data += r;
        len -= r;
        if (op == ZXRIO_READ_AT)
            offset += r;

        // stop at short read
        if (r < xfer) {
            break;
        }
    }
    return count ? count : r;
}

static ssize_t zxrio_read(fdio_t* io, void* _data, size_t len) {
    return read_common(ZXRIO_READ, io, _data, len, 0);
}

static ssize_t zxrio_read_at(fdio_t* io, void* _data, size_t len, off_t offset) {
    return read_common(ZXRIO_READ_AT, io, _data, len, offset);
}

static off_t zxrio_seek(fdio_t* io, off_t offset, int whence) {
    zxrio_t* rio = (zxrio_t*)io;
    zxrio_msg_t msg;
    zx_status_t r;

    memset(&msg, 0, ZXRIO_HDR_SZ);
    msg.op = ZXRIO_SEEK;
    msg.arg2.off = offset;
    msg.arg = whence;

    if ((r = zxrio_txn(rio, &msg)) < 0) {
        return r;
    }

    discard_handles(msg.handle, msg.hcount);
    return msg.arg2.off;
}

zx_status_t zxrio_close(fdio_t* io) {
    zxrio_t* rio = (zxrio_t*)io;
    zxrio_msg_t msg;
    zx_status_t r;

    memset(&msg, 0, ZXRIO_HDR_SZ);
    msg.op = ZXRIO_CLOSE;

    if ((r = zxrio_txn(rio, &msg)) >= 0) {
        discard_handles(msg.handle, msg.hcount);
    }

    zx_handle_t h = rio->h;
    rio->h = 0;
    zx_handle_close(h);
    if (rio->h2 > 0) {
        h = rio->h2;
        rio->h2 = 0;
        zx_handle_close(h);
    }

    return r;
}

// Synchronously (non-pipelined) open an object
static zx_status_t zxrio_sync_open_connection(zx_handle_t rio_h, zxrio_msg_t* msg,
                                              zxrio_describe_t* info, zx_handle_t* out) {
    zx_status_t r;
    zx_handle_t h;
    if ((r = zx_channel_create(0, &h, &msg->handle[0])) < 0) {
        return r;
    }
    msg->hcount = 1;

    // Write the (one-way) request message
    if ((r = zx_channel_write(rio_h, 0, msg, ZXRIO_HDR_SZ + msg->datalen,
                              msg->handle, msg->hcount)) < 0) {
        zx_handle_close(msg->handle[0]);
        zx_handle_close(h);
        return r;
    }

    zx_object_wait_one(h, ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED,
                       ZX_TIME_INFINITE, NULL);

    // Attempt to read the description from open
    uint32_t dsize = sizeof(*info);
    uint32_t actual_handles;
    r = zx_channel_read(h, 0, info, &info->handle, dsize, 1,
                        &dsize, &actual_handles);
    if (r != ZX_OK) {
        zx_handle_close(h);
        return r;
    }
    if (actual_handles == 0) {
        info->handle = ZX_HANDLE_INVALID;
    }
    if (dsize != sizeof(zxrio_describe_t) || info->op != ZXRIO_ON_OPEN) {
        r = ZX_ERR_IO;
    } else {
        r = info->status;
    }
    if (r != ZX_OK) {
        if (info->handle != ZX_HANDLE_INVALID) {
            zx_handle_close(info->handle);
        }
        zx_handle_close(h);
        h = ZX_HANDLE_INVALID;
    }
    *out = h;
    return r;
}

// This function always consumes the cnxn handle
// The svc handle is only used to send a message
static zx_status_t zxrio_connect(zx_handle_t svc, zx_handle_t cnxn,
                                 uint32_t op, uint32_t flags, uint32_t mode,
                                 const char* name) {
    size_t len = strlen(name);
    if (len >= PATH_MAX) {
        zx_handle_close(cnxn);
        return ZX_ERR_BAD_PATH;
    }
    if (flags & ZX_FS_FLAG_DESCRIBE) {
        zx_handle_close(cnxn);
        return ZX_ERR_INVALID_ARGS;
    }

    zxrio_msg_t msg;
    memset(&msg, 0, ZXRIO_HDR_SZ);
    msg.op = op;
    msg.datalen = len;
    msg.arg = flags;
    msg.arg2.mode = mode;
    msg.hcount = 1;
    msg.handle[0] = cnxn;
    memcpy(msg.data, name, len);

    zx_status_t r;
    if ((r = zx_channel_write(svc, 0, &msg, ZXRIO_HDR_SZ + msg.datalen, msg.handle, 1)) < 0) {
        zx_handle_close(cnxn);
        return r;
    }

    return ZX_OK;
}

zx_status_t fdio_service_connect(const char* svcpath, zx_handle_t h) {
    if (svcpath == NULL) {
        zx_handle_close(h);
        return ZX_ERR_INVALID_ARGS;
    }
    // Otherwise attempt to connect through the root namespace
    if (fdio_root_ns != NULL) {
        return fdio_ns_connect(fdio_root_ns, svcpath, h);
    }
    // Otherwise we fail
    zx_handle_close(h);
    return ZX_ERR_NOT_FOUND;
}

zx_status_t fdio_service_connect_at(zx_handle_t dir, const char* path, zx_handle_t h) {
    if (path == NULL) {
        zx_handle_close(h);
        return ZX_ERR_INVALID_ARGS;
    }
    if (dir == ZX_HANDLE_INVALID) {
        zx_handle_close(h);
        return ZX_ERR_UNAVAILABLE;
    }
    return zxrio_connect(dir, h, ZXRIO_OPEN, ZX_FS_RIGHT_READABLE |
                         ZX_FS_RIGHT_WRITABLE, 0755, path);
}

zx_handle_t fdio_service_clone(zx_handle_t svc) {
    zx_handle_t cli, srv;
    zx_status_t r;
    if (svc == ZX_HANDLE_INVALID) {
        return ZX_HANDLE_INVALID;
    }
    if ((r = zx_channel_create(0, &cli, &srv)) < 0) {
        return ZX_HANDLE_INVALID;
    }
    if ((r = zxrio_connect(svc, srv, ZXRIO_CLONE, ZX_FS_RIGHT_READABLE |
                           ZX_FS_RIGHT_WRITABLE, 0755, "")) < 0) {
        zx_handle_close(cli);
        return ZX_HANDLE_INVALID;
    }
    return cli;
}

zx_status_t fdio_service_clone_to(zx_handle_t svc, zx_handle_t srv) {
    if (srv == ZX_HANDLE_INVALID) {
        return ZX_ERR_INVALID_ARGS;
    }
    if (svc == ZX_HANDLE_INVALID) {
        zx_handle_close(srv);
        return ZX_ERR_INVALID_ARGS;
    }
    return zxrio_connect(svc, srv, ZXRIO_CLONE, ZX_FS_RIGHT_READABLE |
                         ZX_FS_RIGHT_WRITABLE, 0755, "");
}

zx_status_t zxrio_misc(fdio_t* io, uint32_t op, int64_t off,
                       uint32_t maxreply, void* ptr, size_t len) {
    zxrio_t* rio = (zxrio_t*)io;
    zxrio_msg_t msg;
    zx_status_t r;

    if ((len > FDIO_CHUNK_SIZE) || (maxreply > FDIO_CHUNK_SIZE)) {
        return ZX_ERR_INVALID_ARGS;
    }

    memset(&msg, 0, ZXRIO_HDR_SZ);
    msg.op = op;
    msg.arg = maxreply;
    msg.arg2.off = off;
    msg.datalen = len;
    if (ptr && len > 0) {
        memcpy(msg.data, ptr, len);
    }
    switch (op) {
    case ZXRIO_RENAME:
    case ZXRIO_LINK:
        // As a hack, 'Rename' and 'Link' take token handles through
        // the offset argument.
        msg.handle[0] = (zx_handle_t) off;
        msg.hcount = 1;
    }

    if ((r = zxrio_txn(rio, &msg)) < 0) {
        return r;
    }

    switch (op) {
    case ZXRIO_MMAP: {
        // Ops which receive single handles:
        if ((msg.hcount != 1) || (msg.datalen > maxreply)) {
            discard_handles(msg.handle, msg.hcount);
            return ZX_ERR_IO;
        }
        r = msg.handle[0];
        memcpy(ptr, msg.data, msg.datalen);
        break;
    }
    case ZXRIO_FCNTL:
        // This is a bit of a hack, but for this case, we
        // return 'msg.arg2.mode' in the data field to simplify
        // this call for the client.
        discard_handles(msg.handle, msg.hcount);
        if (ptr) {
            memcpy(ptr, &msg.arg2.mode, sizeof(msg.arg2.mode));
        }
        break;
    default:
        // Ops which don't receive handles:
        discard_handles(msg.handle, msg.hcount);
        if (msg.datalen > maxreply) {
            return ZX_ERR_IO;
        }
        if (ptr && msg.datalen > 0) {
            memcpy(ptr, msg.data, msg.datalen);
        }
    }
    return r;
}

zx_status_t fdio_create_fd(zx_handle_t* handles, uint32_t* types, size_t hcount,
                           int* fd_out) {
    fdio_t* io;
    zx_status_t r;
    int fd;
    uint32_t type;

    switch (PA_HND_TYPE(types[0])) {
    case PA_FDIO_REMOTE:
        type = FDIO_PROTOCOL_REMOTE;
        break;
    case PA_FDIO_PIPE:
        type = FDIO_PROTOCOL_PIPE;
        break;
    case PA_FDIO_SOCKET:
        type = FDIO_PROTOCOL_SOCKET_CONNECTED;
        break;
    default:
        r = ZX_ERR_IO;
        goto fail;
    }

    if ((r = fdio_from_handles(type, handles, hcount, NULL, &io)) != ZX_OK) {
        goto fail;
    }

    fd = fdio_bind_to_fd(io, -1, 0);
    if (fd < 0) {
        fdio_close(io);
        fdio_release(io);
        return ZX_ERR_BAD_STATE;
    }

    *fd_out = fd;
    return ZX_OK;
fail:
    for (size_t i = 0; i < hcount; i++) {
        zx_handle_close(handles[i]);
    }
    return r;
}

zx_status_t fdio_from_handles(uint32_t type, zx_handle_t* handles, int hcount,
                              const zxrio_object_info_t* extra, fdio_t** out) {
    // All failure cases which require discard_handles set r and break
    // to the end. All other cases in which handle ownership is moved
    // on return locally.
    zx_status_t r;
    fdio_t* io;
    switch (type) {
    case FDIO_PROTOCOL_REMOTE:
        if (hcount == 1) {
            io = fdio_remote_create(handles[0], 0);
            xprintf("rio (%x,%x) -> %p\n", handles[0], 0, io);
        } else if (hcount == 2) {
            io = fdio_remote_create(handles[0], handles[1]);
            xprintf("rio (%x,%x) -> %p\n", handles[0], handles[1], io);
        } else {
            r = ZX_ERR_INVALID_ARGS;
            break;
        }
        if (io == NULL) {
            return ZX_ERR_NO_RESOURCES;
        } else {
            *out = io;
            return ZX_OK;
        }
        break;
    case FDIO_PROTOCOL_SERVICE:
        if (hcount != 1) {
            r = ZX_ERR_INVALID_ARGS;
            break;
        } else if ((*out = fdio_service_create(handles[0])) == NULL) {
            return ZX_ERR_NO_RESOURCES;
        } else {
            return ZX_OK;
        }
        break;
    case FDIO_PROTOCOL_PIPE:
        if (hcount != 1) {
            r = ZX_ERR_INVALID_ARGS;
            break;
        } else if ((*out = fdio_pipe_create(handles[0])) == NULL) {
            return ZX_ERR_NO_RESOURCES;
        } else {
            return ZX_OK;
        }
    case FDIO_PROTOCOL_VMOFILE: {
        if (hcount != 2) {
            r = ZX_ERR_INVALID_ARGS;
            break;
        }
        // Currently, VMO Files don't use a client-side control channel.
        zx_handle_close(handles[0]);
        *out = fdio_vmofile_create(handles[1], extra->vmofile.offset, extra->vmofile.length);
        if (*out == NULL) {
            return ZX_ERR_NO_RESOURCES;
        } else {
            return ZX_OK;
        }
    }
    case FDIO_PROTOCOL_SOCKET_CONNECTED:
    case FDIO_PROTOCOL_SOCKET: {
        int flags = (type == FDIO_PROTOCOL_SOCKET_CONNECTED) ? FDIO_FLAG_SOCKET_CONNECTED : 0;
#if WITH_NEW_SOCKET
        if (hcount != 2) {
            r = ZX_ERR_INVALID_ARGS;
            break;
        }
        zx_handle_close(handles[0]);
        if ((*out = fdio_socket_create(handles[1], flags)) == NULL) {
            return ZX_ERR_NO_RESOURCES;
        } else {
            return ZX_OK;
        }
#else
        if (hcount == 1) {
            io = fdio_socket_create(handles[0], ZX_HANDLE_INVALID, flags);
        } else if (hcount == 2) {
            io = fdio_socket_create(handles[0], handles[1], flags);
        } else {
            r = ZX_ERR_INVALID_ARGS;
            break;
        }
        if (io == NULL) {
            return ZX_ERR_NO_RESOURCES;
        } else {
            *out = io;
            return ZX_OK;
        }
#endif
    }
    default:
        r = ZX_ERR_NOT_SUPPORTED;
        break;
    }
    discard_handles(handles, hcount);
    return r;
}

zx_status_t zxrio_getobject(zx_handle_t rio_h, uint32_t op, const char* name,
                            uint32_t flags, uint32_t mode,
                            zxrio_describe_t* info, zx_handle_t* out) {
    if (name == NULL) {
        return ZX_ERR_INVALID_ARGS;
    }

    size_t len = strlen(name);
    if (len >= PATH_MAX) {
        return ZX_ERR_BAD_PATH;
    }

    if (flags & ZX_FS_FLAG_DESCRIBE) {
        zxrio_msg_t msg;
        memset(&msg, 0, ZXRIO_HDR_SZ);
        msg.op = op;
        msg.datalen = len;
        msg.arg = flags;
        msg.arg2.mode = mode;
        memcpy(msg.data, name, len);
        return zxrio_sync_open_connection(rio_h, &msg, info, out);
    } else {
        zx_handle_t h0, h1;
        zx_status_t r;
        if ((r = zx_channel_create(0, &h0, &h1)) < 0) {
            return r;
        }
        if ((r = zxrio_connect(rio_h, h1, ZXRIO_OPEN, flags, mode, name)) < 0) {
            zx_handle_close(h0);
            return r;
        }
        // fake up a reply message since pipelined opens don't generate one
        info->status = ZX_OK;
        info->type = FDIO_PROTOCOL_REMOTE;
        info->handle = ZX_HANDLE_INVALID;
        *out = h0;
        return ZX_OK;
    }
}

zx_status_t zxrio_open_handle(zx_handle_t h, const char* path, uint32_t flags,
                              uint32_t mode, fdio_t** out) {
    zx_handle_t control_channel;
    zxrio_describe_t info;
    zx_status_t r = zxrio_getobject(h, ZXRIO_OPEN, path, flags, mode, &info, &control_channel);
    if (r < 0) {
        return r;
    }
    zx_handle_t handles[2];
    size_t hcount = (info.handle != ZX_HANDLE_INVALID) ? 2 : 1;
    handles[0] = control_channel;
    handles[1] = info.handle;
    return fdio_from_handles(info.type, handles, hcount, &info.extra, out);
}

zx_status_t zxrio_open_handle_raw(zx_handle_t h, const char* path, uint32_t flags,
                                  uint32_t mode, zx_handle_t *out) {
    zx_handle_t control_channel;
    zxrio_describe_t info;
    zx_status_t r = zxrio_getobject(h, ZXRIO_OPEN, path, flags, mode, &info, &control_channel);
    if (r < 0) {
        return r;
    }
    if (info.type == FDIO_PROTOCOL_REMOTE) {
        if (info.handle != ZX_HANDLE_INVALID) {
            zx_handle_close(info.handle);
        }
        *out = control_channel;
        return ZX_OK;
    }
    if (info.handle != ZX_HANDLE_INVALID) {
        zx_handle_close(info.handle);
    }
    zx_handle_close(control_channel);
    return ZX_ERR_WRONG_TYPE;
}

zx_status_t zxrio_open(fdio_t* io, const char* path, uint32_t flags, uint32_t mode, fdio_t** out) {
    zxrio_t* rio = (void*)io;
    return zxrio_open_handle(rio->h, path, flags, mode, out);
}

static zx_status_t zxrio_clone(fdio_t* io, zx_handle_t* handles, uint32_t* types) {
    zxrio_t* rio = (void*)io;
    zx_handle_t h;
    zxrio_describe_t info;
    zx_status_t r = zxrio_getobject(rio->h, ZXRIO_CLONE, "", ZX_FS_FLAG_DESCRIBE, 0, &info, &h);
    if (r < 0) {
        return r;
    }
    handles[0] = h;
    types[0] = PA_FDIO_REMOTE;
    if (info.handle != ZX_HANDLE_INVALID) {
        handles[1] = info.handle;
        types[1] = PA_FDIO_REMOTE;
    }
    return (info.handle != ZX_HANDLE_INVALID) ? 2 : 1;
}

zx_status_t __zxrio_clone(zx_handle_t h, zx_handle_t* handles, uint32_t* types) {
    zxrio_t rio;
    rio.h = h;
    return zxrio_clone(&rio.io, handles, types);
}

static zx_status_t zxrio_unwrap(fdio_t* io, zx_handle_t* handles, uint32_t* types) {
    zxrio_t* rio = (void*)io;
    zx_status_t r;
    handles[0] = rio->h;
    types[0] = PA_FDIO_REMOTE;
    if (rio->h2 != 0) {
        handles[1] = rio->h2;
        types[1] = PA_FDIO_REMOTE;
        r = 2;
    } else {
        r = 1;
    }
    free(io);
    return r;
}

static void zxrio_wait_begin(fdio_t* io, uint32_t events, zx_handle_t* handle, zx_signals_t* _signals) {
    zxrio_t* rio = (void*)io;
    *handle = rio->h2;

    zx_signals_t signals = 0;
    // Manually add signals that don't fit within POLL_MASK
    if (events & POLLRDHUP) {
        signals |= ZX_CHANNEL_PEER_CLOSED;
    }

    // POLLERR is always detected
    *_signals = (((POLLERR | events) & POLL_MASK) << POLL_SHIFT) | signals;
}

static void zxrio_wait_end(fdio_t* io, zx_signals_t signals, uint32_t* _events) {
    // Manually add events that don't fit within POLL_MASK
    uint32_t events = 0;
    if (signals & ZX_CHANNEL_PEER_CLOSED) {
        events |= POLLRDHUP;
    }
    *_events = ((signals >> POLL_SHIFT) & POLL_MASK) | events;
}

static fdio_ops_t zx_remote_ops = {
    .read = zxrio_read,
    .read_at = zxrio_read_at,
    .write = zxrio_write,
    .write_at = zxrio_write_at,
    .recvfrom = fdio_default_recvfrom,
    .sendto = fdio_default_sendto,
    .recvmsg = fdio_default_recvmsg,
    .sendmsg = fdio_default_sendmsg,
    .misc = zxrio_misc,
    .seek = zxrio_seek,
    .close = zxrio_close,
    .open = zxrio_open,
    .clone = zxrio_clone,
    .ioctl = zxrio_ioctl,
    .wait_begin = zxrio_wait_begin,
    .wait_end = zxrio_wait_end,
    .unwrap = zxrio_unwrap,
    .shutdown = fdio_default_shutdown,
    .posix_ioctl = fdio_default_posix_ioctl,
    .get_vmo = fdio_default_get_vmo,
};

fdio_t* fdio_remote_create(zx_handle_t h, zx_handle_t e) {
    zxrio_t* rio = calloc(1, sizeof(*rio));
    if (rio == NULL) {
        zx_handle_close(h);
        zx_handle_close(e);
        return NULL;
    }
    rio->io.ops = &zx_remote_ops;
    rio->io.magic = FDIO_MAGIC;
    atomic_init(&rio->io.refcount, 1);
    rio->h = h;
    rio->h2 = e;
    atomic_init(&rio->txid, 1);
    return &rio->io;
}
