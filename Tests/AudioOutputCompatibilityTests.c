#include "../Guest/AudioOutputCompatibility.h"
#include "../Guest/AudioOutputTransport.h"
#include <assert.h>
#include <errno.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static unsigned forwarded;
static const OSStatus forwardedStatus = -777;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#define FORWARD(name, arguments) OSStatus name arguments { ++forwarded; return forwardedStatus; }
Boolean AudioObjectExists(AudioObjectID object) { ++forwarded; return false; }
Boolean AudioObjectHasProperty(AudioObjectID object, const AudioObjectPropertyAddress *address) { ++forwarded; return false; }
FORWARD(AudioObjectIsPropertySettable, (AudioObjectID o, const AudioObjectPropertyAddress *a, Boolean *s))
FORWARD(AudioObjectGetPropertyDataSize, (AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 *s))
FORWARD(AudioObjectGetPropertyData, (AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 *s, void *d))
FORWARD(AudioObjectSetPropertyData, (AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 s, const void *d))
FORWARD(AudioObjectAddPropertyListener, (AudioObjectID o, const AudioObjectPropertyAddress *a, LSBAudioPropertyListener l, void *c))
FORWARD(AudioObjectAddPropertyListenerInternal, (AudioObjectID o, const AudioObjectPropertyAddress *a, LSBAudioPropertyListener l, void *c))
FORWARD(AudioObjectRemovePropertyListener, (AudioObjectID o, const AudioObjectPropertyAddress *a, LSBAudioPropertyListener l, void *c))
FORWARD(AudioObjectAddPropertyListenerBlock, (AudioObjectID o, const AudioObjectPropertyAddress *a, dispatch_queue_t q, LSBAudioPropertyListenerBlock l))
FORWARD(AudioObjectRemovePropertyListenerBlock, (AudioObjectID o, const AudioObjectPropertyAddress *a, dispatch_queue_t q, LSBAudioPropertyListenerBlock l))
FORWARD(AudioHardwareCreateAggregateDevice, (CFDictionaryRef d, AudioObjectID *o))
FORWARD(AudioHardwareDestroyAggregateDevice, (AudioObjectID o))
FORWARD(AudioDeviceCreateIOProcID, (AudioObjectID o, LSBAudioDeviceIOProc p, void *c, LSBAudioDeviceIOProcID *i))
FORWARD(AudioDeviceDestroyIOProcID, (AudioObjectID o, LSBAudioDeviceIOProcID i))
FORWARD(AudioDeviceStart, (AudioObjectID o, LSBAudioDeviceIOProcID i))
FORWARD(AudioDeviceStartAtTime, (AudioObjectID o, LSBAudioDeviceIOProcID i, AudioTimeStamp *t, UInt32 f))
FORWARD(AudioDeviceStop, (AudioObjectID o, LSBAudioDeviceIOProcID i))
FORWARD(AudioDeviceGetCurrentTime, (AudioObjectID o, AudioTimeStamp *t))
FORWARD(AudioDeviceTranslateTime, (AudioObjectID o, const AudioTimeStamp *i, AudioTimeStamp *t))
FORWARD(AudioDeviceGetNearestStartTime, (AudioObjectID o, AudioTimeStamp *t, UInt32 f))
#undef FORWARD
#pragma clang diagnostic pop

static pthread_mutex_t testLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t testChanged = PTHREAD_COND_INITIALIZER;
static unsigned opens, writes, closes, callbacks;
static bool transportOpen, failOpen, holdCallback, enteredCallback, releaseCallback;
static float lastSample;
static AudioObjectID expectedDevice = LSB_AUDIO_OUTPUT_DEVICE;
static uint64_t recordedTimes[32];
static unsigned recordedTimeCount;
static bool recordTimes;
static LSBAudioDeviceIOProcID selfDestroy;
static _Atomic(bool) stopReturned;

