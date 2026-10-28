#ifndef LSB_AUDIO_OUTPUT_H
#define LSB_AUDIO_OUTPUT_H
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

enum { LSBAudioMagic = 0x4c534241, LSBAudioVersion = 1, LSBAudioRate = 48000, LSBAudioChannels = 2,
       LSBAudioFrames = 8192, LSBAudioQuantum = 512, LSBAudioMapBytes = 98304, LSBAudioMaximumStreams = 16 };
// Each side keeps its own cursor privately. Treat the peer's cursor as untrusted and reject distances beyond the fixed ring.
// Atomic sample bits also make malformed/concurrent peer writes defined; the renderer clamps non-finite/out-of-range samples.
typedef struct {
    uint32_t magic, version, rate, channels, capacity;
    uint32_t reserved[11];
    _Atomic(uint64_t) written;
    _Atomic(uint32_t) closed;
    uint32_t producerPadding[13];
    _Atomic(uint64_t) read, rendered, underrun;
    uint64_t consumerPadding[5];
    _Atomic(uint32_t) samples[LSBAudioFrames * LSBAudioChannels];
} LSBAudioRing;
_Static_assert(offsetof(LSBAudioRing, written) == 64, "audio producer layout");
_Static_assert(offsetof(LSBAudioRing, read) == 128, "audio consumer layout");
_Static_assert(offsetof(LSBAudioRing, samples) == 192, "audio samples layout");
_Static_assert(sizeof(LSBAudioRing) <= LSBAudioMapBytes, "audio mapping bounds");
_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2 && ATOMIC_INT_LOCK_FREE == 2, "audio callbacks require lock-free atomics");

void LSBAudioRingInitialize(LSBAudioRing *ring);
int LSBAudioRingValidate(const LSBAudioRing *ring, size_t size);
uint32_t LSBAudioRingWrite(LSBAudioRing *ring, uint64_t *cursor, const float *samples, uint32_t frames);
uint32_t LSBAudioRingRead(LSBAudioRing *ring, uint64_t *cursor, float *samples, uint32_t frames);
#endif
