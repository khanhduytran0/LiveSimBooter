#ifndef LSB_CHILD_STATUS_H
#define LSB_CHILD_STATUS_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/event.h>
#include <sys/types.h>

#define LSB_CHILD_STATUS_MAP_SIZE 4096
#define LSB_CHILD_STATUS_MAPPING_SIZE LSB_CHILD_STATUS_MAP_SIZE
#define LSB_CHILD_STATUS_ENV "LSB_CHILD_STATUS"
#define LSB_CHILD_WAKE_FD_ENV "LSB_CHILD_WAKE_FD"
#define LSB_CHILD_STATUS_MAGIC 0x4c534243u
#define LSB_CHILD_STATUS_VERSION 8u
// Widget providers need headroom beyond 64 live launchd children. Keep the ledger within its existing 4096-byte mapping
// and the derived broker job limit within its 256-record process list.
#define LSB_CHILD_STATUS_CAPACITY 120u

typedef struct {
    // High 39 bits: ticket generation; next 5: pending job signal; next 16: observer pins; low 4: phase. One CAS protects reuse against new pins.
    _Atomic(uint64_t) state;
    // Low 32 bits: native PID. High 32 bits: executable generation. One atomic prevents mixed replacement observations.
    _Atomic(uint64_t) execution;
    _Atomic(int32_t) pid, group, status;
} LSBChildSlot;

typedef struct {
    uint32_t magic, version, capacity;
    _Atomic(uint32_t) suppressJobSignal;
    _Atomic(uint64_t) nextGeneration;
    _Atomic(uint32_t) discardExited;
    _Atomic(int32_t) parentPID;
    _Atomic(uint64_t) terminalSessionAndGroup;
    // Upper bits identify a default tty-stop handler invocation. Low 5 bits hold its signal while it is in kill(self, SIGSTOP).
    _Atomic(uint32_t) terminalStop;
    // The terminal guest waits for EVFILT_SIGNAL(SIGSTOP) with its event list in this shared mapping.
    // XNU copies udata here while the guest is stopped. The high word is the native audit PID version; the low bit is one for STOP, zero for CONT.
    struct kevent64_s nativeStopEvent;
    LSBChildSlot slots[LSB_CHILD_STATUS_CAPACITY];
} LSBChildStatus;

_Static_assert(sizeof(struct kevent64_s) == 48 && offsetof(struct kevent64_s, udata) == 24, "Darwin 64-bit kevent ABI changed");
_Static_assert(sizeof(LSBChildSlot) == 32 && offsetof(LSBChildStatus, nativeStopEvent) == 48 && offsetof(LSBChildStatus, slots) == 96,
               "Child status wire layout changed");
_Static_assert(sizeof(LSBChildStatus) <= LSB_CHILD_STATUS_MAP_SIZE, "Child status mapping is too small");

typedef struct { uint32_t index; uint64_t generation; } LSBChildTicket;
typedef struct {
    LSBChildTicket ticket;
    pid_t nativePID;
    uint32_t execGeneration;
    int exited, status;
    pid_t guestPID, group;
} LSBChildObservation;
typedef struct {
    LSBChildTicket ticket;
    uint64_t previousExecution, replacementExecution;
    pid_t replacementGroup; // Zero preserves the existing group. The broker sets this when exec attaches the terminal helper.
} LSBChildExecUpdate;

void LSBChildStatusInitialize(LSBChildStatus *ledger);
int LSBChildStatusValidate(const LSBChildStatus *ledger);
// -1 means no broker-supplied parent identity; zero is a known native-host-only parent.
int LSBChildStatusSetParentPID(LSBChildStatus *ledger, pid_t parentPID);
pid_t LSBChildStatusGetParentPID(const LSBChildStatus *ledger);
// Reservations consume a new generation; reaped slots become reusable after the final observer releases its pin.
// No pointers cross a process boundary.
int LSBChildStatusReserve(LSBChildStatus *ledger, LSBChildTicket *ticket);
int LSBChildStatusPublishRunning(LSBChildStatus *ledger, LSBChildTicket ticket, pid_t pid, pid_t group);
int LSBChildStatusSetGroup(LSBChildStatus *ledger, LSBChildTicket ticket, pid_t group);
int LSBChildStatusPublishJobEvent(LSBChildStatus *ledger, LSBChildTicket ticket, int signalNumber);
// Publishers serialize updates per ticket. Replacement preserves the logical PID/group and increments execGeneration from zero.
int LSBChildStatusPublishExec(LSBChildStatus *ledger, LSBChildTicket ticket, pid_t nativePID);
// Prepare excludes exit/other replacement publication. The owning publisher must serialize and finish exactly once.
// Pending execution stays in the update, so a canceled replacement can never leak through a concurrent observation.
int LSBChildStatusPrepareExec(LSBChildStatus *ledger, LSBChildTicket ticket, pid_t nativePID, LSBChildExecUpdate *update);
int LSBChildStatusCommitExec(LSBChildStatus *ledger, const LSBChildExecUpdate *update);
int LSBChildStatusCancelExec(LSBChildStatus *ledger, const LSBChildExecUpdate *update);
// Nonconsuming snapshot. ECHILD means absent/reaped; EAGAIN means a publication raced the bounded scan. Output changes only on success.
int LSBChildStatusObserve(const LSBChildStatus *ledger, pid_t guestPID, LSBChildObservation *observation);
// Enumerates only unconsumed children for adoption. Import reserves a new ticket and preserves execution identity and final status.
int LSBChildStatusObserveIndex(const LSBChildStatus *ledger, unsigned index, LSBChildObservation *observation);
int LSBChildStatusImport(LSBChildStatus *ledger, const LSBChildObservation *observation, LSBChildTicket *ticket);
// A successful acquire owns one pin. Reaped/discarded exit remains visible by exact ticket until every pin is released.
// Pins and phase share one atomic, so reuse cannot race acquisition. Abandoning a live child reports ESTALE, never EXIT.
int LSBChildStatusAcquireObservation(LSBChildStatus *ledger, pid_t guestPID, LSBChildObservation *observation);
int LSBChildStatusObserveTicket(const LSBChildStatus *ledger, LSBChildTicket ticket, LSBChildObservation *observation);
int LSBChildStatusReleaseObservation(LSBChildStatus *ledger, LSBChildTicket ticket);
int LSBChildStatusPublishExit(LSBChildStatus *ledger, LSBChildTicket ticket, int waitStatus);
// Removes ownership of a reserved/running/exited child. Existing observers retain an actual exit but never invent one for a live removal.
int LSBChildStatusAbandon(LSBChildStatus *ledger, LSBChildTicket ticket);
// 0: one child reaped; EAGAIN: matching live child; ECHILD: none. Outputs change only on success.
int LSBChildStatusTryWait(LSBChildStatus *ledger, pid_t selector, pid_t currentGroup, pid_t *pid, int *status);
int LSBChildStatusTryWaitOptions(LSBChildStatus *ledger, pid_t selector, pid_t currentGroup, int options, pid_t *pid, int *status);
// SIG_IGN and SA_NOCLDWAIT discard both pending and future statuses. Uses only lock-free atomics.
void LSBChildStatusSetDiscard(LSBChildStatus *ledger, int discard);

#endif
