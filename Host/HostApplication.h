#pragma once

#include <stdbool.h>

// Icon launches have only argv[0]. Preserve automatic execution for explicit development/test arguments, including invalid plans.
static inline bool LSBHostArgumentsRequestAutomaticStart(int argc, const char *const argv[]) {
    return argc > 1 && argv && argv[0] && argv[1];
}

int LSBRunHostApplication(int argc, const char *argv[], const char *envp[]);
bool LSBInitialGuestStartsAutomatically(void);
bool LSBInitialGuestStartupWasAttempted(void);
int LSBInitialGuestStartupError(void);
// A successful powered-off session may start again. Failures remain latched because broker ownership can be incomplete.
static inline bool LSBHostShouldAttemptGuestStart(bool attempted, int priorError, bool poweredOff) {
    return !attempted || (!priorError && poweredOff);
}
// Main-thread startup, including a later Start after confirmed Power Off.
int LSBStartInitialGuest(void);
