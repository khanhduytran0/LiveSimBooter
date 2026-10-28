#include "DyldSimLoader.h"
#include "SpawnTransport.h"
#include "PrivateXPC.h"
#include "ChildStatus.h"
#include "NativeProbeLoader.h"
#include "ExecutablePath.h"
#include "ProcessDisplayName.h"
#include "NativeRuntimePolicy.h"
#include "../NativeMetal/Backend.h"
#include <errno.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static LSBBootstrapReply launch;
static LSBMappedImage loader, executable;
static LSBChildStatus *childStatus;

static void closeStatus(void) {
    LSBSpawnPortsDispose(&launch.ports);
    if (MACH_PORT_VALID(launch.launchdTankPort)) mach_port_deallocate(mach_task_self(), launch.launchdTankPort);
    launch.launchdTankPort = MACH_PORT_NULL;
    if (launch.statusFD >= 0) close(launch.statusFD);
    if (launch.wakeFD >= 0) close(launch.wakeFD);
    launch.statusFD = launch.wakeFD = -1;
    if (childStatus) munmap(childStatus, LSB_CHILD_STATUS_MAP_SIZE);
    childStatus = NULL;
}

static int prepareStatus(void) {
    void *mapping = mmap(NULL, LSB_CHILD_STATUS_MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, launch.statusFD, 0);
    if (mapping == MAP_FAILED) return errno;
    childStatus = mapping;
    close(launch.statusFD);
    launch.statusFD = -1;
    int error = LSBChildStatusValidate(childStatus);
    if (error) return error;
    int highest = 2;
    for (unsigned i = 0; i < launch.plan.fdCount; ++i) if (launch.plan.fds[i].number > highest) highest = launch.plan.fds[i].number;
    for (unsigned i = 0; i < launch.plan.actionCount; ++i) {
        const LSBFileAction *action = &launch.plan.actions[i];
        if (action->kind != LSB_ACTION_CHDIR && action->fd > highest) highest = action->fd;
        if (action->kind == LSB_ACTION_DUP2 && action->target > highest) highest = action->target;
    }
    // Keep the private wake endpoint out of the guest's ordered file-action destinations and both close sweeps.
    int staged = fcntl(launch.wakeFD, F_DUPFD_CLOEXEC, highest + 1);
    if (staged < 0) return errno;
    close(launch.wakeFD);
    launch.wakeFD = staged;
    return 0;
}

static int resolveExecutable(char output[PATH_MAX]) {
    return LSBResolveGuestExecutable(launch.runtimeRoot, launch.plan.strings + launch.plan.path, output);
}

static int runNativeProbe(int error) {
    LSBNativeProbe probe = {0};
    char detail[1024] = {0};
    mach_port_t broker = MACH_PORT_NULL;
    int portError = LSBTransportCopyBrokerPort(&broker);
    if (!error) error = portError;
    if (!error) error = LSBNativeProbePrepare(&probe, detail, sizeof(detail));
    // Native framework initialization may create workers. This role keeps the native cache and never performs a simulator handoff.
    int readyError = MACH_PORT_VALID(broker) ? LSBTransportReadyOnPort(broker, launch.requestID, error) :
        LSBTransportReady(launch.requestID, error);
    if (MACH_PORT_VALID(broker)) mach_port_deallocate(mach_task_self(), broker);
    closeStatus();
    if (error || readyError) {
        fprintf(stderr, "[LiveProcess] Native probe preparation failed: %s %s\n", strerror(error ? error : readyError), detail);
        return 1;
    }
    fprintf(stderr, "[LiveProcess] pid=%d native Metal probe folder=%s; host cache retained\n", getpid(), probe.folder);
    fflush(stderr);
    error = LSBNativeProbeRun(&probe);
    fprintf(stderr, "[LiveProcess] Native Metal probe result=%d (%s)\n", error, strerror(error));
    fflush(stderr);
    return error ? 1 : 0;
}

