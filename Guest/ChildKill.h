#ifndef LSB_CHILD_KILL_H
#define LSB_CHILD_KILL_H

#include <sys/types.h>

int LSBKill(pid_t pid, int signalNumber);

int LSBKillPG(pid_t group, int signalNumber);

#endif
