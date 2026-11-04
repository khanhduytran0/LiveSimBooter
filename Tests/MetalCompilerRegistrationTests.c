#include "../Guest/libLiveSimSpawn/MetalCompilerRegistration.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static atomic_uint registrations, originalCalls;
static atomic_bool importFinished;
static char registeredPath[PATH_MAX];
static uint64_t registeredFlags;
static bool reenterImport, holdImport, importEntered, releaseImport;
static pthread_mutex_t importLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t importCondition = PTHREAD_COND_INITIALIZER;
static _Thread_local bool insideImport;
static _Thread_local const char *expectedName;
static _Thread_local dispatch_queue_t expectedQueue;
static _Thread_local int expectedErrno;
static bool requireImport;

static void recordRegistration(const char *path, uint64_t flags);
static xpc_connection_t recordConnection(const char *name, dispatch_queue_t queue);
#define LSB_METAL_COMPILER_REGISTRATION_TESTING
#define xpc_add_bundle recordRegistration
#define xpc_connection_create recordConnection
#include "../Guest/libLiveSimSpawn/MetalCompilerRegistration.c"
#undef xpc_connection_create
#undef xpc_add_bundle

static xpc_connection_t connectionResult(void) { return (xpc_connection_t)(uintptr_t)0x12340; }

static void connectWith(const char *name, dispatch_queue_t queue, int originalErrno) {
    const char *previousName = expectedName;
    dispatch_queue_t previousQueue = expectedQueue;
    int previousErrno = expectedErrno;
    expectedName = name; expectedQueue = queue; expectedErrno = originalErrno;
    errno = originalErrno;
    assert(LSBMetalCompilerConnectionCreate(name, queue) == connectionResult());
    assert(errno == EOWNERDEAD); // The original API, not the registration path, owns the resulting errno.
    expectedName = previousName; expectedQueue = previousQueue; expectedErrno = previousErrno;
}

static xpc_connection_t recordConnection(const char *name, dispatch_queue_t queue) {
    assert(name == expectedName && queue == expectedQueue && errno == expectedErrno);
    if (requireImport && name && !strcmp(name, compilerService) && !insideImport) assert(atomic_load(&importFinished));
    atomic_fetch_add(&originalCalls, 1);
    errno = EOWNERDEAD;
    return connectionResult();
}

static void recordRegistration(const char *path, uint64_t flags) {
    assert(!insideImport);
    insideImport = true;
    atomic_fetch_add(&registrations, 1);
    registeredFlags = flags;
    assert(strlen(path) < sizeof(registeredPath));
    strcpy(registeredPath, path);
    if (reenterImport) {
        connectWith(compilerService, (dispatch_queue_t)(uintptr_t)0x34560, EINTR);
        connectWith("com.example.other", NULL, EBUSY);
    }
    if (holdImport) {
        assert(!pthread_mutex_lock(&importLock));
        importEntered = true;
        assert(!pthread_cond_broadcast(&importCondition));
        while (!releaseImport) assert(!pthread_cond_wait(&importCondition, &importLock));
        assert(!pthread_mutex_unlock(&importLock));
    }
    atomic_store(&importFinished, true);
    insideImport = false;
    errno = ENOSPC; // Must not leak into xpc_connection_create.
}

static void resetPolicy(void) {
    atomic_store(&compilerPolicyReady, false);
    compilerRuntimeRoot[0] = 0;
    compilerRegistrationOnce = (pthread_once_t)PTHREAD_ONCE_INIT;
    registeringCompiler = false;
    atomic_store(&registrations, 0);
    atomic_store(&originalCalls, 0);
    atomic_store(&importFinished, false);
    registeredPath[0] = 0; registeredFlags = 0;
    reenterImport = holdImport = importEntered = releaseImport = requireImport = false;
}

static void join(char output[PATH_MAX], const char *parent, const char *suffix) { assert(!appendPath(output, parent, suffix)); }

static void makeDirectory(const char *path) {
    char copy[PATH_MAX];
    assert(strlen(path) < sizeof(copy));
    strcpy(copy, path);
    for (char *separator = copy + 1; *separator; ++separator) {
        if (*separator != '/') continue;
        *separator = 0;
        assert(!mkdir(copy, 0700) || errno == EEXIST);
        *separator = '/';
    }
    assert(!mkdir(copy, 0700) || errno == EEXIST);
}

static void makeFile(const char *path) {
    int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0700);
    assert(descriptor >= 0 && write(descriptor, "fixture", 7) == 7 && !close(descriptor));
}

static void removeTree(char *path) {
    char *paths[] = {path, NULL};
    FTS *tree = fts_open(paths, FTS_PHYSICAL | FTS_NOCHDIR, NULL);
    assert(tree);
    FTSENT *entry;
    while ((entry = fts_read(tree))) {
        if (entry->fts_info == FTS_D) continue;
        assert(entry->fts_info != FTS_ERR && entry->fts_info != FTS_DNR && entry->fts_info != FTS_NS);
        assert(!(entry->fts_info == FTS_DP ? rmdir(entry->fts_accpath) : unlink(entry->fts_accpath)));
    }
    assert(!fts_close(tree));
}

