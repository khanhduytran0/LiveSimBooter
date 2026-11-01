// All shared-region syscalls, TSD switches and exits are mocked; no real host cache or thread state is changed.
#define LSB_SHARED_CACHE_HANDOFF_TESTING
#include "../LiveSimBooter/SharedCacheHandoff.cpp"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <setjmp.h>
#include <string>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

static unsigned checks = 0;
static void check(bool condition, const char *message) {
    ++checks;
    if (!condition) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}

enum class Event { ReadTSD, Query, DebugBegin, Switch, Detach, Restore, DebugFinish, DebugCancel, Acquire, Release, Reserve, Diagnostic, Entry, Main, Exit };
static std::vector<Event> events;
static constexpr uintptr_t hostTSD = 0x100020e0, simulatorTSD = 0x140000e0;
static uintptr_t currentTSD;
static uint64_t queryAddress;
static jmp_buf exitJump;
static int queryError, detachError, queryCalls, detachCalls, switchCalls, restoreCalls, entryCalls, mainCalls, mainStatus, exitStatus;
static int debugBeginError, debugFinishError, debugBeginCalls, debugFinishCalls, debugCancelCalls;
static uintptr_t returnedStartGlue;
static bool returnMain, failSwitch, failRestore;
static const LSBDyldSimLaunch *expected;
static bool alternateStackTest = false;
static uintptr_t alternateBottom, alternateTop, oldStackBottom;
static size_t oldStackSize;
static unsigned reservationCalls;
static bool failReservation, wrongReservationAddress;
static std::string reservationDiagnostic;
static LSBSlidableDataHeader slidableBacking;
static bool useSlidableBacking, failAcquisition;
static unsigned acquiredRanges, releasedRanges;

const LSBSlidableDataHeader *LSBPreparedSlidableCacheData() { return useSlidableBacking ? &slidableBacking : nullptr; }

static void expectEvents(std::initializer_list<Event> sequence) {
    check(events.size() == sequence.size(), "expected operation count");
    size_t index = 0;
    for (Event event : sequence) check(events[index++] == event, "TSD, cache and entry operations happen in safe order");
}

