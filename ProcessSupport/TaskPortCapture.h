#pragma once
#include <mach/mach.h>

enum { LSB_TASK_CAPTURE_NONCE_BYTES = 16 };
typedef struct {
    mach_port_t port;
    unsigned char nonce[LSB_TASK_CAPTURE_NONCE_BYTES];
} LSBTaskPortCaptureOffer;

// Native host: create a receive right and one send right with the same name. The caller owns both and releases them separately.
int LSBTaskPortCaptureCreate(LSBTaskPortCaptureOffer *offer);
// Consume at most one queued exception without waiting. Success returns one owned kernel-supplied task send right.
// The caller keeps the offer alive and arranges timeout/cancellation. EAGAIN means no message was queued.
int LSBTaskPortCaptureReceive(const LSBTaskPortCaptureOffer *offer, const audit_token_t *expectedAudit, mach_port_t *task);
// Native child: temporarily register only its current thread's breakpoint handler and perform a nonce-bearing rendezvous.
// The caller retains ownership of the offer's send right. No worker thread or task-self right transfer is used.
int LSBTaskPortCapturePerform(const LSBTaskPortCaptureOffer *offer);
