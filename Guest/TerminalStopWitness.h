#pragma once

#include <sys/event.h>
#include <stdint.h>

// Called once after the guest runtime has initialized, only for processes attached to the fake terminal.
int LSBStartTerminalStopWitness(struct kevent64_s *sharedEvent);
// Exposed to the fork trial for authenticated parking of this runtime-owned worker.
uint64_t LSBTerminalStopWitnessThreadID(void);
