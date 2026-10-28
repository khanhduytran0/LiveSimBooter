#ifndef LSB_AUDIO_OUTPUT_TRANSPORT_H
#define LSB_AUDIO_OUTPUT_TRANSPORT_H

#include <stdint.h>

// Fixed output-only PCM contract. Open/Close may block; Write must not allocate, block, or perform IPC.
// Write copies accepted samples before returning; the caller retains its buffer and drops any unaccepted tail.
int LSBGuestAudioOutputOpen(void **stream);
uint32_t LSBGuestAudioOutputWrite(void *stream, const float *interleavedStereo, uint32_t frames, uint64_t hostTime);
void LSBGuestAudioOutputClose(void *stream);

#endif