static int runNativeMetalService(int error, int planApplied) {
    LSBNativeProbe backend = {0};
    char detail[1024] = {0};
    mach_port_t broker = MACH_PORT_NULL, registration = MACH_PORT_NULL;
    int portError = LSBTransportCopyBrokerPort(&broker);
    if (!error) error = portError;
    if (!error) error = LSBNativeProbePrepare(&backend, detail, sizeof(detail));
    LSBMetalServicePrepare prepare = NULL;
    LSBMetalServiceRun run = NULL;
    if (!error) {
        prepare = (LSBMetalServicePrepare)dlsym(backend.handle, "LSBMetalNativeServicePrepare");
        run = (LSBMetalServiceRun)dlsym(backend.handle, "LSBMetalNativeServiceRun");
        Dl_info expected = {0}, prepareImage = {0}, runImage = {0};
        if (!prepare || !run || !dladdr((void *)backend.entry, &expected) || !dladdr((void *)prepare, &prepareImage) ||
            !dladdr((void *)run, &runImage) || expected.dli_fbase != prepareImage.dli_fbase || expected.dli_fbase != runImage.dli_fbase)
            error = ENOEXEC;
    }
    // fd3 is installed only by the broker's fixed native-service plan, never selected by a guest argument or environment.
    if (!error) error = prepare(backend.folder, 3, &registration);
    if (planApplied) close(3);
    if (error && MACH_PORT_VALID(registration)) { mach_port_deallocate(mach_task_self(), registration); registration = MACH_PORT_NULL; }
    int readyError = MACH_PORT_VALID(broker) ? LSBTransportReadyNativeServiceOnPort(broker, launch.requestID, error, registration) :
        LSBTransportReady(launch.requestID, error ? error : EIO);
    if (MACH_PORT_VALID(registration)) mach_port_deallocate(mach_task_self(), registration);
    if (MACH_PORT_VALID(broker)) mach_port_deallocate(mach_task_self(), broker);
    closeStatus();
    if (error || readyError) {
        fprintf(stderr, "[LiveProcess] Native Metal service preparation failed: %s %s\n", strerror(error ? error : readyError), detail);
        return 1;
    }
    fprintf(stderr, "[LiveProcess] pid=%d %s ready; host cache retained\n", getpid(), launch.plan.strings + launch.plan.argv[0]);
    fflush(stderr);
    run();
    return 1; // The service owns the process until the broker terminates it.
}

