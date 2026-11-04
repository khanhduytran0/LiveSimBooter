// Run inside the simulator after adopting native kernel/platform/pthread support. Every blocking stage has a process-wide deadline.
#include <dispatch/dispatch.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { ThreadCount = 8, DispatchJobs = 32, DispatchRounds = 3, WorkerSlots = 64 };
static pthread_key_t threadKey;
static pthread_mutex_t gateLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gateCondition = PTHREAD_COND_INITIALIZER;
static unsigned readyThreads;
static bool releaseThreads;
static atomic_uint failures, destructorCalls, dispatchCalls, timerCalls;
static atomic_uint_fast64_t workers[DispatchRounds][WorkerSlots];

static void deadline(int signalNumber) {
    (void)signalNumber;
    static const char message[] = "NATIVE_SUPPORT_PROBE FAIL deadline exceeded\n";
    write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(124);
}

static void fail(const char *stage, int error) {
    fprintf(stderr, "NATIVE_SUPPORT_PROBE FAIL stage=%s error=%d\n", stage, error);
    fflush(stderr);
    _exit(1);
}

static void checkpoint(const char *stage) {
    printf("NATIVE_SUPPORT_PROBE checkpoint=%s\n", stage);
    fflush(stdout);
}

static void verify(bool condition) {
    if (!condition) atomic_fetch_add_explicit(&failures, 1, memory_order_relaxed);
}

static bool exerciseAllocation(unsigned seed) {
    const size_t initialSize = 257, finalSize = 8193;
    unsigned char *bytes = malloc(initialSize);
    if (!bytes) return false;
    for (size_t i = 0; i != initialSize; ++i) bytes[i] = (unsigned char)(seed + i);
    unsigned char *expanded = realloc(bytes, finalSize);
    if (!expanded) { free(bytes); return false; }
    bool valid = true;
    for (size_t i = 0; i != initialSize; ++i) valid &= expanded[i] == (unsigned char)(seed + i);
    memset(expanded + initialSize, 0xa5, finalSize - initialSize);
    free(expanded);
    return valid;
}

typedef struct { unsigned index; uint64_t threadID; } ThreadContext;
typedef struct { unsigned index; uint64_t marker; } ThreadValue;

static void destroyThreadValue(void *pointer) {
    ThreadValue *value = pointer;
    verify(value && value->index < ThreadCount && value->marker == UINT64_C(0x4c53425453440000) + value->index);
    free(value);
    atomic_fetch_add_explicit(&destructorCalls, 1, memory_order_relaxed);
}

static void *pthreadWorker(void *pointer) {
    ThreadContext *context = pointer;
    ThreadValue *value = malloc(sizeof(*value));
    if (!value) fail("pthread-tls-allocation", ENOMEM);
    *value = (ThreadValue){context->index, UINT64_C(0x4c53425453440000) + context->index};
    int error = pthread_setspecific(threadKey, value);
    if (error) fail("pthread-setspecific", error);
    verify(pthread_getspecific(threadKey) == value);
    verify(pthread_threadid_np(NULL, &context->threadID) == 0 && context->threadID != 0);
    int expectedErrno = 100 + (int)context->index;
    errno = expectedErrno;
    pthread_mutex_lock(&gateLock);
    ++readyThreads;
    pthread_cond_broadcast(&gateCondition);
    while (!releaseThreads) pthread_cond_wait(&gateCondition, &gateLock);
    pthread_mutex_unlock(&gateLock);
    verify(errno == expectedErrno);
    for (unsigned iteration = 0; iteration != 32; ++iteration) verify(exerciseAllocation(context->index + iteration));
    verify(pthread_getspecific(threadKey) == value);
    return (void *)(uintptr_t)(context->index + 1);
}

static void exercisePthreads(void) {
    int error = pthread_key_create(&threadKey, destroyThreadValue);
    if (error) fail("pthread-key-create", error);
    pthread_t threads[ThreadCount];
    ThreadContext contexts[ThreadCount] = {0};
    errno = EDOM;
    for (unsigned i = 0; i != ThreadCount; ++i) {
        contexts[i].index = i;
        if ((error = pthread_create(&threads[i], NULL, pthreadWorker, &contexts[i]))) fail("pthread-create", error);
    }
    struct timespec limit;
    clock_gettime(CLOCK_REALTIME, &limit);
    limit.tv_sec += 3;
    pthread_mutex_lock(&gateLock);
    while (readyThreads != ThreadCount) {
        error = pthread_cond_timedwait(&gateCondition, &gateLock, &limit);
        if (error) fail("pthread-start-barrier", error);
    }
    verify(errno == EDOM && !pthread_getspecific(threadKey));
    releaseThreads = true;
    pthread_cond_broadcast(&gateCondition);
    pthread_mutex_unlock(&gateLock);
    checkpoint("pthread-all-started");
    for (unsigned i = 0; i != ThreadCount; ++i) {
        void *result = NULL;
        if ((error = pthread_join(threads[i], &result))) fail("pthread-join", error);
        verify(result == (void *)(uintptr_t)(i + 1));
        for (unsigned j = 0; j != i; ++j) verify(contexts[i].threadID != contexts[j].threadID);
    }
    verify(atomic_load_explicit(&destructorCalls, memory_order_relaxed) == ThreadCount);
    if ((error = pthread_key_delete(threadKey))) fail("pthread-key-delete", error);
    if (atomic_load_explicit(&failures, memory_order_relaxed)) fail("pthread-assertions", EIO);
    checkpoint("pthread-join-errno-tls-malloc-pass");
}

