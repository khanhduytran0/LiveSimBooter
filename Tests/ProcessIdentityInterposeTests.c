#include "../Guest/libLiveSimSpawn/ProcessIdentity.h"
#include <assert.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <os/log.h>
#include <stdio.h>
#include <unistd.h>
#include <xpc/xpc.h>
extern xpc_object_t xpc_copy_entitlements_for_self(void);
int main(int argc, const char *argv[]) {
    assert(argc == 1 || argc == 3);
    assert(getpid() == 2147483000 && LSBGuestPID() == 2147483000 && LSBNativePID() != 2147483000);
    audit_token_t token = {{0}};
    mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
    assert(!task_info(mach_task_self(), TASK_AUDIT_TOKEN, (task_info_t)&token, &count));
    assert(token.val[5] == (unsigned)LSBNativePID());
    if (argc == 3) {
        void *baseBoard = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        void *unrelated = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
        assert(baseBoard && unrelated);
        pid_t (*baseBoardPID)(void) = dlsym(baseBoard, "LSBIdentityFixturePID");
        pid_t (*unrelatedPID)(void) = dlsym(unrelated, "LSBIdentityFixturePID");
        int (*nativeIsSelf)(pid_t) = dlsym(baseBoard, "LSBIdentityFixtureIsSelf");
        int (*unrelatedIsSelf)(pid_t) = dlsym(unrelated, "LSBIdentityFixtureIsSelf");
        assert(baseBoardPID && unrelatedPID && baseBoardPID != unrelatedPID);
        assert(nativeIsSelf && unrelatedIsSelf);
        assert(baseBoardPID() == (pid_t)token.val[5]); // BSGetVersionedPID's kernel audit PID comparison must agree.
        assert(nativeIsSelf((pid_t)token.val[5]) && !nativeIsSelf(2147483000));
        assert(!unrelatedIsSelf((pid_t)token.val[5]) && unrelatedIsSelf(2147483000));
        assert(unrelatedPID() == 2147483000 && getpid() == 2147483000);
        assert(!dlclose(baseBoard) && !dlclose(unrelated));
        assert(getpid() == 2147483000);
    }
    mach_port_t taskName = MACH_PORT_NULL;
    int nativePID = 0;
    assert(task_name_for_pid(mach_task_self(), getpid(), &taskName) == KERN_SUCCESS && MACH_PORT_VALID(taskName));
    assert(pid_for_task(taskName, &nativePID) == KERN_SUCCESS && nativePID == LSBNativePID());
    assert(mach_port_deallocate(mach_task_self(), taskName) == KERN_SUCCESS);
    xpc_object_t entitlements = xpc_copy_entitlements_for_self();
    if (entitlements) xpc_release(entitlements);
    os_log_t log = os_log_create("com.kdt.LiveSimBooter.identity-smoke", "self");
    assert(log);
    os_release(log);
    puts("Identity smoke: guest getpid, native task-name right, unchanged audit token, libxpc self lookup and firehose passed");
}