int LSBGuestAudioOutputOpen(void **stream) {
    pthread_mutex_lock(&testLock);
    assert(!transportOpen);
    if (failOpen) { *stream = NULL; pthread_mutex_unlock(&testLock); return EACCES; }
    transportOpen = true;
    ++opens;
    *stream = &transportOpen;
    pthread_mutex_unlock(&testLock);
    return 0;
}
uint32_t LSBGuestAudioOutputWrite(void *stream, const float *samples, uint32_t frames, uint64_t hostTime) {
    pthread_mutex_lock(&testLock);
    assert(transportOpen && stream == &transportOpen && samples && frames == LSB_AUDIO_OUTPUT_FRAMES && hostTime);
    ++writes;
    if (recordTimes && recordedTimeCount < sizeof(recordedTimes) / sizeof(*recordedTimes)) recordedTimes[recordedTimeCount++] = hostTime;
    lastSample = samples[0];
    pthread_cond_broadcast(&testChanged);
    pthread_mutex_unlock(&testLock);
    return frames / 2; // A full ring may accept less; the HAL worker must neither retry nor block.
}
void LSBGuestAudioOutputClose(void *stream) {
    pthread_mutex_lock(&testLock);
    assert(transportOpen && stream == &transportOpen);
    transportOpen = false;
    ++closes;
    pthread_cond_broadcast(&testChanged);
    pthread_mutex_unlock(&testLock);
}

static void waitCount(unsigned *value, unsigned minimum) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 3;
    pthread_mutex_lock(&testLock);
    while (*value < minimum) assert(pthread_cond_timedwait(&testChanged, &testLock, &deadline) == 0);
    pthread_mutex_unlock(&testLock);
}

static OSStatus render(AudioObjectID object, const AudioTimeStamp *now, const AudioBufferList *input, const AudioTimeStamp *inputTime,
                       AudioBufferList *output, const AudioTimeStamp *outputTime, void *context) {
    assert(object == expectedDevice && now->mHostTime && now == inputTime && now == outputTime && input->mNumberBuffers == 0);
    assert(output->mNumberBuffers == 1 && output->mBuffers[0].mNumberChannels == 2);
    assert(output->mBuffers[0].mDataByteSize == LSB_AUDIO_OUTPUT_FRAMES * 2 * sizeof(float));
    float *samples = output->mBuffers[0].mData;
    for (unsigned i = 0; i < LSB_AUDIO_OUTPUT_FRAMES * 2; ++i) samples[i] = *(float *)context;
    pthread_mutex_lock(&testLock);
    ++callbacks;
    enteredCallback = true;
    pthread_cond_broadcast(&testChanged);
    while (holdCallback && !releaseCallback) pthread_cond_wait(&testChanged, &testLock);
    LSBAudioDeviceIOProcID retiring = selfDestroy;
    selfDestroy = NULL;
    pthread_mutex_unlock(&testLock);
    if (retiring) assert(LSBAudioDeviceDestroyIOProcID(object, retiring) == 0);
    return 0;
}

