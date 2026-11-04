#ifndef LSB_SPAWN_DIAGNOSTICS_H
#define LSB_SPAWN_DIAGNOSTICS_H

#ifdef LSB_TRANSPORT_TRACE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// launchd redirects stderr to /dev/null. Keep opt-in spawn failures in the isolated simulator log directory as well.
__attribute__((format(printf, 1, 2))) static inline void LSBSpawnTrace(const char *format, ...) {
    int saved = errno;
    va_list arguments;
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
    const char *root = getenv("SIMULATOR_LOG_ROOT");
    if (root && root[0] == '/') {
        char path[PATH_MAX];
        int count = snprintf(path, sizeof(path), "%s/spawn-rejections.%d.log", root, getpid());
        if (count > 0 && (size_t)count < sizeof(path)) {
            int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (fd >= 0) {
                va_start(arguments, format);
                vdprintf(fd, format, arguments);
                va_end(arguments);
                close(fd);
            }
        }
    }
    errno = saved;
}
#endif
#endif
