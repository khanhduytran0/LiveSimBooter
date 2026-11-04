// Resolve a user on both sides of a real fork. A stale resolver mutex would leave the child stuck in its second lookup.
#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc > 2) return 64;
    const char *name = argc == 2 ? argv[1] : "mobile";
    struct passwd *entry = getpwnam(name);
    if (!entry) { fprintf(stderr, "could not resolve %s before fork\n", name); return 1; }
    uid_t uid = entry->pw_uid;
    pid_t child = fork();
    if (child < 0) { perror("fork"); return 1; }
    if (!child) {
        alarm(6);
        entry = getpwnam(name);
        _exit(entry && entry->pw_uid == uid ? 37 : 43);
    }
    int status;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited != child) { perror("waitpid"); return 1; }
    printf("resolver %s uid=%u child=%d wait=%#x\n", name, uid, child, status);
    return WIFEXITED(status) && WEXITSTATUS(status) == 37 ? 0 : 1;
}
