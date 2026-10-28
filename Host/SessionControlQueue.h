#ifndef LSB_SESSION_CONTROL_QUEUE_H
#define LSB_SESSION_CONTROL_QUEUE_H

// Call only after every process from the previous simulator session has actually exited. The supplied path must
// name the canonical per-build data directory. Missing queues succeed without creating a new queue.
// While holding worker.lock, preserve active/request/client.lock under one unique reboot-interrupted.<UUID>
// directory. Never replay requests, synthesize success, overwrite receipts, or follow queue symlinks.
// Returns an errno value and preserves caller errno. An error may leave safely archived evidence; do not start a new session.
int LSBQuarantineSessionControlQueue(const char *dataDirectory);

#endif