int NSExtensionMain(int argc, const char *argv[], const char *envp[], const char *apple[]) {
    (void)envp;
    char *nativeArgv0 = argc > 0 && argv && argv[0] ? (char *)argv[0] : NULL;
    size_t nativeArgv0Capacity = nativeArgv0 ? strlen(nativeArgv0) + 1 : 0;
    // Cancel launchd's extension-alive timer through its C API before any broker retry.
    launch_extension_check_in_live_4UIKit();
    int error = LSBRequireSingleThread();
    if (error) { fprintf(stderr, "[LiveProcess] Native worker at startup: %s\n", strerror(error)); return 1; }
    int received[LSB_MAX_FDS];
    for (unsigned i = 0; i < LSB_MAX_FDS; ++i) received[i] = -1;
    error = EAGAIN;
    // PlugInKit can launch our C entry before the host-side connection exposes its PID.
    for (unsigned attempt = 0; attempt < 100 && error == EAGAIN; ++attempt) {
        error = LSBTransportCheckIn(&launch, received);
        if (error == EAGAIN) usleep(100000);
    }
    if (error) { fprintf(stderr, "[LiveProcess] Check-in failed: %s\n", strerror(error)); return 1; }
    const char *displayName = launch.plan.strings + (launch.executionRole == LSB_EXECUTION_NATIVE_METAL_SERVICE ?
        launch.plan.argv[0] : launch.plan.path);
    int nameError = LSBSetProcessDisplayName(displayName, nativeArgv0, nativeArgv0Capacity);
    if (nameError) fprintf(stderr, "[LiveProcess] Could not set native process name: %s\n", strerror(nameError));
    error = prepareStatus();
    if (!error) error = LSBApplySpawnPlanPreservingFD(&launch.plan, received, launch.wakeFD);
    int planApplied = !error;
    for (unsigned i = 0; i < LSB_MAX_FDS; ++i) if (received[i] >= 0) { close(received[i]); received[i] = -1; }
    // Only the authenticated broker bootstrap can select this role; no environment value or executable path is consulted.
    if (launch.executionRole == LSB_EXECUTION_NATIVE_METAL_PROBE) return runNativeProbe(error);
    if (launch.executionRole == LSB_EXECUTION_NATIVE_METAL_SERVICE) return runNativeMetalService(error, planApplied);
    // The native host receives a kernel-created movable task right through this one-shot thread exception.
    // Restore the thread's prior handlers before simulator TLS/workqueue initialization; never send mach_task_self() over XPC.
    if (!error) error = MACH_PORT_VALID(launch.taskCapture.port) ? LSBTaskPortCapturePerform(&launch.taskCapture) : EPROTO;
    if (MACH_PORT_VALID(launch.taskCapture.port)) mach_port_deallocate(mach_task_self(), launch.taskCapture.port);
    memset(&launch.taskCapture, 0, sizeof(launch.taskCapture));
    const char *policyEnvironment[LSB_MAX_VECTOR + 1];
    for (unsigned index = 0; index < launch.plan.envc; ++index) policyEnvironment[index] = launch.plan.strings + launch.plan.envp[index];
    policyEnvironment[launch.plan.envc] = NULL;
    LSBNativeRuntimePolicy nativePolicy = {0};
    if (!error) error = LSBResolveNativeRuntimePolicy(launch.nativeProductionStatus, policyEnvironment, &nativePolicy);
    char executablePath[PATH_MAX], dyldPath[PATH_MAX], insertLibrary[PATH_MAX + 32], brokerEnvironment[64], detail[1024] = {0};
    char capabilitiesEnvironment[PATH_MAX + sizeof("SIMULATOR_CAPABILITIES=")];
    char statusEnvironment[64], wakeEnvironment[64];
    char guestPIDEnvironment[48], parentPIDEnvironment[48];
    char workqueueStartEnvironment[80], workqueueFeaturesEnvironment[80];
    mach_port_t brokerPort = MACH_PORT_NULL;
    if (!error) error = resolveExecutable(executablePath);
    int length = snprintf(dyldPath, sizeof(dyldPath), "%s/usr/lib/dyld_sim", launch.runtimeRoot);
    if (!error && (length < 0 || (size_t)length >= sizeof(dyldPath))) error = ENAMETOOLONG;
    length = snprintf(insertLibrary, sizeof(insertLibrary), "DYLD_INSERT_LIBRARIES=%s/usr/lib/libLiveSimSpawn.dylib", launch.runtimeRoot);
    if (!error && (length < 0 || (size_t)length >= sizeof(insertLibrary))) error = ENAMETOOLONG;
    length = snprintf(capabilitiesEnvironment, sizeof(capabilitiesEnvironment),
                      "SIMULATOR_CAPABILITIES=%s/Library/Developer/CoreSimulator/DeviceType/capabilities.plist", launch.runtimeRoot);
    if (!error && (length < 0 || (size_t)length >= sizeof(capabilitiesEnvironment))) error = ENAMETOOLONG;
    if (!error && !LSBMapDyldSimForRuntime(dyldPath, launch.runtimeRoot, &loader, detail, sizeof(detail))) error = ENOEXEC;
    if (!error && !LSBMapSimExecutableForRuntime(executablePath, launch.runtimeRoot, &executable, detail, sizeof(detail))) error = ENOEXEC;
    // Keep only a kernel send right across detachment; simulator libxpc cannot look up services without simulator launchd.
    if (!error) error = LSBTransportCopyBrokerPort(&brokerPort);
    if (!error) {
        uintptr_t workqueueStart = (uintptr_t)dlsym(RTLD_DEFAULT, "start_wqthread");
        int (*features)(void) = (int (*)(void))dlsym(RTLD_DEFAULT, "_pthread_workqueue_supported");
        if (!workqueueStart || !features) error = ENOTSUP;
        else {
            // Retain only numeric metadata; guest code never calls these native-cache function pointers after detachment.
            __asm__("xpaci %0" : "+r"(workqueueStart));
            snprintf(workqueueStartEnvironment, sizeof(workqueueStartEnvironment), "LSB_HOST_WORKQUEUE_START=0x%llx",
                     (unsigned long long)workqueueStart);
            snprintf(workqueueFeaturesEnvironment, sizeof(workqueueFeaturesEnvironment), "LSB_HOST_WORKQUEUE_FEATURES=0x%x", features());
        }
    }
    snprintf(brokerEnvironment, sizeof(brokerEnvironment), "LSB_BROKER_PORT=%u", brokerPort);
    snprintf(statusEnvironment, sizeof(statusEnvironment), LSB_CHILD_STATUS_ENV "=0x%llx", (unsigned long long)(uintptr_t)childStatus);
    snprintf(wakeEnvironment, sizeof(wakeEnvironment), LSB_CHILD_WAKE_FD_ENV "=%d", launch.wakeFD);
    snprintf(guestPIDEnvironment, sizeof(guestPIDEnvironment), "LSB_GUEST_PID=%d", launch.guestPID);
    snprintf(parentPIDEnvironment, sizeof(parentPIDEnvironment), "LSB_GUEST_PPID=%d", launch.parentPID);
    // Complete fallible Mach state setup before READY can commit an exec. Readiness uses the retained broker right directly.
    kern_return_t portsError = KERN_SUCCESS;
    if (!error && launch.ports.registeredPresent)
        portsError = mach_ports_register(mach_task_self(), launch.ports.registered, launch.ports.count);
    if (!error && !portsError && launch.ports.bootstrapPresent)
        portsError = task_set_bootstrap_port(mach_task_self(), launch.ports.bootstrapPort);
    if (!error && !portsError && MACH_PORT_VALID(launch.launchdTankPort))
        portsError = task_set_bootstrap_port(mach_task_self(), launch.launchdTankPort);
    LSBSpawnPortsDispose(&launch.ports);
    if (MACH_PORT_VALID(launch.launchdTankPort)) mach_port_deallocate(mach_task_self(), launch.launchdTankPort);
    launch.launchdTankPort = MACH_PORT_NULL;
    if (portsError && !error) {
        fprintf(stderr, "[LiveProcess] Cannot install simulator Mach ports: %d\n", portsError);
        error = EIO;
    }
    if (!error) error = LSBRequireSingleThread();
    int readyError = MACH_PORT_VALID(brokerPort) ? LSBTransportReadyOnPort(brokerPort, launch.requestID, error) :
        LSBTransportReady(launch.requestID, error);
    if (error || readyError) {
        if (MACH_PORT_VALID(brokerPort)) mach_port_deallocate(mach_task_self(), brokerPort);
        closeStatus();
        fprintf(stderr, "[LiveProcess] Preparation failed: %s %s\n", strerror(error ? error : readyError), detail);
        return 1;
    }
    // All native XPC objects have been released. No native dispatch queue has been created.
    if ((error = LSBRequireSingleThread())) {
        mach_port_deallocate(mach_task_self(), brokerPort);
        closeStatus();
        fprintf(stderr, "[LiveProcess] Native worker appeared before handoff: %s\n", strerror(error)); return 1;
    }
    const char *arguments[LSB_MAX_VECTOR + 1], *environment[LSB_MAX_VECTOR + 15];
    for (unsigned i = 0; i < launch.plan.argc; ++i) arguments[i] = launch.plan.strings + launch.plan.argv[i];
    arguments[launch.plan.argc] = NULL;
    unsigned count = 0;
    int stopBeforeHandoff = 0;
    for (unsigned i = 0; i < launch.plan.envc; ++i) {
        const char *entry = launch.plan.strings + launch.plan.envp[i];
        const size_t debugPrefixLength = sizeof("LSB_DEBUG_STOP_BEFORE_HANDOFF=") - 1;
        if (!strncmp(entry, "LSB_DEBUG_STOP_BEFORE_HANDOFF=", debugPrefixLength)) {
            stopBeforeHandoff = !strcmp(entry + debugPrefixLength, "1");
            continue;
        }
        if (strncmp(entry, "DYLD_INSERT_LIBRARIES=", 22) && strncmp(entry, "LSB_BROKER_PORT=", 16) &&
            strncmp(entry, "RB_USE_BINARY_ARCHIVE=", sizeof("RB_USE_BINARY_ARCHIVE=") - 1) &&
            strncmp(entry, "LSB_SIMULATE_FRAMEWORK_BUNDLES=", sizeof("LSB_SIMULATE_FRAMEWORK_BUNDLES=") - 1) &&
            strncmp(entry, "LSB_REGISTER_METAL_COMPILER=", sizeof("LSB_REGISTER_METAL_COMPILER=") - 1) &&
            strncmp(entry, "SIMULATOR_CAPABILITIES=", sizeof("SIMULATOR_CAPABILITIES=") - 1) &&
            strncmp(entry, "LSB_GUEST_PID=", sizeof("LSB_GUEST_PID=") - 1) &&
            strncmp(entry, "LSB_GUEST_PPID=", sizeof("LSB_GUEST_PPID=") - 1) &&
            strncmp(entry, "LSB_HOST_WORKQUEUE_START=", sizeof("LSB_HOST_WORKQUEUE_START=") - 1) &&
            strncmp(entry, "LSB_HOST_WORKQUEUE_FEATURES=", sizeof("LSB_HOST_WORKQUEUE_FEATURES=") - 1) &&
            strncmp(entry, LSB_CHILD_STATUS_ENV "=", sizeof(LSB_CHILD_STATUS_ENV)) &&
            strncmp(entry, LSB_CHILD_WAKE_FD_ENV "=", sizeof(LSB_CHILD_WAKE_FD_ENV))) environment[count++] = entry;
    }
    // RenderBox binary archives require targetDeviceArchitecture, which stock MTLSimDevice does not implement.
    // Apply to every guest, including spawned apps with a replacement environment; native Metal helpers return above.
    environment[count++] = "RB_USE_BINARY_ARCHIVE=0";
    // Cached install paths must resolve resources inside RuntimeRoot, including apps that supply a replacement environment.
    environment[count++] = "LSB_SIMULATE_FRAMEWORK_BUNDLES=1";
    // Apps need the physical compiler bundle imported into their own PID domain; the guest hook validates the executable role.
    environment[count++] = "LSB_REGISTER_METAL_COMPILER=1";
    // MobileGestalt reads this profile directly. A missing optional profile must not prevent command-line guests from starting.
    environment[count++] = capabilitiesEnvironment;
    environment[count++] = insertLibrary;
    environment[count++] = brokerEnvironment;
    environment[count++] = statusEnvironment;
    environment[count++] = wakeEnvironment;
    environment[count++] = workqueueStartEnvironment;
    environment[count++] = workqueueFeaturesEnvironment;
    environment[count++] = guestPIDEnvironment;
    environment[count++] = parentPIDEnvironment;
    environment[count] = NULL;
    // Direct probes and children with a replacement environment also need the native policy before simulator initialization.
    int policyError = LSBAppendNativeRuntimePolicy(environment, &count, sizeof(environment) / sizeof(*environment), nativePolicy);
    if (policyError) {
        mach_port_deallocate(mach_task_self(), brokerPort);
        closeStatus();
        fprintf(stderr, "[LiveProcess] Could not apply native runtime defaults: %s\n", strerror(policyError));
        return 1;
    }
    if (LSBTraceHandoffRequested(environment))
        fprintf(stderr, "[LiveProcess] pid=%d entering %s with %u arguments\n", getpid(), executablePath, launch.plan.argc);
    if (stopBeforeHandoff) {
        fprintf(stderr, "[LiveProcess] pid=%d waiting for LLDB before simulator handoff\n", getpid());
        fflush(stderr);
        kill(getpid(), SIGSTOP);
    }
    int result = LSBEnterDyldSim(&loader, executable.header, executablePath, dyldPath, launch.runtimeRoot,
                               (int)launch.plan.argc, arguments, environment, apple);
    // Successful detachment never returns; only native preparation/handoff failures reach this cleanup.
    mach_port_deallocate(mach_task_self(), brokerPort);
    closeStatus();
    return result;
}
