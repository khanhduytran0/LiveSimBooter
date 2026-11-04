// The child accepts a pending fork offer without initializing its libxpc connection or starting a workqueue.
#pragma once
#include "RuntimeLayout.h"
#include <errno.h>
#include <mach/message.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>

static inline int LSBForkSocketAddress(const char *root, const audit_token_t *child, struct sockaddr_un *address) {
    static const char suffix[] = "/" LSB_STORAGE_BUILD_RELATIVE_PATH "/" LSB_RUNTIME_DIRECTORY;
    if (!root || !child || !address || !child->val[5] || !child->val[7]) return EINVAL;
    if (!strncmp(root, "/private/var/", 13)) root += 8;
    size_t n = strlen(root);
    if (n <= sizeof(suffix) - 1 || strcmp(root + n - sizeof(suffix) + 1, suffix)) return EINVAL;
    size_t groupLength = n - (sizeof(suffix) - 1);
    if (groupLength >= sizeof(address->sun_path)) return ENAMETOOLONG;
    memset(address, 0, sizeof(*address));
    address->sun_family = AF_UNIX;
    int length = snprintf(address->sun_path, sizeof(address->sun_path), "%.*s/LSB/.f-%x-%x", (int)groupLength, root, child->val[5], child->val[7]);
    if (length < 0 || (size_t)length >= sizeof(address->sun_path)) return ENAMETOOLONG;
    address->sun_len = (uint8_t)(offsetof(struct sockaddr_un, sun_path) + length + 1);
    return 0;
}

static inline int LSBForkCheckSocketPeer(int fd, const audit_token_t *expected) {
    audit_token_t token = {{0}};
    socklen_t n = sizeof(token);
    if (getsockopt(fd, SOL_LOCAL, LOCAL_PEERTOKEN, &token, &n)) return errno;
    return n == sizeof(token) && token.val[5] && token.val[7] && !memcmp(&token, expected, sizeof(token)) ? 0 : EACCES;
}
