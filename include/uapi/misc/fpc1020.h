/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/*
 * FPC fingerprint sensor behind a QSEE trustlet: /dev/fpc_tee.
 *
 * Copyright (C) 2026 Giuseppe Maggio <jertlok@proton.me>
 */
#ifndef _UAPI_MISC_FPC1020_H
#define _UAPI_MISC_FPC1020_H

#include <linux/ioctl.h>
#include <linux/types.h>

/**
 * struct fpc_tee_xfer - a command buffer for the trustlet
 * @buf: Address of the buffer, which the trustlet rewrites in place.
 * @len: Length of the buffer, at most one page. The trustlet reads it as the
 *       size of the buffer it may use, so pass the real size.
 * @pad: Must be zero.
 */
struct fpc_tee_xfer {
	__u64 buf;
	__u32 len;
	__u32 pad;
};

/**
 * struct fpc_tee_listener - a listener request from the trustlet
 * @id:         FPC_TEE_IOC_LISTENER_RECV: the listener service the request
 *              is for. FPC_TEE_IOC_LISTENER_SEND: the same ID.
 * @status:     FPC_TEE_IOC_LISTENER_SEND: 0 if the request was served,
 *              nonzero to fail it. The service's own result, if it has one,
 *              goes in the response.
 * @buf:        Address of a buffer for the request, or of the response.
 * @len:        FPC_TEE_IOC_LISTENER_RECV: size of @buf on entry, length of
 *              the request copied on return. FPC_TEE_IOC_LISTENER_SEND:
 *              length of the response.
 * @timeout_ms: FPC_TEE_IOC_LISTENER_RECV: how long to wait for a request.
 *
 * The request and response layouts are the listener service's, as QSEE
 * defines them.
 */
struct fpc_tee_listener {
	__u32 id;
	__s32 status;
	__u64 buf;
	__u32 len;
	__u32 timeout_ms;
};

/* Carry one command buffer to the trustlet and back, loading it on first use. */
#define FPC_TEE_IOC_XFER		_IOWR('F', 0x01, struct fpc_tee_xfer)
/* Wait for a listener request raised by a command in flight. */
#define FPC_TEE_IOC_LISTENER_RECV	_IOWR('F', 0x02, struct fpc_tee_listener)
/* Answer the request last received, resuming the command. */
#define FPC_TEE_IOC_LISTENER_SEND	_IOW('F', 0x03, struct fpc_tee_listener)

#endif /* _UAPI_MISC_FPC1020_H */