namespace lsb {
int *errnoAddress() { static int error = EACCES; return &error; }
mach_port_t task_self_trap() { return 0x123; }
kern_return_t vm_allocate(vm_map_t task, vm_address_t *address, vm_size_t size, int flags) {
    events.push_back(Event::Acquire); ++acquiredRanges;
    check(task == 0x123 && flags == VM_FLAGS_FIXED && address && *address == slidableBacking.actualBase && size == slidableBacking.cacheSpan,
          "slidable reservation uses fixed allocation without overwrite");
    check(currentTSD == hostTSD && !debugBeginCalls && !switchCalls, "non-overwriting reservation happens while native TLS is intact");
    return failAcquisition ? KERN_NO_SPACE : KERN_SUCCESS;
}
kern_return_t vm_deallocate(vm_map_t task, vm_address_t address, vm_size_t size) {
    events.push_back(Event::Release); ++releasedRanges;
    check(task == 0x123 && address == slidableBacking.actualBase && size == slidableBacking.cacheSpan && currentTSD == hostTSD,
          "pre-entry failure releases only the acquired range with native TLS restored");
    return KERN_SUCCESS;
}

ssize_t write(int fd, const void *bytes, size_t size) {
    events.push_back(Event::Diagnostic);
    check(fd == 2 && size < 192 && reservationCalls == 1 && !entryCalls, "reservation failure emits one bounded pre-entry stderr diagnostic");
    reservationDiagnostic.assign(static_cast<const char *>(bytes), size);
    return -1; // Even a closed stderr must not prevent raw termination after detachment.
}

void *mmap(void *address, size_t size, int protection, int flags, int fd, off_t offset) {
    events.push_back(Event::Reserve);
    ++reservationCalls;
    check(expected && address == reinterpret_cast<void *>(expected->cacheAddress) && size == expected->cacheSize,
          "whole cache reservation includes its declared gaps");
    check(protection == PROT_NONE && flags == (MAP_FIXED | MAP_ANON | MAP_PRIVATE) && fd == -1 && offset == 0,
          "cache reservation owns inaccessible anonymous pages at the exact selected address");
    check(currentTSD == simulatorTSD && debugFinishCalls == 1 && !debugFinishError && !entryCalls,
          "cache reservation follows completed TLS, cache and debugger handoff, before dyld entry");
    if (alternateStackTest) {
        uintptr_t frame = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
        check(frame >= alternateBottom && frame < alternateTop, "cache reservation runs entirely on the replacement stack");
        return ::mmap(address, size, protection, flags, fd, offset);
    }
    if (failReservation) return MAP_FAILED;
    return wrongReservationAddress ? static_cast<unsigned char *>(address) + 4096 : address;
}

int beginDebuggerHandoff() {
    events.push_back(Event::DebugBegin);
    ++debugBeginCalls;
    check(currentTSD == hostTSD, "debugger handoff begins with host TLS still installed");
    check(queryCalls == 1 && switchCalls == 0 && detachCalls == 0 && entryCalls == 0,
          "debugger publication begins after cache query and before TLS switch, detach or simulator entry");
    check(queryError == 0 || queryError == EINVAL || queryError == ENOMEM, "fatal cache query errors never publish debugger handoff");
    return debugBeginError;
}

int finishDebuggerHandoff(bool cacheDetached) {
    events.push_back(Event::DebugFinish);
    ++debugFinishCalls;
    check(currentTSD == simulatorTSD && switchCalls == 1, "debugger handoff finishes using simulator TLS");
    check(debugBeginCalls == 1 && !debugBeginError && !failSwitch && !detachError, "only successful cache handling finishes debugger handoff");
    check(cacheDetached == (!queryError && !expected->retainHostCache) && detachCalls == (cacheDetached ? 1 : 0),
          "debugger finish receives the actual cache detachment state");
    check(restoreCalls == 0 && debugCancelCalls == 0 && entryCalls == 0, "debugger finish precedes simulator entry without rollback");
    return debugFinishError;
}

void cancelDebuggerHandoff() {
    events.push_back(Event::DebugCancel);
    ++debugCancelCalls;
    check(currentTSD == hostTSD && restoreCalls == 1 && !failRestore, "debugger handoff cancellation requires successful host TLS restoration");
    check(debugBeginCalls == 1 && !debugBeginError && debugFinishCalls == 0 && entryCalls == 0,
          "only a begun, unfinished debugger handoff may be cancelled before simulator entry");
    check(failSwitch || detachError, "debugger rollback happens only after a TLS switch or cache detachment failure");
}

uintptr_t currentTSDBase() {
    events.push_back(Event::ReadTSD);
    return currentTSD;
}

bool setTSDBase(uintptr_t address) {
    if (address == simulatorTSD) {
        events.push_back(Event::Switch);
        ++switchCalls;
        check(queryCalls == 1 && detachCalls == 0 && entryCalls == 0, "TLS switches after query and before detach or entry");
        check(debugBeginCalls == 1 && !debugBeginError, "TLS switches only after debugger handoff begins successfully");
        check(currentTSD == hostTSD, "simulator TLS switch starts from host TLS");
        // Even a failed switch may have changed kernel state; callers must explicitly restore host TLS.
        currentTSD = simulatorTSD;
        return !failSwitch;
    }
    check(address == hostTSD, "only the prepared simulator and original host TSD bases may be installed");
    events.push_back(Event::Restore);
    ++restoreCalls;
    check(switchCalls == 1 && entryCalls == 0, "rollback happens only after a switch and before simulator entry");
    if (!failRestore) currentTSD = hostTSD;
    return !failRestore;
}
}

int LSBTestSharedRegionCheck(uint64_t *address) {
    if (address) {
        events.push_back(Event::Query);
        ++queryCalls;
        check(currentTSD == hostTSD, "cache query uses original host TLS");
        check(switchCalls == 0 && detachCalls == 0, "query precedes TLS switch and cache removal");
        check(*address == 0, "query address storage is initialized");
        *address = queryAddress;
        return queryError;
    }
    events.push_back(Event::Detach);
    ++detachCalls;
    check(currentTSD == simulatorTSD && switchCalls == 1, "cache removal requires simulator TLS already installed");
    check(!failSwitch && queryError == 0, "failed switch or query never removes the cache");
    return detachError;
}

