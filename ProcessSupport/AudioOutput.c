#include "AudioOutput.h"
#include <errno.h>
#include <math.h>
#include <string.h>

void LSBAudioRingInitialize(LSBAudioRing *ring) {
    memset(ring, 0, sizeof(*ring));
    ring->magic = LSBAudioMagic; ring->version = LSBAudioVersion;
    ring->rate = LSBAudioRate; ring->channels = LSBAudioChannels; ring->capacity = LSBAudioFrames;
}
int LSBAudioRingValidate(const LSBAudioRing *ring, size_t size) {
    return ring && size == LSBAudioMapBytes && ring->magic == LSBAudioMagic && ring->version == LSBAudioVersion &&
        ring->rate == LSBAudioRate && ring->channels == LSBAudioChannels && ring->capacity == LSBAudioFrames ? 0 : EPROTO;
}
uint32_t LSBAudioRingWrite(LSBAudioRing *ring, uint64_t *cursor, const float *samples, uint32_t frames) {
    if (!ring || !cursor || !samples || atomic_load_explicit(&ring->closed, memory_order_acquire)) return 0;
    uint64_t read = atomic_load_explicit(&ring->read, memory_order_acquire), used = *cursor - read;
    if (used > LSBAudioFrames) return 0;
    if (frames > LSBAudioFrames - used) frames = (uint32_t)(LSBAudioFrames - used);
    for (uint32_t frame = 0; frame < frames; ++frame) for (unsigned channel = 0; channel < LSBAudioChannels; ++channel) {
        uint32_t bits;
        memcpy(&bits, &samples[frame * LSBAudioChannels + channel], sizeof(bits));
        size_t index = ((*cursor + frame) % LSBAudioFrames) * LSBAudioChannels + channel;
        atomic_store_explicit(&ring->samples[index], bits, memory_order_relaxed);
    }
    *cursor += frames;
    atomic_store_explicit(&ring->written, *cursor, memory_order_release);
    return frames;
}
uint32_t LSBAudioRingRead(LSBAudioRing *ring, uint64_t *cursor, float *samples, uint32_t frames) {
    if (!ring || !cursor || !samples || frames > LSBAudioFrames) return 0;
    memset(samples, 0, frames * LSBAudioChannels * sizeof(float));
    if (atomic_load_explicit(&ring->closed, memory_order_acquire)) return 0;
    uint64_t written = atomic_load_explicit(&ring->written, memory_order_acquire), available = written - *cursor;
    if (available > LSBAudioFrames) {
        atomic_store_explicit(&ring->closed, 1, memory_order_release);
        return 0;
    }
    uint32_t count = available < frames ? (uint32_t)available : frames;
    for (uint32_t frame = 0; frame < count; ++frame) for (unsigned channel = 0; channel < LSBAudioChannels; ++channel) {
        size_t index = ((*cursor + frame) % LSBAudioFrames) * LSBAudioChannels + channel;
        uint32_t bits = atomic_load_explicit(&ring->samples[index], memory_order_relaxed);
        float sample;
        memcpy(&sample, &bits, sizeof(sample));
        samples[frame * LSBAudioChannels + channel] = !isfinite(sample) ? 0 : fmaxf(-1, fminf(1, sample));
    }
    *cursor += count;
    atomic_store_explicit(&ring->read, *cursor, memory_order_release);
    atomic_fetch_add_explicit(&ring->rendered, count, memory_order_relaxed);
    atomic_fetch_add_explicit(&ring->underrun, frames - count, memory_order_relaxed);
    return count;
}