static void testProperties(void) {
    AudioObjectPropertyAddress address = {'dOut', 'glob', 0};
    UInt32 size = 4, identifier = 0;
    unsetenv(LSB_AUDIO_OUTPUT_ENABLE_ENV);
    assert(LSBAudioObjectGetPropertyData(1, &address, 0, NULL, &size, &identifier) == forwardedStatus && forwarded == 1);
    setenv(LSB_AUDIO_OUTPUT_ENABLE_ENV, "true", 1);
    assert(LSBAudioObjectGetPropertyData(1, &address, 0, NULL, &size, &identifier) == forwardedStatus && forwarded == 2);
    setenv(LSB_AUDIO_OUTPUT_ENABLE_ENV, "1", 1);
    assert(LSBAudioObjectGetPropertyData(1, &address, 0, NULL, &size, &identifier) == 0 && identifier == LSB_AUDIO_OUTPUT_DEVICE);
    assert(LSBAudioObjectExists(identifier));
    address.mSelector = 'dIn ';
    assert(LSBAudioObjectGetPropertyData(1, &address, 0, NULL, &size, &identifier) == 0 && identifier == kAudioObjectUnknown);
    address.mSelector = 'sfmt';
    address.mScope = 'outp';
    size = 0;
    assert(LSBAudioObjectGetPropertyDataSize(LSB_AUDIO_OUTPUT_STREAM, &address, 0, NULL, &size) == 0);
    assert(size == sizeof(AudioStreamBasicDescription));
    AudioStreamBasicDescription format;
    memset(&format, 0xaa, sizeof(format));
    UInt32 shortSize = 1;
    assert(LSBAudioObjectGetPropertyData(LSB_AUDIO_OUTPUT_STREAM, &address, 0, NULL, &shortSize, &format) == kAudioHardwareBadPropertySizeError);
    assert(shortSize == sizeof(format) && ((unsigned char *)&format)[0] == 0xaa);
    assert(LSBAudioObjectGetPropertyData(LSB_AUDIO_OUTPUT_STREAM, &address, 0, NULL, &size, &format) == 0);
    assert(format.mSampleRate == 48000 && format.mChannelsPerFrame == 2 && format.mBytesPerFrame == 8 && format.mBitsPerChannel == 32);
    assert(LSBAudioObjectSetPropertyData(LSB_AUDIO_OUTPUT_STREAM, &address, 0, NULL, size, &format) == 0);
    format.mSampleRate = 44100;
    assert(LSBAudioObjectSetPropertyData(LSB_AUDIO_OUTPUT_STREAM, &address, 0, NULL, size, &format) == kAudioHardwareUnsupportedOperationError);
    address = (AudioObjectPropertyAddress){'slay', 'inpt', 0};
    AudioBufferList buffers = {0};
    size = sizeof(buffers);
    assert(LSBAudioObjectGetPropertyData(LSB_AUDIO_OUTPUT_DEVICE, &address, 0, NULL, &size, &buffers) == 0);
    assert(!buffers.mNumberBuffers && size == offsetof(AudioBufferList, mBuffers));
    address = (AudioObjectPropertyAddress){'zzzz', 'glob', 0};
    assert(!LSBAudioObjectHasProperty(LSB_AUDIO_OUTPUT_DEVICE, &address));
    assert(LSBAudioObjectGetPropertyData(LSB_AUDIO_OUTPUT_DEVICE, &address, 0, NULL, &size, &buffers) == kAudioHardwareUnknownPropertyError);
    assert(LSBAudioObjectGetPropertyData(1024, &address, 0, NULL, &size, &buffers) == forwardedStatus);
    address.mSelector = 'uid ';
    CFStringRef uid = NULL;
    size = sizeof(uid);
    assert(LSBAudioObjectGetPropertyData(LSB_AUDIO_OUTPUT_DEVICE, &address, 0, NULL, &size, &uid) == 0);
    assert(CFEqual(uid, CFSTR("LiveSimBooterOutput")));
    CFRelease(uid);
}