[[noreturn]] void LSBTestExit(int status) {
    events.push_back(Event::Exit);
    if (failRestore) {
        check(status == 127 && restoreCalls == 1 && entryCalls == 0, "failed host TLS restoration exits before simulator entry");
        check(debugCancelCalls == 0 && debugFinishCalls == 0, "failed TLS restoration neither cancels nor finishes debugger handoff");
    } else if (debugFinishError) {
        check(status == 126 && debugFinishCalls == 1 && currentTSD == simulatorTSD, "failed debugger finish exits fatally with simulator TLS");
        check(entryCalls == 0 && mainCalls == 0 && restoreCalls == 0 && debugCancelCalls == 0,
              "failed debugger finish never enters simulator or attempts host rollback");
    } else if (failReservation || wrongReservationAddress) {
        check(status == 126 && reservationCalls == 1 && currentTSD == simulatorTSD, "cache reservation failure exits fatally after detachment");
        check(!entryCalls && !mainCalls && !restoreCalls && !debugCancelCalls, "reservation failure cannot enter dyld or return to host cleanup");
    } else {
        check(entryCalls == 1 && currentTSD == simulatorTSD, "normal raw exit follows entry using simulator TLS");
    }
    exitStatus = status;
    if (alternateStackTest) _exit(status);
    longjmp(exitJump, 1);
}

static int fakeMain(int argc, const char *const argv[], const char *const envp[], const char *const apple[]) {
    if (alternateStackTest) {
        uintptr_t frame = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
        check(frame >= alternateBottom && frame < alternateTop, "returned main runs entirely on the replacement stack");
    }
    events.push_back(Event::Main);
    ++mainCalls;
    check(entryCalls == 1 && returnMain && returnedStartGlue == 1, "main is invoked once after a valid LC_MAIN prepare result");
    check(currentTSD == simulatorTSD && restoreCalls == 0, "returned main keeps simulator TLS and never restores host TLS");
    check(queryCalls == 1 && detachCalls == (!queryError && !expected->retainHostCache ? 1 : 0),
          "returned main runs after completed cache handling");
    check(argc == expected->argc && argv == expected->argv && envp == expected->envp && apple == expected->apple,
          "returned main receives the same argument vectors as prepare");
    return mainStatus;
}

