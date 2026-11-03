#pragma once

#include <sys/event.h>

// Called once after the guest runtime has initialized, only for processes attached to the fake terminal.
int LSBStartTerminalStopWitness(struct kevent64_s *sharedEvent);
