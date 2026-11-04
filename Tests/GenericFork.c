// Exercise fork from a separately loaded dylib. Neither image links to the guest shim.
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int testForkInLibrary(const char *executable, int shared, int closeOnExec, bool useVfork);

static int runAfterExec(int shared, int closeOnExec) {
    struct stat retained, other;
    if (fstat(shared, &retained)) return 51;
    if (!fstat(closeOnExec, &other)) {
        // The runtime can reuse the descriptor number during startup.
        if (other.st_dev == retained.st_dev && other.st_ino == retained.st_ino) return 52;
    } else if (errno != EBADF) return 53;
    char byte;
    return read(shared, &byte, 1) == 1 && byte == 'B' ? 37 : 54;
}

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "--after-exec")) return runAfterExec(atoi(argv[2]), atoi(argv[3]));
    bool useVfork = argc == 2 && !strcmp(argv[1], "--vfork");
    if ((argc != 1 && !useVfork) || !strchr(argv[0], '/')) {
        fprintf(stderr, "invoke the fixture by path so execve can open the same executable\n");
        return 64;
    }

    FILE *temporary = tmpfile();
    if (!temporary) { perror("tmpfile"); return 1; }
    int shared = dup(fileno(temporary));
    fclose(temporary);
    if (shared < 0) { perror("dup"); return 1; }
    if (write(shared, "ABC", 3) != 3 || lseek(shared, 0, SEEK_SET) != 0) { perror("prepare file"); return 1; }
    int closeOnExec = fcntl(shared, F_DUPFD_CLOEXEC, 10);
    if (closeOnExec < 0) { perror("F_DUPFD_CLOEXEC"); return 1; }
    return testForkInLibrary(argv[0], shared, closeOnExec, useVfork);
}