static LSBDyldSimLaunch::Main fakeEntry(int argc, const char *const argv[], const char *const envp[], const char *const apple[],
                                      const mach_header *executable, const mach_header *dyld, uintptr_t slide,
                                      const SimSyscallHelpers *helpers, uintptr_t *startGlue) {
    events.push_back(Event::Entry);
    ++entryCalls;
    check(currentTSD == simulatorTSD && switchCalls == 1 && restoreCalls == 0, "simulator entry uses fresh TLS without rollback");
    check(queryCalls == 1 && detachCalls == (!queryError && !expected->retainHostCache ? 1 : 0), "entry follows completed cache handling");
    check(reservationCalls == (expected->cacheSize ? 1U : 0U), "dyld starts only after its complete cache span is reserved");
    check(!detachError && !failSwitch, "failed detach or TLS switch never enters dyld_sim");
    check(debugBeginCalls == 1 && debugFinishCalls == 1 && !debugFinishError && debugCancelCalls == 0,
          "entry follows a completed debugger handoff without cancellation");
    check(argc == expected->argc && argv == expected->argv && envp == expected->envp && apple == expected->apple,
          "entry receives original argument vectors");
    check(executable == expected->executable && dyld == expected->dyld && slide == expected->slide && helpers == expected->helpers,
          "entry receives executable, dyld and helpers unchanged");
    check(startGlue && *startGlue == 0, "entry receives initialized start glue storage");
    if (alternateStackTest) {
        auto *frame = static_cast<uintptr_t *>(__builtin_frame_address(0));
        check(reinterpret_cast<uintptr_t>(frame) >= alternateBottom && reinterpret_cast<uintptr_t>(frame) < alternateTop,
              "prepare entry runs on the replacement stack");
        auto *caller = reinterpret_cast<const uintptr_t *>(frame[0]);
        check(reinterpret_cast<uintptr_t>(caller) >= alternateBottom && reinterpret_cast<uintptr_t>(caller) < alternateTop && !caller[0],
              "new continuation has no frame link into the abandoned stack");
        vm_address_t region = oldStackBottom;
        vm_size_t length = 0;
        vm_region_basic_info_data_64_t info = {};
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        check(vm_region_64(mach_task_self(), &region, &length, VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info), &count,
                           &object) == KERN_SUCCESS, "query actual cache reservation");
        if (object) mach_port_deallocate(mach_task_self(), object);
        check(region <= oldStackBottom && region + length >= oldStackBottom + oldStackSize && info.protection == VM_PROT_NONE,
              "post-switch reservation already made the old stack inaccessible before the dyld entry");
        size_t page = getpagesize();
        for (uintptr_t address : {oldStackBottom, oldStackBottom + oldStackSize - page}) {
            check(::mmap(reinterpret_cast<void *>(address), page, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_ANON | MAP_PRIVATE, -1, 0) ==
                  reinterpret_cast<void *>(address), "simulated cache segment replaces part of the reservation");
        }
        region = oldStackBottom + page; count = VM_REGION_BASIC_INFO_COUNT_64; object = MACH_PORT_NULL;
        check(vm_region_64(mach_task_self(), &region, &length, VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info), &count,
                           &object) == KERN_SUCCESS, "query the gap between replacement cache segments");
        if (object) mach_port_deallocate(mach_task_self(), object);
        check(region == oldStackBottom + page && length == oldStackSize - 2 * page && info.protection == VM_PROT_NONE,
              "cache gap remains owned and inaccessible after segment replacements");
        void *loose = ::mmap(reinterpret_cast<void *>(oldStackBottom + page), page, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        check(loose != MAP_FAILED && (reinterpret_cast<uintptr_t>(loose) < oldStackBottom ||
                                     reinterpret_cast<uintptr_t>(loose) >= oldStackBottom + oldStackSize),
              "ordinary loose-image allocation cannot use the reserved cache gap");
        check(::munmap(loose, page) == 0, "release test loose-image allocation outside the cache");
        check(mprotect(reinterpret_cast<void *>(oldStackBottom), oldStackSize, PROT_NONE) == 0,
              "make the entire native stack inaccessible before prepare returns");
    }
    *startGlue = returnedStartGlue;
    return returnMain ? fakeMain : nullptr;
}

static void reset(int query = 0, int detach = 0, bool hasMain = true) {
    events.clear();
    currentTSD = hostTSD;
    queryAddress = 0x180000000;
    queryError = query;
    detachError = detach;
    returnMain = hasMain;
    mainStatus = 0;
    returnedStartGlue = 1;
    failSwitch = failRestore = false;
    queryCalls = detachCalls = switchCalls = restoreCalls = entryCalls = mainCalls = 0;
    debugBeginError = debugFinishError = debugBeginCalls = debugFinishCalls = debugCancelCalls = 0;
    exitStatus = -1;
    reservationCalls = 0;
    failReservation = wrongReservationAddress = false;
    reservationDiagnostic.clear();
    useSlidableBacking = failAcquisition = false;
    acquiredRanges = releasedRanges = 0;
}

static void expectExit(const LSBDyldSimLaunch &launch, int status, int entries = 1, int mains = 1) {
    expected = &launch;
    if (setjmp(exitJump) == 0) {
        LSBDetachHostCacheAndEnter(&launch);
        check(false, "successful entry, failed TLS rollback or failed debugger finish must never return to host code");
    }
    check(exitStatus == status, "expected raw exit status");
    check(entryCalls == entries && mainCalls == mains, "expected prepare and returned main invocation counts");
}

static void expectReturn(const LSBDyldSimLaunch *launch, int error) {
    check(LSBDetachHostCacheAndEnter(launch) == error, "handoff returns the expected error");
    check(currentTSD == hostTSD, "return to host code requires original TLS restored");
    check(entryCalls == 0 && mainCalls == 0 && exitStatus == -1, "recoverable failure neither enters simulator nor exits");
}

