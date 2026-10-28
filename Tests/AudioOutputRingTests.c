#include "../ProcessSupport/AudioOutput.h"
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static LSBAudioRing ring;
static void *produce(void *unused) {
    (void)unused;
    uint64_t cursor = 0;
    for (unsigned i = 0; i < 100000; ++i) {
        float frame[2] = {(float)(i % 1000) / 1000, -(float)(i % 1000) / 1000};
        while (!LSBAudioRingWrite(&ring, &cursor, frame, 1)) sched_yield();
    }
    return NULL;
}
int main(void) {
    LSBAudioRingInitialize(&ring);
    assert(!LSBAudioRingValidate(&ring, LSBAudioMapBytes));
    assert(LSBAudioRingValidate(&ring, sizeof(ring)));
    ring.version++; assert(LSBAudioRingValidate(&ring, LSBAudioMapBytes)); ring.version--;
    float *frames = calloc(LSBAudioFrames * 2, sizeof(float));
    assert(frames);
    for (unsigned i = 0; i < LSBAudioFrames * 2; ++i) frames[i] = 0.25f;
    uint64_t writer = 0, reader = 0;
    assert(LSBAudioRingWrite(&ring, &writer, frames, LSBAudioFrames + 1) == LSBAudioFrames);
    assert(!LSBAudioRingWrite(&ring, &writer, frames, 1));
    assert(LSBAudioRingRead(&ring, &reader, frames, LSBAudioFrames) == LSBAudioFrames);
    for (unsigned i = 0; i < LSBAudioFrames * 2; ++i) assert(frames[i] == 0.25f);
    frames[0] = NAN; frames[1] = INFINITY; frames[2] = 2; frames[3] = -2;
    assert(LSBAudioRingWrite(&ring, &writer, frames, 2) == 2);
    assert(LSBAudioRingRead(&ring, &reader, frames, 4) == 2);
    assert(frames[0] == 0 && frames[1] == 0 && frames[2] == 1 && frames[3] == -1 && frames[4] == 0 && frames[7] == 0);
    atomic_store(&ring.read, writer + 1); assert(!LSBAudioRingWrite(&ring, &writer, frames, 1));
    atomic_store(&ring.written, reader + LSBAudioFrames + 1);
    assert(!LSBAudioRingRead(&ring, &reader, frames, 1) && atomic_load(&ring.closed));
    LSBAudioRingInitialize(&ring); reader = 0;
    pthread_t producer;
    assert(!pthread_create(&producer, NULL, produce, NULL));
    for (unsigned i = 0; i < 100000; ++i) {
        while (!LSBAudioRingRead(&ring, &reader, frames, 1)) sched_yield();
        float expected = (float)(i % 1000) / 1000;
        assert(frames[0] == expected && frames[1] == -expected);
    }
    assert(!pthread_join(producer, NULL));
    assert(reader == 100000 && atomic_load(&ring.rendered) == 100000);
    atomic_store(&ring.closed, 1);
    assert(!LSBAudioRingWrite(&ring, &writer, frames, 1));
    free(frames);
    puts("AudioOutputRing: wraparound, backpressure, malformed peer, finite sample clamp and concurrent ordering passed");
}
