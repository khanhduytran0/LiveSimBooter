#include "AudioOutputTransport.h"
#include "../ProcessSupport/AudioOutput.h"
#include "../ProcessSupport/SpawnTransport.h"
#include <errno.h>
#include <stdlib.h>
#include <sys/mman.h>

typedef struct { LSBAudioRing *ring; size_t size; uint64_t cursor; } AudioStream;
int LSBGuestAudioOutputOpen(void **stream) {
    if (!stream) return EINVAL;
    *stream = NULL;
    AudioStream *value = calloc(1, sizeof(*value));
    if (!value) return ENOMEM;
    void *mapping = NULL;
    int error = LSBTransportOpenAudioOutput(&mapping, &value->size);
    value->ring = mapping;
    if (!error) error = LSBAudioRingValidate(value->ring, value->size);
    if (error) {
        if (mapping) { LSBTransportCloseAudioOutput(); munmap(mapping, value->size); }
        free(value);
    } else *stream = value;
    return error;
}
uint32_t LSBGuestAudioOutputWrite(void *stream, const float *samples, uint32_t frames, uint64_t hostTime) {
    (void)hostTime;
    AudioStream *value = stream;
    return value ? LSBAudioRingWrite(value->ring, &value->cursor, samples, frames) : 0;
}
void LSBGuestAudioOutputClose(void *stream) {
    AudioStream *value = stream;
    if (!value) return;
    atomic_store_explicit(&value->ring->closed, 1, memory_order_release);
    LSBTransportCloseAudioOutput();
    munmap(value->ring, value->size);
    free(value);
}