static CFMutableDictionaryRef aggregateDescription(void) {
    const void *childKeys[] = {CFSTR("uid")}, *childValues[] = {CFSTR("LiveSimBooterOutput")};
    CFDictionaryRef child = CFDictionaryCreate(NULL, childKeys, childValues, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFArrayRef children = CFArrayCreate(NULL, (const void **)&child, 1, &kCFTypeArrayCallBacks);
    CFMutableDictionaryRef description = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(description, CFSTR("name"), CFSTR("iOSSimulatorAudioDevice-123-1"));
    CFDictionarySetValue(description, CFSTR("uid"), CFSTR("iOSSimulatorAudioDevice-123-1"));
    CFDictionarySetValue(description, CFSTR("stacked"), kCFBooleanFalse);
    CFDictionarySetValue(description, CFSTR("subdevices"), children);
    CFRelease(children); CFRelease(child);
    return description;
}

static UInt32 readInteger(AudioObjectID object, UInt32 selector) {
    AudioObjectPropertyAddress address = {selector, 'glob', 0};
    UInt32 value = 0, size = sizeof(value);
    assert(LSBAudioObjectGetPropertyData(object, &address, 0, NULL, &size, &value) == 0 && size == sizeof(value));
    return value;
}

static void testLegacyUID(CFStringRef uid, AudioObjectID expected) {
    AudioObjectPropertyAddress address = {'duid', 'glob', 0};
    UInt32 size = 0;
    assert(LSBAudioObjectHasProperty(1, &address));
    assert(!LSBAudioObjectGetPropertyDataSize(1, &address, 0, NULL, &size) && size == sizeof(AudioValueTranslation));
    AudioObjectID output = 0xabababab;
    AudioValueTranslation translation = {&uid, sizeof(uid), &output, sizeof(output)};
    UInt32 shortSize = 1;
    assert(LSBAudioObjectGetPropertyData(1, &address, 0, NULL, &shortSize, &translation) == kAudioHardwareBadPropertySizeError);
    assert(shortSize == sizeof(translation) && output == 0xabababab);
    translation.mOutputDataSize = 1;
    assert(LSBAudioObjectGetPropertyData(1, &address, 0, NULL, &size, &translation) == kAudioHardwareBadPropertySizeError && output == 0xabababab);
    translation.mOutputDataSize = sizeof(output);
    translation.mInputDataSize = 1;
    assert(LSBAudioObjectGetPropertyData(1, &address, 0, NULL, &size, &translation) == kAudioHardwareBadPropertySizeError && output == 0xabababab);
    translation.mInputDataSize = sizeof(uid);
    assert(!LSBAudioObjectGetPropertyData(1, &address, 0, NULL, &size, &translation) && output == expected);
    assert(translation.mInputData == &uid && translation.mOutputData == &output);
}

static void testAggregate(void) {
    CFMutableDictionaryRef description = aggregateDescription();
    AudioObjectID identifier = 0;
    CFDictionarySetValue(description, CFSTR("stacked"), kCFBooleanTrue);
    assert(LSBAudioHardwareCreateAggregateDevice(description, &identifier) == kAudioHardwareUnsupportedOperationError && !identifier);
    CFDictionarySetValue(description, CFSTR("stacked"), kCFBooleanFalse);
    assert(!LSBAudioHardwareCreateAggregateDevice(description, &identifier));
    assert(identifier != LSB_AUDIO_OUTPUT_DEVICE && LSBAudioObjectExists(identifier) && LSBAudioObjectExists(identifier + 1));
    AudioObjectID duplicate = UINT32_MAX;
    assert(LSBAudioHardwareCreateAggregateDevice(description, &duplicate) == kAudioHardwareIllegalOperationError && !duplicate);
    CFMutableDictionaryRef otherDescription = aggregateDescription();
    CFDictionarySetValue(otherDescription, CFSTR("name"), CFSTR("iOSSimulatorAudioDevice-123-2"));
    CFDictionarySetValue(otherDescription, CFSTR("uid"), CFSTR("iOSSimulatorAudioDevice-123-2"));
    AudioObjectID otherDevice = 0;
    assert(!LSBAudioHardwareCreateAggregateDevice(otherDescription, &otherDevice) && otherDevice != identifier);
    CFRelease(otherDescription);
    assert(readInteger(identifier, 'clas') == 'aagg' && readInteger(identifier, 'bcls') == 'adev');
    assert(readInteger(identifier + 1, 'stdv') == identifier && readInteger(identifier, 'stm#') == identifier + 1);
    AudioObjectPropertyAddress address = {'acom', 'glob', 0};
    assert(!LSBAudioObjectHasProperty(LSB_AUDIO_OUTPUT_DEVICE, &address)); // No self-referencing child composition.
    UInt32 size = 0;
    assert(!LSBAudioObjectGetPropertyDataSize(identifier, &address, 0, NULL, &size) && size == sizeof(CFDictionaryRef));
    CFDictionaryRef composition = NULL;
    assert(!LSBAudioObjectGetPropertyData(identifier, &address, 0, NULL, &size, &composition) && CFEqual(composition, description));
    CFDictionaryRemoveValue(description, CFSTR("subdevices"));
    assert(CFDictionaryContainsKey(composition, CFSTR("subdevices"))); // Deep immutable snapshot, not caller's mutable dictionary.
    CFStringRef uid = CFDictionaryGetValue(composition, CFSTR("uid"));
    testLegacyUID(uid, identifier);
    testLegacyUID(CFSTR("LiveSimBooterOutput"), LSB_AUDIO_OUTPUT_DEVICE);
    testLegacyUID(CFSTR("missing-device"), kAudioObjectUnknown);
    address.mSelector = 'uidd';
    AudioObjectID translated = 0;
    size = sizeof(translated);
    assert(!LSBAudioObjectGetPropertyData(1, &address, sizeof(uid), &uid, &size, &translated) && translated == identifier);
    address = (AudioObjectPropertyAddress){'srnd', 'outp', 0};
    AudioChannelLayout layout = {0}; size = sizeof(layout);
    assert(!LSBAudioObjectGetPropertyData(identifier, &address, 0, NULL, &size, &layout));
    assert(layout.mChannelLayoutTag == kAudioChannelLayoutTag_Stereo && size == offsetof(AudioChannelLayout, mChannelDescriptions));
    address = (AudioObjectPropertyAddress){'sfmt', 'outp', 0};
    AudioStreamBasicDescription format; size = sizeof(format);
    assert(!LSBAudioObjectGetPropertyData(identifier + 1, &address, 0, NULL, &size, &format));
    assert(!LSBAudioObjectSetPropertyData(identifier + 1, &address, 0, NULL, size, &format));
    float level = 0.25f;
    LSBAudioDeviceIOProcID proc;
    assert(!LSBAudioDeviceCreateIOProcID(identifier, render, &level, &proc));
    assert(LSBAudioHardwareDestroyAggregateDevice(identifier) == kAudioHardwareIllegalOperationError);
    assert(LSBAudioDeviceStart(LSB_AUDIO_OUTPUT_DEVICE, proc) == kAudioHardwareIllegalOperationError);
    assert(!readInteger(identifier, 'goin') && !readInteger(otherDevice, 'gone') && !readInteger(LSB_AUDIO_OUTPUT_DEVICE, 'goin'));
    expectedDevice = identifier;
    pthread_mutex_lock(&testLock);
    unsigned targetWrites = writes + 8;
    recordTimes = true; recordedTimeCount = 0;
    pthread_mutex_unlock(&testLock);
    assert(!LSBAudioDeviceStart(identifier, proc));
    waitCount(&writes, targetWrites);
    assert(readInteger(identifier, 'goin') == 1 && readInteger(identifier, 'gone') == 1);
    assert(!readInteger(otherDevice, 'goin') && !readInteger(otherDevice, 'gone'));
    assert(readInteger(LSB_AUDIO_OUTPUT_DEVICE, 'goin') == 1); // Physical output carries active aggregate samples.
    assert(!LSBAudioDeviceStop(identifier, proc));
    assert(!readInteger(identifier, 'goin') && !readInteger(otherDevice, 'gone') && !readInteger(LSB_AUDIO_OUTPUT_DEVICE, 'goin'));
    assert(!LSBAudioDeviceDestroyIOProcID(identifier, proc));
    expectedDevice = LSB_AUDIO_OUTPUT_DEVICE;
    pthread_mutex_lock(&testLock);
    recordTimes = false;
    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);
    uint64_t interval = (uint64_t)LSB_AUDIO_OUTPUT_FRAMES * 1000000000ull * timebase.denom / (LSB_AUDIO_OUTPUT_RATE * (uint64_t)timebase.numer);
    assert(recordedTimeCount >= 8);
    unsigned steadyIntervals = 0;
    for (unsigned i = 1; i < recordedTimeCount; ++i) {
        uint64_t delta = recordedTimes[i] - recordedTimes[i - 1];
        if (delta == interval) ++steadyIntervals;
        else assert(delta > interval * 4); // A real long scheduler pause may reset; ordinary wake jitter must not.
    }
    assert(steadyIntervals >= 3);
    pthread_mutex_unlock(&testLock);
    assert(!LSBAudioHardwareDestroyAggregateDevice(identifier));
    assert(!LSBAudioHardwareDestroyAggregateDevice(otherDevice));
    testLegacyUID(uid, kAudioObjectUnknown);
    assert(!LSBAudioObjectExists(identifier) && !LSBAudioObjectExists(identifier + 1));
    assert(CFDictionaryContainsKey(composition, CFSTR("subdevices"))); // Returned CF ownership survives device destruction.
    address = (AudioObjectPropertyAddress){'acom', 'glob', 0}; size = sizeof(composition);
    assert(LSBAudioObjectGetPropertyData(identifier, &address, 0, NULL, &size, &uid) == kAudioHardwareBadObjectError);
    AudioTimeStamp timestamp;
    assert(LSBAudioDeviceGetCurrentTime(identifier, &timestamp) == kAudioHardwareBadDeviceError);
    CFRelease(composition); CFRelease(description);
}

