#ifndef LSB_PROCESS_SESSION_H
#define LSB_PROCESS_SESSION_H

#include <sys/types.h>

#define LSB_SESSION_ENABLE_ENV "LSB_SIMULATE_SESSIONS"
#define LSB_SESSION_ID_ENV "LSB_GUEST_SID"
#define LSB_PROCESS_GROUP_ENV "LSB_GUEST_PGID"

// The attached fake terminal uses broker-owned logical groups. Other opted-in processes retain local session metadata.
// Spawn/exec must replace inherited SID/PGID environment entries with this snapshot, including when envp is rebuilt by xpcproxy.
// Returns ENOTSUP when disabled, EINVAL for malformed inherited metadata, or a native initialization error.
int LSBProcessSessionSnapshot(pid_t *session, pid_t *group);
pid_t LSBGuestProcessGroup(void);
pid_t LSBGuestGetPGID(pid_t pid);
pid_t LSBGuestGetSID(pid_t pid);
pid_t LSBGuestSetSID(void);
int LSBGuestSetPGID(pid_t pid, pid_t group);
pid_t LSBGuestSetPGrp(void);

#endif
