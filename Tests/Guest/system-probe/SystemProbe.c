#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>

// The iPhoneOS SDK declares system() unavailable but libSystem exports the symbol. Import it exactly as a prebuilt app would.
#ifndef LSB_SYSTEM_PROBE_SYMBOL
#define LSB_SYSTEM_PROBE_SYMBOL "_system"
#endif
extern int importedSystem(const char *command) __asm(LSB_SYSTEM_PROBE_SYMBOL);

static int run(const char *label, const char *command) {
    errno = 0;
    int result = importedSystem(command);
    printf("%s result=%d errno=%d\n", label, result, result == -1 ? errno : 0);
    return result;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (setenv("LSB_SYSTEM_PROBE_VALUE", "two words", 1)) return 1;
    int available = run("shell", NULL);
    int exited = run("quoted", "test \"$LSB_SYSTEM_PROBE_VALUE\" = 'two words' || exit 1; printf '%s\\n' 'system child'; exit 37");
    int external = run("df", "\"$DYLD_ROOT_PATH/bin/df\" -P . >/dev/null");
    int missing = run("missing", "/lsb-system-probe-missing-command 2>/dev/null");
    int signalled = run("signal", "kill -TERM $$");
    return !(available > 0 && exited != -1 && WIFEXITED(exited) && WEXITSTATUS(exited) == 37 && external == 0 &&
             missing != -1 && WIFEXITED(missing) && WEXITSTATUS(missing) == 127 &&
             signalled != -1 && WIFSIGNALED(signalled) && WTERMSIG(signalled) == SIGTERM);
}