static void actualStackSwitchTest(const LSBDyldSimLaunch &launch) {
    pid_t child = fork();
    check(child >= 0, "fork real assembly stack-switch probe");
    if (!child) {
        reset();
        oldStackSize = pthread_get_stacksize_np(pthread_self());
        oldStackBottom = reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(pthread_self())) - oldStackSize;
        size_t page = getpagesize(), usable = 8 * 1024 * 1024;
        void *mapping = mmap(nullptr, usable + 2 * page, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
        check(mapping != MAP_FAILED, "allocate guarded test stack");
        alternateBottom = reinterpret_cast<uintptr_t>(mapping) + page;
        alternateTop = alternateBottom + usable;
        check(mprotect(reinterpret_cast<void *>(alternateBottom), usable, PROT_READ | PROT_WRITE) == 0, "enable test stack interior");
        static LSBDyldSimLaunch staticLaunch;
        staticLaunch = launch;
        staticLaunch.stackTop = alternateTop;
        staticLaunch.cacheAddress = oldStackBottom;
        staticLaunch.cacheSize = oldStackSize;
        expected = &staticLaunch;
        alternateStackTest = true;
        mainStatus = 42;
        LSBDetachHostCacheAndEnter(&staticLaunch);
        _exit(2);
    }
    int status = 0;
    check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 42,
          "prepare, returned main and raw exit survive a completely inaccessible original stack");
}