static void *stopFromAnotherThread(void *argument) {
    assert(LSBAudioDeviceStop(LSB_AUDIO_OUTPUT_DEVICE, (LSBAudioDeviceIOProcID)argument) == 0);
    atomic_store(&stopReturned, true);
    return NULL;
}

static void testRendering(void) {
    float level = 0.25f;
    LSBAudioDeviceIOProcID identifier;
    assert(LSBAudioDeviceCreateIOProcID(LSB_AUDIO_OUTPUT_DEVICE, render, &level, &identifier) == 0);
    failOpen = true;
    assert(LSBAudioDeviceStart(LSB_AUDIO_OUTPUT_DEVICE, identifier) == kAudioHardwareNotReadyError);
    failOpen = false;
    assert(LSBAudioDeviceStart(LSB_AUDIO_OUTPUT_DEVICE, identifier) == 0);
    assert(LSBAudioDeviceStart(LSB_AUDIO_OUTPUT_DEVICE, identifier) == 0);
    waitCount(&writes, 3);
    assert(LSBAudioDeviceStop(LSB_AUDIO_OUTPUT_DEVICE, identifier) == 0);
    pthread_mutex_lock(&testLock);
    assert(opens == 1 && closes == 1 && !transportOpen && lastSample == 0.25f);
    unsigned stoppedWrites = writes;
    pthread_mutex_unlock(&testLock);
    usleep(25000);
    pthread_mutex_lock(&testLock);
    assert(writes == stoppedWrites);
    holdCallback = true; releaseCallback = false; enteredCallback = false;
    unsigned expectedCallbacks = callbacks + 1;
    pthread_mutex_unlock(&testLock);
    assert(LSBAudioDeviceStart(LSB_AUDIO_OUTPUT_DEVICE, identifier) == 0);
    waitCount(&callbacks, expectedCallbacks);
    pthread_t stopper;
    assert(pthread_create(&stopper, NULL, stopFromAnotherThread, (void *)identifier) == 0);
    usleep(20000);
    assert(!atomic_load(&stopReturned));
    pthread_mutex_lock(&testLock);
    releaseCallback = true; holdCallback = false;
    pthread_cond_broadcast(&testChanged);
    pthread_mutex_unlock(&testLock);
    pthread_join(stopper, NULL);
    assert(atomic_load(&stopReturned));
    assert(LSBAudioDeviceDestroyIOProcID(LSB_AUDIO_OUTPUT_DEVICE, identifier) == 0);
    assert(LSBAudioDeviceStart(LSB_AUDIO_OUTPUT_DEVICE, identifier) == kAudioHardwareIllegalOperationError);
    assert(LSBAudioDeviceCreateIOProcID(LSB_AUDIO_OUTPUT_DEVICE, render, &level, &identifier) == 0);
    pthread_mutex_lock(&testLock);
    selfDestroy = identifier;
    unsigned expectedCloses = closes + 1;
    pthread_mutex_unlock(&testLock);
    assert(LSBAudioDeviceStart(LSB_AUDIO_OUTPUT_DEVICE, identifier) == 0);
    waitCount(&closes, expectedCloses);
    assert(LSBAudioDeviceStart(LSB_AUDIO_OUTPUT_DEVICE, identifier) == kAudioHardwareIllegalOperationError);

    LSBAudioDeviceIOProcID identifiers[LSB_AUDIO_OUTPUT_MAX_PROCS];
    for (unsigned i = 0; i < LSB_AUDIO_OUTPUT_MAX_PROCS; ++i)
        assert(LSBAudioDeviceCreateIOProcID(LSB_AUDIO_OUTPUT_DEVICE, render, &level, &identifiers[i]) == 0);
    assert(LSBAudioDeviceCreateIOProcID(LSB_AUDIO_OUTPUT_DEVICE, render, &level, &identifier) == kAudioHardwareIllegalOperationError);
    for (unsigned i = 0; i < LSB_AUDIO_OUTPUT_MAX_PROCS; ++i) assert(LSBAudioDeviceDestroyIOProcID(LSB_AUDIO_OUTPUT_DEVICE, identifiers[i]) == 0);
}

int main(void) {
    testProperties();
    testRendering();
    testAggregate();
    puts("Audio output compatibility tests passed");
    return 0;
}
