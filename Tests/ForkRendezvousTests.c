#include "../ProcessSupport/ForkRendezvous.h"
#include <assert.h>
#include <limits.h>
#include <mach/mach.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static audit_token_t ownAudit(void) {
    audit_token_t token = {{0}};
    mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
    assert(!task_info(mach_task_self(), TASK_AUDIT_TOKEN, (task_info_t)&token, &count) && count == TASK_AUDIT_TOKEN_COUNT);
    return token;
}
int main(void) {
    char group[] = "/tmp/lsbf.XXXXXX";
    assert(mkdtemp(group));
    char store[PATH_MAX], root[PATH_MAX];
    assert(snprintf(store, sizeof(store), "%s/LSB", group) > 0 && !mkdir(store, 0700));
    assert(snprintf(root, sizeof(root), "%s/" LSB_STORAGE_BUILD_RELATIVE_PATH "/" LSB_RUNTIME_DIRECTORY, group) > 0);
    struct sockaddr_un address;
    audit_token_t server = ownAudit();
    assert(LSBForkSocketAddress(group, &server, &address) == EINVAL);
    assert(!LSBForkSocketAddress(root, &server, &address));
    int fd = socket(AF_UNIX, SOCK_STREAM, 0), channel[2];
    assert(fd >= 0 && !bind(fd, (struct sockaddr *)&address, address.sun_len) && !listen(fd, 1) && !pipe(channel));
    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid) {
        close(fd);
        close(channel[0]);
        int peer = socket(AF_UNIX, SOCK_STREAM, 0);
        audit_token_t child = ownAudit();
        assert(peer >= 0 && !connect(peer, (struct sockaddr *)&address, address.sun_len));
        assert(!LSBForkCheckSocketPeer(peer, &server));
        assert(write(channel[1], &child, sizeof(child)) == sizeof(child));
        char ack;
        assert(read(peer, &ack, 1) == 1 && ack == 'x');
        close(peer);
        _exit(0);
    }
    close(channel[1]);
    int peer = accept(fd, NULL, NULL);
    audit_token_t child;
    assert(peer >= 0 && read(channel[0], &child, sizeof(child)) == sizeof(child) && child.val[5] == (uint32_t)pid);
    assert(!LSBForkCheckSocketPeer(peer, &child));
    ++child.val[7];
    assert(LSBForkCheckSocketPeer(peer, &child) == EACCES);
    assert(write(peer, "x", 1) == 1);
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid && !status);
    close(peer);
    close(channel[0]);
    close(fd);
    unlink(address.sun_path);
    rmdir(store);
    rmdir(group);
    puts("Fork rendezvous verified both kernel peer identities and rejected a stale audit token");
    return 0;
}