static void expectScope(const char *enabled, const char *root, const char *simulatorRoot, const char *executable,
                        uint32_t platform, int error, bool ready) {
    resetPolicy();
    assert(configureCompilerRegistration(enabled, root, simulatorRoot, executable, platform) == error);
    assert(atomic_load(&compilerPolicyReady) == ready && !atomic_load(&registrations));
    if (!ready) {
        connectWith(compilerService, NULL, ECHILD);
        assert(!atomic_load(&registrations));
    }
}

static void expectImport(const char *root, const char *executable, unsigned expectedRegistrations) {
    expectScope("1", root, root, executable, 7, 0, true);
    requireImport = expectedRegistrations != 0;
    connectWith(compilerService, NULL, ECHILD);
    connectWith(compilerService, (dispatch_queue_t)(uintptr_t)0x45670, ERANGE);
    assert(atomic_load(&registrations) == expectedRegistrations && atomic_load(&originalCalls) == 2);
    if (expectedRegistrations) assert(registeredFlags == 2);
}

static void *concurrentConnection(void *context) {
    connectWith(compilerService, (dispatch_queue_t)context, EDOM);
    return NULL;
}

int main(void) {
    char temporary[] = "/tmp/LiveSimBooter-metal-compiler.XXXXXX", canonical[PATH_MAX];
    assert(mkdtemp(temporary) && realpath(temporary, canonical));
    char root[PATH_MAX], directory[PATH_MAX], executable[PATH_MAX], bundle[PATH_MAX], info[PATH_MAX], compiler[PATH_MAX];
    join(root, canonical, "RuntimeRoot");
    join(directory, root, "usr/libexec"); makeDirectory(directory);
    join(executable, directory, "UnlistedMetalClient"); makeFile(executable);
    join(bundle, root, compilerSuffix); makeDirectory(bundle);
    join(info, bundle, "Info.plist"); makeFile(info);
    join(compiler, bundle, "MTLCompilerService"); makeFile(compiler);

    // Pre-constructor calls do not even inspect a service-name pointer; native/original forwarding owns its validation.
    resetPolicy();
    connectWith((const char *)(uintptr_t)1, (dispatch_queue_t)(uintptr_t)0x22220, EBUSY);
    connectWith(NULL, NULL, EINVAL);
    assert(!atomic_load(&registrations));
    const char *disabled[] = {NULL, "", "0", "true", "01", "1 "};
    for (unsigned i = 0; i < sizeof(disabled) / sizeof(*disabled); ++i) expectScope(disabled[i], NULL, NULL, NULL, 7, 0, false);
    for (unsigned platform = 0; platform <= 12; ++platform)
        if (platform != 7) expectScope("1", NULL, NULL, NULL, platform, 0, false);
    expectScope("1", NULL, NULL, executable, 7, EINVAL, false);
    expectScope("1", "/", "/", executable, 7, EINVAL, false);
    expectScope("1", "RuntimeRoot", "RuntimeRoot", executable, 7, EINVAL, false);
    expectScope("1", root, NULL, executable, 7, EINVAL, false);
    expectScope("1", root, canonical, executable, 7, EINVAL, false);
    expectScope("1", root, root, "relative", 7, EINVAL, false);
    expectScope("1", root, root, NULL, 7, EINVAL, false);
    expectScope("1", executable, executable, executable, 7, ENOTDIR, false);
    expectScope("1", root, root, directory, 7, EINVAL, false);

    // No process whitelist or plist parsing: every actual contained guest caller can request the fixed compiler service.
    const char *clients[] = {"sbin/launchd_sim", "usr/libexec/xpcproxy_sim", "usr/libexec/backboardd", "bin/df",
        "System/Library/CoreServices/SpringBoard.app/SpringBoard", "System/Library/CoreServices/iconservicesagent",
        "System/Library/ExtensionKit/Extensions/GPUExtension.appex/com.apple.WebKit.GPU",
        "Applications/NoInfo.app/PlugIns/Widget.appex/Widget", "Applications/NoInfo.app/XPCServices/Worker.xpc/Worker"};
    for (unsigned i = 0; i < sizeof(clients) / sizeof(*clients); ++i) {
        char path[PATH_MAX], parent[PATH_MAX];
        join(path, root, clients[i]); strcpy(parent, path); *strrchr(parent, '/') = 0; makeDirectory(parent); makeFile(path);
        expectImport(root, path, 1);
        assert(!strcmp(registeredPath, bundle));
    }
    char installed[PATH_MAX], parent[PATH_MAX];
    join(parent, canonical, "data/Containers/Bundle/Application/fixture/App.app/PlugIns/Widget.appex"); makeDirectory(parent);
    join(installed, parent, "Widget"); makeFile(installed);
    expectImport(root, installed, 1);

    char outside[PATH_MAX], outsideFile[PATH_MAX], candidate[PATH_MAX], alias[PATH_MAX];
    join(outside, canonical, "RuntimeRoot-other"); makeDirectory(outside);
    join(outsideFile, outside, "NativeHelper"); makeFile(outsideFile);
    expectScope("1", root, root, outsideFile, 7, EPERM, false);
    join(parent, canonical, "data/Containers/Data/Application/fixture"); makeDirectory(parent);
    join(candidate, parent, "NotAnInstalledExecutable"); makeFile(candidate);
    expectScope("1", root, root, candidate, 7, EPERM, false);
    assert(!unlink(executable) && !symlink(outsideFile, executable));
    expectScope("1", root, root, executable, 7, EPERM, false);
    assert(!unlink(executable)); makeFile(executable);
    join(alias, canonical, "runtime-alias"); assert(!symlink(root, alias));
    join(candidate, alias, "usr/libexec/UnlistedMetalClient");
    expectImport(alias, candidate, 1);
    assert(!strcmp(registeredPath, bundle));

    // Configuration and nonmatching connections never import; the first exact request imports before native connection creation.
    expectScope("1", root, root, executable, 7, 0, true);
    const char *otherServices[] = {NULL, "", "com.apple.MTLCompilerService.other", "com.apple.mtlcompilerservice", "com.example.service"};
    for (unsigned i = 0; i < sizeof(otherServices) / sizeof(*otherServices); ++i) connectWith(otherServices[i], NULL, EIO);
    assert(!atomic_load(&registrations));
    requireImport = true;
    connectWith(compilerService, (dispatch_queue_t)(uintptr_t)0x12300, EAGAIN);
    connectWith(compilerService, NULL, E2BIG);
    assert(atomic_load(&registrations) == 1 && atomic_load(&originalCalls) == 7);
    assert(!strcmp(registeredPath, bundle) && registeredFlags == 2);

    // Reentrant import requests forward without deadlocking/repeating registration. Concurrent callers wait for the first import.
    expectScope("1", root, root, executable, 7, 0, true);
    requireImport = reenterImport = holdImport = true;
    pthread_t workers[8];
    assert(!pthread_create(&workers[0], NULL, concurrentConnection, NULL));
    assert(!pthread_mutex_lock(&importLock));
    while (!importEntered) assert(!pthread_cond_wait(&importCondition, &importLock));
    assert(!pthread_mutex_unlock(&importLock));
    for (unsigned i = 1; i < 8; ++i) assert(!pthread_create(&workers[i], NULL, concurrentConnection, (void *)(uintptr_t)(i * 16)));
    connectWith("com.example.concurrent", NULL, ESRCH);
    assert(atomic_load(&registrations) == 1 && atomic_load(&originalCalls) == 3);
    assert(!pthread_mutex_lock(&importLock));
    releaseImport = true;
    assert(!pthread_cond_broadcast(&importCondition) && !pthread_mutex_unlock(&importLock));
    for (unsigned i = 0; i < 8; ++i) assert(!pthread_join(workers[i], NULL));
    assert(atomic_load(&registrations) == 1 && atomic_load(&originalCalls) == 11);

    // Files are checked only on demand and the canonical compiler bundle may not escape the snapshot runtime.
    char moved[PATH_MAX]; join(moved, outside, "MTLCompilerService.xpc");
    assert(!rename(bundle, moved) && !symlink(moved, bundle)); expectImport(root, executable, 0);
    assert(!unlink(bundle)); expectImport(root, executable, 0);
    assert(!rename(moved, bundle));
    assert(!unlink(info)); expectImport(root, executable, 0);
    assert(!symlink(outsideFile, info)); expectImport(root, executable, 0);
    assert(!unlink(info) && !mkdir(info, 0700)); expectImport(root, executable, 0);
    assert(!rmdir(info) && !mkfifo(info, 0600)); expectImport(root, executable, 0);
    assert(!unlink(info)); makeFile(info);
    assert(!truncate(compiler, 0)); expectImport(root, executable, 0);
    makeFile(compiler);
    assert(!unlink(compiler)); expectImport(root, executable, 0);
    assert(!symlink(outsideFile, compiler)); expectImport(root, executable, 0);
    assert(!unlink(compiler)); makeFile(compiler);
    expectImport(root, executable, 1);

    // One failed attempt per process: fixing files does not repeat a potentially committed void import in an existing PID domain.
    assert(!unlink(info)); expectImport(root, executable, 0);
    makeFile(info);
    connectWith(compilerService, NULL, EIO);
    assert(!atomic_load(&registrations));
    expectImport(root, executable, 1);
    removeTree(temporary);
    puts("MetalCompilerRegistration: guest-only demand import, all contained clients, exact service, ordering, concurrency, recursion and errno passed");
    return 0;
}