int main() {
    static const char *argv[] = {"/RuntimeRoot/bin/df", "-h", nullptr};
    static const char *envp[] = {"DYLD_ROOT_PATH=/RuntimeRoot", nullptr};
    static const char *apple[] = {"executable_path=/RuntimeRoot/bin/df", nullptr};
    LSBDyldSimLaunch launch = {fakeEntry, 2, argv, envp, apple, reinterpret_cast<const mach_header *>(0x100000000),
                               reinterpret_cast<const mach_header *>(0x120000000), 0x20000000,
                               reinterpret_cast<const SimSyscallHelpers *>(0x130000000), {hostTSD, simulatorTSD}};
    check(launch.retainHostCache, "aggregate launch descriptors retain the native cache by default");
    launch.retainHostCache = false; // Exercise the historical detachment and rollback cases explicitly.
    actualStackSwitchTest(launch);
    LSBDyldSimLaunch reserved = launch;
    reserved.cacheAddress = 0x120000000;
    reserved.cacheSize = 0x100000;
    LSBDyldSimLaunch retained = reserved;
    retained.retainHostCache = true;
    LSBDyldSimLaunch slidable = retained;
    slidable.cacheAddress = 0x7000000000; slidable.cacheSize = LSB_SLIDABLE_CACHE_SPAN;
    slidableBacking.actualBase = slidable.cacheAddress; slidableBacking.cacheSpan = slidable.cacheSize;
    reset(); useSlidableBacking = true;
    expectExit(slidable, 0);
    check(acquiredRanges == 1 && !releasedRanges && !detachCalls, "high cache owns its complete range while retaining the native cache");
    expectEvents({Event::ReadTSD, Event::Query, Event::Acquire, Event::DebugBegin, Event::Switch, Event::DebugFinish,
                  Event::Reserve, Event::Entry, Event::Main, Event::Exit});
    reset(); useSlidableBacking = true; failAcquisition = true;
    expectReturn(&slidable, EADDRINUSE);
    check(!debugBeginCalls && !switchCalls && !reservationCalls && !releasedRanges, "occupied high range is rejected without replacing or releasing it");
    reset(); useSlidableBacking = true; debugBeginError = EIO;
    expectReturn(&slidable, EIO);
    check(acquiredRanges == 1 && releasedRanges == 1 && !switchCalls, "debugger preparation failure releases the acquired high range");
    reset(); useSlidableBacking = true; failSwitch = true;
    expectReturn(&slidable, EIO);
    check(acquiredRanges == 1 && releasedRanges == 1 && restoreCalls == 1, "TLS failure restores native state and releases the acquired high range");
    reset(); useSlidableBacking = true;
    slidable.retainHostCache = false;
    expectReturn(&slidable, EPERM);
    check(!acquiredRanges && !debugBeginCalls && !detachCalls, "slidable cache cannot request native-cache detachment");
    for (uint64_t nativeBase : {uint64_t(0x180000000), uint64_t(0x120100000)}) {
        reset();
        queryAddress = nativeBase;
        mainStatus = 42;
        expectExit(retained, 42);
        expectEvents({Event::ReadTSD, Event::Query, Event::DebugBegin, Event::Switch, Event::DebugFinish,
                      Event::Reserve, Event::Entry, Event::Main, Event::Exit});
        check(!detachCalls && !restoreCalls && reservationCalls == 1,
              "retention preserves native mappings while switching TLS and reserving the disjoint simulator span");
    }
    for (uint64_t nativeBase : {uint64_t(0x1200ff000), uint64_t(0x120000000), uint64_t(0x110000000)}) {
        reset();
        queryAddress = nativeBase;
        expectReturn(&retained, EADDRINUSE);
        expectEvents({Event::ReadTSD, Event::Query});
        check(!debugBeginCalls && !switchCalls && !detachCalls && !reservationCalls,
              "overlap is refused before debugger, TLS or mapping mutations");
    }
    for (int absent : {EINVAL, ENOMEM, 0}) {
        reset(absent);
        if (!absent) queryAddress = 0;
        expectReturn(&retained, ENOTSUP);
        expectEvents({Event::ReadTSD, Event::Query});
        check(!debugBeginCalls && !switchCalls && !detachCalls && !reservationCalls,
              "retention requires a successful nonzero native cache query before any mutation");
    }
    reset(EIO);
    expectReturn(&retained, EIO);
    expectEvents({Event::ReadTSD, Event::Query});
    reset();
    debugBeginError = EPERM;
    expectReturn(&retained, EPERM);
    expectEvents({Event::ReadTSD, Event::Query, Event::DebugBegin});
    reset();
    failSwitch = true;
    expectReturn(&retained, EIO);
    expectEvents({Event::ReadTSD, Event::Query, Event::DebugBegin, Event::Switch, Event::Restore, Event::DebugCancel});
    reset();
    debugFinishError = EIO;
    expectExit(retained, 126, 0, 0);
    expectEvents({Event::ReadTSD, Event::Query, Event::DebugBegin, Event::Switch, Event::DebugFinish, Event::Exit});
    for (int query : {0, EINVAL}) {
        reset(query);
        expectExit(reserved, 0);
        check(reservationCalls == 1, "cache span is reserved even when no native shared region needed detaching");
        for (bool wrongAddress : {false, true}) {
            reset(query);
            failReservation = !wrongAddress;
            wrongReservationAddress = wrongAddress;
            expectExit(reserved, 126, 0, 0);
            check(events[events.size() - 3] == Event::Reserve && events[events.size() - 2] == Event::Diagnostic && events.back() == Event::Exit,
                  "failed reservation has only a bounded diagnostic and raw exit after its mapping attempt");
            check(reservationDiagnostic.find(wrongAddress ? "wrong address" : "mmap failed") != std::string::npos,
                  "fatal diagnostic distinguishes mmap failure from a wrong returned address");
            if (!wrongAddress)
                check(reservationDiagnostic.find("errno=0x0000000d address=0x0000000120000000 size=0x0000000000100000") != std::string::npos,
                      "mmap failure reports exact numeric errno and range without host formatting or TLS");
        }
    }
    for (int status : {0, 42, 127}) {
        reset();
        mainStatus = status;
        expectExit(launch, status);
        expectEvents({Event::ReadTSD, Event::Query, Event::DebugBegin, Event::Switch, Event::Detach, Event::DebugFinish, Event::Entry, Event::Main, Event::Exit});
    }
    for (uintptr_t startGlue : {uintptr_t(0), uintptr_t(1), uintptr_t(0x12345678)}) {
        reset(0, 0, false);
        returnedStartGlue = startGlue;
        expectExit(launch, 1, 1, 0);
        expectEvents({Event::ReadTSD, Event::Query, Event::DebugBegin, Event::Switch, Event::Detach, Event::DebugFinish, Event::Entry, Event::Exit});
    }
    for (uintptr_t startGlue : {uintptr_t(0), uintptr_t(0x12345678)}) {
        reset();
        returnedStartGlue = startGlue;
        expectExit(launch, 126, 1, 0);
        expectEvents({Event::ReadTSD, Event::Query, Event::DebugBegin, Event::Switch, Event::Detach, Event::DebugFinish, Event::Entry, Event::Exit});
    }
    for (int noCache : {EINVAL, ENOMEM}) {
        reset(noCache);
        expectExit(launch, 0);
        expectEvents({Event::ReadTSD, Event::Query, Event::DebugBegin, Event::Switch, Event::DebugFinish, Event::Entry, Event::Main, Event::Exit});
    }
    for (int error : {EIO, EPERM}) {
        reset(error);
        expectReturn(&launch, error);
        expectEvents({Event::ReadTSD, Event::Query});
    }
    for (int query : {0, EINVAL, ENOMEM}) {
        for (int error : {EIO, EPERM}) {
            reset(query);
            debugBeginError = error;
            expectReturn(&launch, error);
            check(switchCalls == 0 && detachCalls == 0 && restoreCalls == 0, "failed debugger begin leaves host TLS and cache untouched");
            check(debugFinishCalls == 0 && debugCancelCalls == 0, "failed debugger begin neither finishes nor cancels handoff");
            expectEvents({Event::ReadTSD, Event::Query, Event::DebugBegin});

            reset(query);
            debugFinishError = error;
            expectExit(launch, 126, 0, 0);
            if (!query) {
                expectEvents({Event::ReadTSD, Event::Query, Event::DebugBegin, Event::Switch, Event::Detach, Event::DebugFinish, Event::Exit});
            } else {
                expectEvents({Event::ReadTSD, Event::Query, Event::DebugBegin, Event::Switch, Event::DebugFinish, Event::Exit});
            }
        }
    }

    reset();
    expectReturn(nullptr, EINVAL);
    expectEvents({});
    for (unsigned mutation = 0; mutation < 5; ++mutation) {
        reset();
        auto invalid = reserved;
        switch (mutation) {
            case 0: invalid.cacheAddress = 0; break;
            case 1: invalid.cacheSize = 0; break;
            case 2: ++invalid.cacheAddress; break;
            case 3: ++invalid.cacheSize; break;
            case 4: invalid.cacheAddress = UINTPTR_MAX - 4095; invalid.cacheSize = 4096; break;
        }
        expectReturn(&invalid, EINVAL);
        expectEvents({});
    }
    reset();
    LSBDyldSimLaunch misaligned = launch;
    misaligned.stackTop = 0x400100001;
    expectReturn(&misaligned, EINVAL);
    expectEvents({});
    for (bool missingHost : {true, false}) {
        reset();
        LSBDyldSimLaunch invalid = launch;
        if (missingHost) invalid.thread.hostTSD = 0;
        else invalid.thread.simulatorTSD = 0;
        expectReturn(&invalid, EINVAL);
        expectEvents({});
    }
    reset();
    LSBDyldSimLaunch mismatch = launch;
    mismatch.thread.hostTSD += 16;
    expectReturn(&mismatch, EINVAL);
    expectEvents({Event::ReadTSD});

    for (int query : {0, EINVAL, ENOMEM}) {
        reset(query);
        failSwitch = true;
        expectReturn(&launch, EIO);
        expectEvents({Event::ReadTSD, Event::Query, Event::DebugBegin, Event::Switch, Event::Restore, Event::DebugCancel});

        reset(query);
        failSwitch = failRestore = true;
        expectExit(launch, 127, 0, 0);
        expectEvents({Event::ReadTSD, Event::Query, Event::DebugBegin, Event::Switch, Event::Restore, Event::Exit});
    }
    for (int error : {EPERM, EIO}) {
        reset(0, error);
        expectReturn(&launch, error);
        expectEvents({Event::ReadTSD, Event::Query, Event::DebugBegin, Event::Switch, Event::Detach, Event::Restore, Event::DebugCancel});

        reset(0, error);
        failRestore = true;
        expectExit(launch, 127, 0, 0);
        expectEvents({Event::ReadTSD, Event::Query, Event::DebugBegin, Event::Switch, Event::Detach, Event::Restore, Event::Exit});
    }
    printf("Shared cache handoff: %u checks passed\n", checks);
}
