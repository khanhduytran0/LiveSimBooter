#import "AudioOutput.h"
#import <AudioToolbox/AudioToolbox.h>
#include "../ProcessSupport/AudioOutput.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <xpc/xpc.h>

typedef struct { LSBAudioRing *ring; uint64_t cursor; _Atomic(bool) stopping; } AudioRenderContext;
static unsigned liveStreams; // Main thread only; also bounds memory/AudioQueue resources across all guest jobs.
static void render(void *context, AudioQueueRef queue, AudioQueueBufferRef buffer) {
    AudioRenderContext *state = context;
    if (atomic_load_explicit(&state->stopping, memory_order_acquire)) return;
    LSBAudioRingRead(state->ring, &state->cursor, buffer->mAudioData, LSBAudioQuantum);
    buffer->mAudioDataByteSize = LSBAudioQuantum * LSBAudioChannels * sizeof(float);
    AudioQueueEnqueueBuffer(queue, buffer, 0, NULL);
}

@implementation LSBAudioOutput {
    AudioQueueRef _queue;
    AudioRenderContext _render;
    xpc_object_t _sharedMemory;
    BOOL _counted;
}
- (instancetype)initWithError:(int *)error {
    NSAssert(NSThread.isMainThread, @"Audio stream control belongs to the main thread");
    if (error) *error = 0;
    if (!(self = [super init])) { if (error) *error = ENOMEM; return nil; }
    if (liveStreams >= LSBAudioMaximumStreams) { if (error) *error = EBUSY; return nil; }
    void *mapping = mmap(NULL, LSBAudioMapBytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    if (mapping == MAP_FAILED) { if (error) *error = errno; return nil; }
    _render.ring = mapping;
    _sharedMemory = xpc_shmem_create(mapping, LSBAudioMapBytes);
    if (!_sharedMemory) { if (error) *error = ENOMEM; return nil; }
    LSBAudioRingInitialize(_render.ring);
    AudioStreamBasicDescription format = {.mSampleRate = LSBAudioRate, .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked, .mBytesPerPacket = 8, .mFramesPerPacket = 1,
        .mBytesPerFrame = 8, .mChannelsPerFrame = LSBAudioChannels, .mBitsPerChannel = 32};
    OSStatus status = AudioQueueNewOutput(&format, render, &_render, NULL, NULL, 0, &_queue);
    for (unsigned i = 0; !status && i < 3; ++i) {
        AudioQueueBufferRef buffer = NULL;
        status = AudioQueueAllocateBuffer(_queue, LSBAudioQuantum * LSBAudioChannels * sizeof(float), &buffer);
        if (!status) {
            memset(buffer->mAudioData, 0, buffer->mAudioDataBytesCapacity);
            buffer->mAudioDataByteSize = buffer->mAudioDataBytesCapacity;
            status = AudioQueueEnqueueBuffer(_queue, buffer, 0, NULL);
        }
    }
    // Uses the native host's genuine foreground session. Do not alter the independent silent-background-audio preference/category.
    if (!status) status = AudioQueueStart(_queue, NULL);
    if (status) {
        fprintf(stderr, "[audio] native output start failed: %d\n", (int)status);
        if (error) *error = EIO;
        return nil;
    }
    ++liveStreams; _counted = YES;
    return self;
}
- (void *)sharedMemory { return (__bridge void *)_sharedMemory; }
- (void)invalidate {
    atomic_store_explicit(&_render.stopping, true, memory_order_release);
    if (_queue) { AudioQueueDispose(_queue, true); _queue = NULL; } // Waits for the last callback before unmapping.
    if (_render.ring) {
        atomic_store_explicit(&_render.ring->closed, 1, memory_order_release);
        fprintf(stderr, "[audio] output closed: rendered=%llu underrun=%llu frames\n",
            (unsigned long long)atomic_load(&_render.ring->rendered), (unsigned long long)atomic_load(&_render.ring->underrun));
        munmap(_render.ring, LSBAudioMapBytes); _render.ring = NULL;
    }
    _sharedMemory = nil;
    if (_counted) { --liveStreams; _counted = NO; }
}
- (void)dealloc { [self invalidate]; }
@end