typedef struct { unsigned round; unsigned index; } DispatchContext;
static void recordWorker(unsigned round, uint64_t threadID) {
    for (unsigned i = 0; i != WorkerSlots; ++i) {
        uint_fast64_t value = atomic_load_explicit(&workers[round][i], memory_order_relaxed);
        if (value == threadID) return;
        if (!value && atomic_compare_exchange_strong_explicit(&workers[round][i], &value, threadID, memory_order_relaxed, memory_order_relaxed)) return;
        if (value == threadID) return;
    }
    verify(false);
}

static void dispatchWorker(void *pointer) {
    DispatchContext *context = pointer;
    uint64_t threadID = 0;
    verify(pthread_threadid_np(NULL, &threadID) == 0 && threadID != 0);
    recordWorker(context->round, threadID);
    errno = (int)context->index + 200;
    int expectedErrno = errno;
    verify(exerciseAllocation(context->index));
    verify(errno == expectedErrno);
    atomic_fetch_add_explicit(&dispatchCalls, 1, memory_order_relaxed);
}

typedef struct { dispatch_source_t source; dispatch_semaphore_t complete; } TimerContext;
static void timerFired(void *pointer) {
    TimerContext *timer = pointer;
    verify(exerciseAllocation(81));
    if (atomic_fetch_add_explicit(&timerCalls, 1, memory_order_relaxed) + 1 >= 4) dispatch_source_cancel(timer->source);
}
static void timerCancelled(void *pointer) {
    TimerContext *timer = pointer;
    dispatch_semaphore_signal(timer->complete);
}

static void exerciseDispatch(void) {
    dispatch_group_t group = dispatch_group_create();
    dispatch_queue_t queue = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
    if (!group || !queue) fail("dispatch-create", ENOMEM);
    DispatchContext contexts[DispatchRounds][DispatchJobs];
    for (unsigned round = 0; round != DispatchRounds; ++round) {
        for (unsigned i = 0; i != DispatchJobs; ++i) {
            contexts[round][i] = (DispatchContext){round, i};
            dispatch_group_async_f(group, queue, &contexts[round][i], dispatchWorker);
        }
        if (dispatch_group_wait(group, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC))) fail("dispatch-group-wait", ETIMEDOUT);
        unsigned count = 0, reused = 0;
        for (unsigned i = 0; i != WorkerSlots; ++i) {
            uint64_t worker = atomic_load_explicit(&workers[round][i], memory_order_relaxed);
            if (!worker) continue;
            ++count;
            if (round) for (unsigned j = 0; j != WorkerSlots; ++j) {
                if (worker == atomic_load_explicit(&workers[round - 1][j], memory_order_relaxed)) { ++reused; break; }
            }
        }
        // Physical worker reuse is observed, not required: dispatch may legitimately retire or replace workers between rounds.
        printf("NATIVE_SUPPORT_PROBE dispatch-round=%u jobs=%u workers=%u reused=%u\n", round, DispatchJobs, count, reused);
        fflush(stdout);
        usleep(20000);
    }
    dispatch_release(group);
    verify(atomic_load_explicit(&dispatchCalls, memory_order_relaxed) == DispatchRounds * DispatchJobs);
    if (atomic_load_explicit(&failures, memory_order_relaxed)) fail("dispatch-assertions", EIO);
    checkpoint("dispatch-worker-group-reuse-pass");

    dispatch_queue_t timerQueue = dispatch_queue_create("com.kdt.LiveSimBooter.native-support-probe.timer", DISPATCH_QUEUE_SERIAL);
    TimerContext timer = {dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, timerQueue), dispatch_semaphore_create(0)};
    if (!timerQueue || !timer.source || !timer.complete) fail("dispatch-timer-create", ENOMEM);
    dispatch_set_context(timer.source, &timer);
    dispatch_source_set_event_handler_f(timer.source, timerFired);
    dispatch_source_set_cancel_handler_f(timer.source, timerCancelled);
    dispatch_source_set_timer(timer.source, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_MSEC), 10 * NSEC_PER_MSEC, NSEC_PER_MSEC);
    dispatch_resume(timer.source);
    if (dispatch_semaphore_wait(timer.complete, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC))) fail("dispatch-timer-wait", ETIMEDOUT);
    verify(atomic_load_explicit(&timerCalls, memory_order_relaxed) >= 4);
    dispatch_release(timer.source);
    dispatch_release(timer.complete);
    dispatch_release(timerQueue);
    if (atomic_load_explicit(&failures, memory_order_relaxed)) fail("dispatch-timer-assertions", EIO);
    checkpoint("dispatch-timer-cancel-pass");
}

int main(void) {
    struct sigaction action = {0};
    action.sa_handler = deadline;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGALRM, &action, NULL) == -1) fail("deadline-handler", errno);
    alarm(20);
    printf("NATIVE_SUPPORT_PROBE begin pid=%d native-support=%s\n", getpid(), getenv("LSB_NATIVE_SUPPORT") ?: "unset");
    fflush(stdout);
    verify(exerciseAllocation(31));
    exercisePthreads();
    exerciseDispatch();
    alarm(0);
    printf("NATIVE_SUPPORT_PROBE PASS pthreads=%u destructors=%u dispatch=%u timer=%u failures=%u\n", ThreadCount,
           atomic_load(&destructorCalls), atomic_load(&dispatchCalls), atomic_load(&timerCalls), atomic_load(&failures));
    fflush(stdout);
    return 0;
}
