#ifndef LSB_GUEST_FAKE_PTY_H
#define LSB_GUEST_FAKE_PTY_H

#include "../../ProcessSupport/FakePTY.h"

// The mapping remains owned by the caller. Production initializes it from the host's per-session state-file environment variable.
int LSBGuestFakePTYConfigure(LSBFakePTYState *state, size_t mappedBytes);
// Owned close-on-exec state descriptor for group-change wakeups. The spawn shim excludes it from guest descriptor handoff.
int LSBGuestFakePTYWatchFD(void);
int LSBPTYIsATTY(int fd);
int LSBPTYGetAttributes(int fd, struct termios *attributes);
int LSBPTYSetAttributes(int fd, int action, const struct termios *attributes);
pid_t LSBPTYGetForeground(int fd);
int LSBPTYSetForeground(int fd, pid_t foreground);
int LSBPTYIOCtl(int fd, unsigned long request, ...);
int LSBPTYFlush(int fd, int selector);
int LSBPTYDrain(int fd);
int LSBPTYFlow(int fd, int action);
int LSBPTYSendBreak(int fd, int duration);
int LSBPTYNameR(int fd, char *buffer, size_t length);
char *LSBPTYName(int fd);

#endif
