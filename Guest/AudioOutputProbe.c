#include "AudioOutputCompatibility.h"
#include <AudioToolbox/AudioToolbox.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static uint64_t phase;
static _Atomic(unsigned) callbacks;
static void fill(float *samples, unsigned frames) {
    for (unsigned i = 0; i < frames; ++i, ++phase) {
        // A quiet, finite 660 Hz tone; zero tail gives the queue a clean ending.
        double envelope = phase < 4800 ? (double)phase / 4800 : phase > 43200 ? fmax(0, (48000.0 - phase) / 4800) : 1;
        float sample = phase < 48000 ? (float)(0.12 * envelope * sin(phase * (2 * M_PI * 660 / 48000))) : 0;
        samples[i * 2] = samples[i * 2 + 1] = sample;
    }
    atomic_fetch_add(&callbacks, 1);
}
static void queueCallback(void *context, AudioQueueRef queue, AudioQueueBufferRef buffer) {
    (void)context;
    fill(buffer->mAudioData, 512);
    buffer->mAudioDataByteSize = 512 * 2 * sizeof(float);
    AudioQueueEnqueueBuffer(queue, buffer, 0, NULL);
}
static OSStatus deviceCallback(AudioObjectID device, const AudioTimeStamp *now, const AudioBufferList *input,
    const AudioTimeStamp *inputTime, AudioBufferList *output, const AudioTimeStamp *outputTime, void *context) {
    (void)device; (void)now; (void)input; (void)inputTime; (void)outputTime; (void)context;
    if (output->mNumberBuffers != 1 || output->mBuffers[0].mNumberChannels != 2) return -1;
    fill(output->mBuffers[0].mData, output->mBuffers[0].mDataByteSize / 8);
    return 0;
}
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(15);
    bool direct = argc == 2 && !strcmp(argv[1], "--direct");
    if (argc > 2 || (argc == 2 && !direct)) return 2;
    OSStatus status;
    if (direct) {
        AudioObjectID device = 0;
        UInt32 size = sizeof(device);
        AudioObjectPropertyAddress address = {'dOut', 'glob', 0};
        status = AudioObjectGetPropertyData(1, &address, 0, NULL, &size, &device);
        printf("audio-probe default-output=%u status=%d\n", device, (int)status);
        LSBAudioDeviceIOProcID proc = NULL;
        if (!status) status = AudioDeviceCreateIOProcID(device, deviceCallback, NULL, &proc);
        if (!status) status = AudioDeviceStart(device, proc);
        printf("audio-probe direct-start=%d\n", (int)status);
        if (!status) usleep(1400000);
        if (proc) { AudioDeviceStop(device, proc); AudioDeviceDestroyIOProcID(device, proc); }
    } else {
        AudioStreamBasicDescription format = {.mSampleRate = 48000, .mFormatID = kAudioFormatLinearPCM,
            .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked, .mBytesPerPacket = 8, .mFramesPerPacket = 1,
            .mBytesPerFrame = 8, .mChannelsPerFrame = 2, .mBitsPerChannel = 32};
        AudioQueueRef queue = NULL;
        status = AudioQueueNewOutput(&format, queueCallback, NULL, NULL, NULL, 0, &queue);
        printf("audio-probe queue-create=%d\n", (int)status);
        for (unsigned i = 0; !status && i < 3; ++i) {
            AudioQueueBufferRef buffer = NULL;
            status = AudioQueueAllocateBuffer(queue, 512 * 8, &buffer);
            if (!status) queueCallback(NULL, queue, buffer);
        }
        if (!status) status = AudioQueueStart(queue, NULL);
        printf("audio-probe queue-start=%d\n", (int)status);
        if (!status) usleep(1400000);
        if (queue) AudioQueueDispose(queue, true);
    }
    printf("audio-probe callbacks=%u result=%d\n", atomic_load(&callbacks), (int)status);
    return status ? 1 : atomic_load(&callbacks) < 20 ? 3 : 0;
}
