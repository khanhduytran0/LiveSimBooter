#include "../Host/HostApplication.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    const char *icon[] = {"LiveSimBooter", NULL};
    assert(!LSBHostArgumentsRequestAutomaticStart(1, icon));
    const char *services[] = {"LiveSimBooter", "--launchd-services", NULL};
    const char *terminal[] = {"LiveSimBooter", "--terminal", "/bin/fish", NULL};
    const char *guest[] = {"LiveSimBooter", "/bin/df", "-h", NULL};
    const char *invalidPlan[] = {"LiveSimBooter", "--unknown-test-option", NULL};
    assert(LSBHostArgumentsRequestAutomaticStart(2, services));
    assert(LSBHostArgumentsRequestAutomaticStart(3, terminal));
    assert(LSBHostArgumentsRequestAutomaticStart(3, guest));
    assert(LSBHostArgumentsRequestAutomaticStart(2, invalidPlan)); // Report plan errors instead of silently waiting for a tap.
    assert(!LSBHostArgumentsRequestAutomaticStart(0, NULL));
    assert(!LSBHostArgumentsRequestAutomaticStart(-1, icon));
    assert(!LSBHostArgumentsRequestAutomaticStart(2, NULL));
    assert(!LSBHostArgumentsRequestAutomaticStart(2, icon));
    assert(LSBHostShouldAttemptGuestStart(false, 0, true));
    assert(!LSBHostShouldAttemptGuestStart(true, 0, false)); // Never overlap the running/starting/draining broker.
    assert(LSBHostShouldAttemptGuestStart(true, 0, true)); // Successful Power Off allows Start in the same native host.
    assert(!LSBHostShouldAttemptGuestStart(true, 5, true)); // A partial startup failure stays latched.
    assert(!LSBHostShouldAttemptGuestStart(true, 5, false));
    puts("Host startup policy: icon launches wait; explicit guest/test arguments start automatically.");
    return 0;
}
