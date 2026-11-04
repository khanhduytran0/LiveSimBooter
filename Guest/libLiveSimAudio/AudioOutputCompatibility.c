#include "AudioOutputCompatibility.h"
#include "AudioOutputTransport.h"
#include <errno.h>
#include <mach/mach_time.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 23A343 AudioToolbox's AQMEIO_HAL ultimately consumes an AudioDevice IOProc. Interpose that output boundary;
// guest AudioQueue/RemoteIO, converters and mixers remain stock. No native audio code runs on a guest thread.
typedef struct {
    uintptr_t identifier;
    AudioObjectID device;
    LSBAudioDeviceIOProc callback;
    void *context;
    bool running, executing, retiring, releaseAfterCallback;
} OutputProc;
typedef struct { AudioObjectID identifier; CFDictionaryRef composition; } OutputAggregate;

static pthread_mutex_t outputLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t outputChanged = PTHREAD_COND_INITIALIZER;
static OutputProc outputProcs[LSB_AUDIO_OUTPUT_MAX_PROCS];
static OutputAggregate outputAggregates[LSB_AUDIO_OUTPUT_MAX_AGGREGATES];
static AudioObjectID nextAggregate = LSB_AUDIO_OUTPUT_AGGREGATE_FIRST;
static uintptr_t nextIdentifier = 1;
static pthread_t outputThread;
static bool hasOutputThread;
static void *outputStream;
static uint64_t outputEpoch;
static _Atomic(uint32_t) runningCount;
static _Atomic(unsigned) traceEvents;

static bool enabled(void) {
    const char *value = getenv(LSB_AUDIO_OUTPUT_ENABLE_ENV);
    return value && !strcmp(value, "1");
}
static bool aggregateObject(AudioObjectID object) { return object >= LSB_AUDIO_OUTPUT_AGGREGATE_FIRST && object < LSB_AUDIO_OUTPUT_AGGREGATE_LIMIT; }
static bool device(AudioObjectID object) { return object == LSB_AUDIO_OUTPUT_DEVICE || (aggregateObject(object) && !(object & 1)); }
static bool ours(AudioObjectID object) { return device(object) || object == LSB_AUDIO_OUTPUT_STREAM || aggregateObject(object); }
static OutputAggregate *findAggregate(AudioObjectID object) {
    object &= ~1u; // Every aggregate owns the following odd-numbered stream ID.
    for (unsigned i = 0; i < LSB_AUDIO_OUTPUT_MAX_AGGREGATES; ++i)
        if (outputAggregates[i].identifier == object) return &outputAggregates[i];
    return NULL;
}
static bool objectExists(AudioObjectID object) {
    if (!aggregateObject(object)) return object == 1 || ours(object);
    pthread_mutex_lock(&outputLock);
    bool exists = findAggregate(object) != NULL;
    pthread_mutex_unlock(&outputLock);
    return exists;
}

static void traceProperty(const char *operation, AudioObjectID object, const AudioObjectPropertyAddress *address, OSStatus status) {
    const char *trace = getenv("LSB_AUDIO_TRACE");
    if (!trace || strcmp(trace, "1") || atomic_fetch_add(&traceEvents, 1) >= 96) return;
    UInt32 selector = address ? address->mSelector : 0, scope = address ? address->mScope : 0;
    char text[5];
    for (unsigned i = 0; i < 4; ++i) { unsigned byte = (selector >> ((3 - i) * 8)) & 255; text[i] = byte >= 32 && byte < 127 ? byte : '.'; }
    text[4] = '\0';
    fprintf(stderr, "[audio-hal] %s object=0x%x selector='%s'/0x%x scope=0x%x element=%u status=%d\n", operation,
        object, text, selector, scope, address ? address->mElement : 0, (int)status);
}

static AudioStreamBasicDescription outputFormat(void) {
    return (AudioStreamBasicDescription){.mSampleRate = LSB_AUDIO_OUTPUT_RATE, .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked, .mBytesPerPacket = 8, .mFramesPerPacket = 1,
        .mBytesPerFrame = 8, .mChannelsPerFrame = LSB_AUDIO_OUTPUT_CHANNELS, .mBitsPerChannel = 32};
}

static uint64_t ticksPerBlock(void) {
    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);
    return (uint64_t)LSB_AUDIO_OUTPUT_FRAMES * 1000000000ull * timebase.denom / (LSB_AUDIO_OUTPUT_RATE * (uint64_t)timebase.numer);
}

static void fillTime(AudioTimeStamp *time, uint64_t hostTime) {
    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);
    pthread_mutex_lock(&outputLock);
    if (!outputEpoch) outputEpoch = hostTime;
    uint64_t epoch = outputEpoch;
    pthread_mutex_unlock(&outputLock);
    *time = (AudioTimeStamp){.mHostTime = hostTime, .mSampleTime = ((double)hostTime - (double)epoch) * timebase.numer / timebase.denom *
        (LSB_AUDIO_OUTPUT_RATE / 1e9), .mRateScalar = 1, .mFlags = kAudioTimeStampSampleTimeValid | kAudioTimeStampHostTimeValid |
        kAudioTimeStampRateScalarValid};
}

static OutputProc *findProc(LSBAudioDeviceIOProcID identifier) {
    for (unsigned i = 0; i < LSB_AUDIO_OUTPUT_MAX_PROCS; ++i)
        if (outputProcs[i].identifier == (uintptr_t)identifier && identifier && !outputProcs[i].retiring) return &outputProcs[i];
    return NULL;
}

static void *renderOutput(void *unused) {
    (void)unused;
    pthread_setname_np("LiveSimBooter audio output");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    const uint64_t interval = ticksPerBlock();
    float mixed[LSB_AUDIO_OUTPUT_FRAMES * LSB_AUDIO_OUTPUT_CHANNELS], scratch[LSB_AUDIO_OUTPUT_FRAMES * LSB_AUDIO_OUTPUT_CHANNELS];
    uint64_t deadline = 0;
    for (;;) {
        pthread_mutex_lock(&outputLock);
        while (!runningCount) {
            if (outputStream) { LSBGuestAudioOutputClose(outputStream); outputStream = NULL; }
            pthread_cond_broadcast(&outputChanged);
            pthread_cond_wait(&outputChanged, &outputLock);
            deadline = 0;
        }
        void *stream = outputStream;
        pthread_mutex_unlock(&outputLock);
        uint64_t now = mach_absolute_time();
        // Normal scheduling jitter must not move the sample clock on every block. Catch up at most four quanta after a delay.
        if (!deadline || (now > deadline && now - deadline > interval * 4)) deadline = now;
        AudioTimeStamp timestamp;
        fillTime(&timestamp, deadline);
        memset(mixed, 0, sizeof(mixed));
        for (unsigned i = 0; i < LSB_AUDIO_OUTPUT_MAX_PROCS; ++i) {
            pthread_mutex_lock(&outputLock);
            OutputProc *proc = &outputProcs[i];
            if (!proc->running) { pthread_mutex_unlock(&outputLock); continue; }
            proc->executing = true;
            LSBAudioDeviceIOProc callback = proc->callback;
            AudioObjectID callbackDevice = proc->device;
            void *context = proc->context;
            pthread_mutex_unlock(&outputLock);
            memset(scratch, 0, sizeof(scratch));
            AudioBufferList input = {.mNumberBuffers = 0};
            AudioBufferList output = {.mNumberBuffers = 1, .mBuffers = {{LSB_AUDIO_OUTPUT_CHANNELS, sizeof(scratch), scratch}}};
            OSStatus result = callback(callbackDevice, &timestamp, &input, &timestamp, &output, &timestamp, context);
            if (!result && output.mNumberBuffers == 1 && output.mBuffers[0].mData == scratch && output.mBuffers[0].mDataByteSize == sizeof(scratch))
                for (unsigned sample = 0; sample < sizeof(mixed) / sizeof(*mixed); ++sample)
                    if (isfinite(scratch[sample])) mixed[sample] += scratch[sample];
            pthread_mutex_lock(&outputLock);
            proc->executing = false;
            if (proc->releaseAfterCallback) *proc = (OutputProc){0};
            pthread_cond_broadcast(&outputChanged);
            pthread_mutex_unlock(&outputLock);
        }
        for (unsigned sample = 0; sample < sizeof(mixed) / sizeof(*mixed); ++sample)
            mixed[sample] = fmaxf(-1, fminf(1, mixed[sample]));
        // Only this worker writes/closes stream; a concurrent Stop waits for it to finish the current block.
        LSBGuestAudioOutputWrite(stream, mixed, LSB_AUDIO_OUTPUT_FRAMES, timestamp.mHostTime);
        deadline += interval;
        mach_wait_until(deadline);
    }
    return NULL;
}

// Size queries do not create or retain CF objects. Unknown properties never become invented successful values.
typedef struct {
    UInt32 size;
    union { UInt32 integers[2]; Float64 number; AudioValueRange range; AudioStreamBasicDescription format;
        AudioStreamRangedDescription rangedFormat; AudioBufferList buffers; AudioChannelLayout layout; CFTypeRef object; } value;
    bool object, owned;
} Property;

static AudioObjectID deviceForUID(CFStringRef uid) {
    if (!uid || CFGetTypeID(uid) != CFStringGetTypeID()) return kAudioObjectUnknown;
    if (CFEqual(uid, CFSTR("LiveSimBooterOutput"))) return LSB_AUDIO_OUTPUT_DEVICE;
    AudioObjectID identifier = kAudioObjectUnknown;
    pthread_mutex_lock(&outputLock);
    for (unsigned i = 0; i < LSB_AUDIO_OUTPUT_MAX_AGGREGATES; ++i) {
        OutputAggregate *aggregate = &outputAggregates[i];
        if (aggregate->composition && CFEqual(uid, CFDictionaryGetValue(aggregate->composition, CFSTR("uid")))) {
            identifier = aggregate->identifier;
            break;
        }
    }
    pthread_mutex_unlock(&outputLock);
    return identifier;
}

static bool deviceRunning(AudioObjectID object) {
    // All aggregate output reaches the physical device, but one aggregate does not inherit another's IOProc state.
    if (!aggregateObject(object)) return atomic_load(&runningCount) != 0;
    AudioObjectID identifier = object & ~1u;
    bool running = false;
    pthread_mutex_lock(&outputLock);
    for (unsigned i = 0; i < LSB_AUDIO_OUTPUT_MAX_PROCS; ++i)
        if (outputProcs[i].device == identifier && outputProcs[i].running) { running = true; break; }
    pthread_mutex_unlock(&outputLock);
    return running;
}

static OSStatus property(AudioObjectID object, const AudioObjectPropertyAddress *address, UInt32 qualifierSize,
                         const void *qualifier, bool copyObjects, Property *result) {
    if (!address) return kAudioHardwareIllegalOperationError;
    memset(result, 0, sizeof(*result));
    UInt32 selector = address->mSelector;
    bool input = address->mScope == kAudioObjectPropertyScopeInput;
    result->size = sizeof(UInt32);
    if (object == 1) {
        switch (selector) {
            case 'dev#': case 'dOut': case 'sOut': result->value.integers[0] = LSB_AUDIO_OUTPUT_DEVICE; return 0;
            case 'dIn ': return 0; // Output-only configuration: there is no simulator input device to aggregate.
            case 'duid': result->size = sizeof(AudioValueTranslation); return 0;
            case 'uidd': {
                if (qualifierSize != sizeof(CFStringRef) || !qualifier) return kAudioHardwareBadPropertySizeError;
                result->value.integers[0] = deviceForUID(*(CFStringRef const *)qualifier);
                return 0;
            }
            default: return kAudioHardwareUnknownPropertyError;
        }
    }
    if (!ours(object) || !objectExists(object)) return kAudioHardwareBadObjectError;
    if (address->mElement > LSB_AUDIO_OUTPUT_CHANNELS || (address->mScope != 'glob' && address->mScope != 'outp' && !input))
        return kAudioHardwareUnknownPropertyError;
    if (aggregateObject(object) && device(object) && (selector == 'acom' || selector == 'uid ' || selector == 'lnam')) {
        pthread_mutex_lock(&outputLock);
        OutputAggregate *aggregate = findAggregate(object);
        if (aggregate) {
            result->size = sizeof(CFTypeRef); result->object = true;
            CFTypeRef value = selector == 'acom' ? aggregate->composition :
                CFDictionaryGetValue(aggregate->composition, selector == 'uid ' ? CFSTR("uid") : CFSTR("name"));
            if (copyObjects) { result->value.object = CFRetain(value); result->owned = true; }
        }
        pthread_mutex_unlock(&outputLock);
        return aggregate ? 0 : kAudioHardwareBadObjectError;
    }
    switch (selector) {
        case 'clas': result->value.integers[0] = device(object) ? (aggregateObject(object) ? 'aagg' : 'adev') : 'astr'; break;
        case 'bcls': result->value.integers[0] = device(object) && aggregateObject(object) ? 'adev' : 'aobj'; break;
        case 'stdv': result->value.integers[0] = device(object) ? 1 : aggregateObject(object) ? object - 1 : LSB_AUDIO_OUTPUT_DEVICE; break;
        case 'lnam': case 'lmod': case 'lmak': case 'uid ': case 'muid':
            result->size = sizeof(CFStringRef); result->object = true;
            result->value.object = selector == 'uid ' ? CFSTR("LiveSimBooterOutput") : CFSTR("LiveSimBooter Audio Output"); break;
        case 'ownd': case 'stm#':
            result->size = device(object) && !input ? sizeof(UInt32) : 0;
            result->value.integers[0] = aggregateObject(object) ? object + 1 : LSB_AUDIO_OUTPUT_STREAM; break;
        case 'slay':
            result->size = input ? offsetof(AudioBufferList, mBuffers) : sizeof(AudioBufferList);
            result->value.buffers = (AudioBufferList){.mNumberBuffers = input ? 0 : 1, .mBuffers = {{2, 0, NULL}}}; break;
        case 'srnd':
            result->size = offsetof(AudioChannelLayout, mChannelDescriptions);
            result->value.layout = (AudioChannelLayout){.mChannelLayoutTag = input ? kAudioChannelLayoutTag_UseChannelDescriptions :
                kAudioChannelLayoutTag_Stereo, .mNumberChannelDescriptions = 0}; break;
        case 'dflt': case 'sflt': result->value.integers[0] = input ? 0 : 1; break;
        case 'livn': case 'jack': case 'actv': result->value.integers[0] = 1; break;
        case 'goin': case 'gone': result->value.integers[0] = deviceRunning(object); break;
        case 'fsiz': result->value.integers[0] = LSB_AUDIO_OUTPUT_FRAMES; break;
        case 'fsz#': result->size = sizeof(AudioValueRange); result->value.range = (AudioValueRange){512, 512}; break;
        case 'nsrt': case 'asrt': result->size = sizeof(Float64); result->value.number = LSB_AUDIO_OUTPUT_RATE; break;
        case 'nsr#': result->size = sizeof(AudioValueRange); result->value.range = (AudioValueRange){48000, 48000}; break;
        case 'ltnc': case 'saft': case 'clkd': case 'vfsz': case 'hide': case 'mute': case 'stpd': case 'over': break;
        case 'tran': result->value.integers[0] = 'virt'; break;
        case 'oink': result->value.integers[0] = (UInt32)-1; break;
        case 'dch2': result->size = sizeof(result->value.integers); result->value.integers[0] = 1; result->value.integers[1] = 2; break;
        case 'sdir': result->value.integers[0] = 0; break;
        case 'schn': result->value.integers[0] = 1; break;
        case 'sfmt': case 'pft ': result->size = sizeof(AudioStreamBasicDescription); result->value.format = outputFormat(); break;
        case 'sfma': case 'pfta':
            result->size = sizeof(AudioStreamRangedDescription);
            result->value.rangedFormat = (AudioStreamRangedDescription){outputFormat(), {48000, 48000}}; break;
        default:
            traceProperty("unsupported", object, address, kAudioHardwareUnknownPropertyError);
            return kAudioHardwareUnknownPropertyError;
    }
    return 0;
}

static bool intercept(AudioObjectID object, const AudioObjectPropertyAddress *address) {
    if (!enabled()) return false;
    if (ours(object)) return true;
    if (object != 1 || !address || address->mScope != 'glob' || address->mElement != 0) return false;
    bool handled = address->mSelector == 'dev#' || address->mSelector == 'dOut' || address->mSelector == 'sOut' ||
        address->mSelector == 'dIn ' || address->mSelector == 'uidd' || address->mSelector == 'duid';
    if (!handled) traceProperty("forward-system", object, address, 0);
    return handled;
}

Boolean LSBAudioObjectExists(AudioObjectID object) {
    return enabled() && (ours(object) || object == 1) ? objectExists(object) : AudioObjectExists(object);
}

Boolean LSBAudioObjectHasProperty(AudioObjectID object, const AudioObjectPropertyAddress *address) {
    if (!intercept(object, address)) return AudioObjectHasProperty(object, address);
    Property value;
    // UID translation's qualifier is only required when reading its value.
    if (object == 1 && address->mSelector == 'uidd') return true;
    return property(object, address, 0, NULL, false, &value) == 0;
}

OSStatus LSBAudioObjectIsPropertySettable(AudioObjectID object, const AudioObjectPropertyAddress *address, Boolean *settable) {
    if (!intercept(object, address)) return AudioObjectIsPropertySettable(object, address, settable);
    if (!settable) return kAudioHardwareIllegalOperationError;
    *settable = false;
    return LSBAudioObjectHasProperty(object, address) ? 0 : kAudioHardwareUnknownPropertyError;
}

OSStatus LSBAudioObjectGetPropertyDataSize(AudioObjectID object, const AudioObjectPropertyAddress *address,
                                          UInt32 qualifierSize, const void *qualifier, UInt32 *size) {
    if (!intercept(object, address)) return AudioObjectGetPropertyDataSize(object, address, qualifierSize, qualifier, size);
    if (!size) return kAudioHardwareIllegalOperationError;
    Property value;
    OSStatus status = property(object, address, qualifierSize, qualifier, false, &value);
    if (!status) *size = value.size;
    return status;
}

OSStatus LSBAudioObjectGetPropertyData(AudioObjectID object, const AudioObjectPropertyAddress *address,
                                      UInt32 qualifierSize, const void *qualifier, UInt32 *size, void *data) {
    if (!intercept(object, address)) return AudioObjectGetPropertyData(object, address, qualifierSize, qualifier, size, data);
    if (!size) return kAudioHardwareIllegalOperationError;
    if (object == 1 && address->mSelector == 'duid') {
        // Deprecated DeviceForUID uses an in/out AudioValueTranslation, not the modern 'uidd' qualifier ABI.
        // 23A343 DeviceState::loadState resolves each aggregate child through this form.
        if (*size < sizeof(AudioValueTranslation) || !data || qualifierSize) {
            *size = sizeof(AudioValueTranslation); return kAudioHardwareBadPropertySizeError;
        }
        AudioValueTranslation translation;
        memcpy(&translation, data, sizeof(translation));
        if (!translation.mInputData || translation.mInputDataSize != sizeof(CFStringRef) || !translation.mOutputData ||
            translation.mOutputDataSize != sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
        CFStringRef uid;
        memcpy(&uid, translation.mInputData, sizeof(uid));
        AudioObjectID identifier = deviceForUID(uid);
        memcpy(translation.mOutputData, &identifier, sizeof(identifier));
        *size = sizeof(translation);
        return 0;
    }
    Property value;
    OSStatus status = property(object, address, qualifierSize, qualifier, true, &value);
    if (status) return status;
    if (*size < value.size || (!data && value.size)) {
        if (value.owned) CFRelease(value.value.object);
        *size = value.size; return kAudioHardwareBadPropertySizeError;
    }
    if (value.object && !value.owned) CFRetain(value.value.object);
    if (value.size) memcpy(data, &value.value, value.size);
    *size = value.size;
    return 0;
}

OSStatus LSBAudioObjectSetPropertyData(AudioObjectID object, const AudioObjectPropertyAddress *address,
                                      UInt32 qualifierSize, const void *qualifier, UInt32 size, const void *data) {
    if (!intercept(object, address)) return AudioObjectSetPropertyData(object, address, qualifierSize, qualifier, size, data);
    Property value;
    OSStatus status = property(object, address, qualifierSize, qualifier, false, &value);
    if (status) return status;
    // Stock HAL clients repeat their chosen format/buffer size during initialization. Accept only unchanged fixed configuration.
    if (device(object) && (address->mSelector == 'fsiz' || address->mSelector == 'nsrt') && size == value.size && data &&
        !memcmp(data, &value.value, size)) return 0;
    if (!device(object) && (address->mSelector == 'sfmt' || address->mSelector == 'pft ') &&
        size == value.size && data && !memcmp(data, &value.value, size)) return 0;
    return kAudioHardwareUnsupportedOperationError;
}

// This device has immutable topology/format. Listener registration is supported only for properties that never change.
static OSStatus immutableListener(AudioObjectID object, const AudioObjectPropertyAddress *address) {
    if (!LSBAudioObjectHasProperty(object, address)) return kAudioHardwareUnknownPropertyError;
    if (address->mSelector == 'goin' || address->mSelector == 'gone') {
        traceProperty("unsupported-listener", object, address, kAudioHardwareUnsupportedOperationError);
        return kAudioHardwareUnsupportedOperationError;
    }
    return 0;
}
OSStatus LSBAudioObjectAddPropertyListener(AudioObjectID object, const AudioObjectPropertyAddress *address,
                                          LSBAudioPropertyListener listener, void *context) {
    if (!intercept(object, address)) return AudioObjectAddPropertyListener(object, address, listener, context);
    return listener ? immutableListener(object, address) : kAudioHardwareIllegalOperationError;
}
// CoreAudio 23A343 +0x34818 has the same four-argument ABI as the public listener entry, with an internal-listener flag.
OSStatus LSBAudioObjectAddPropertyListenerInternal(AudioObjectID object, const AudioObjectPropertyAddress *address,
                                                  LSBAudioPropertyListener listener, void *context) {
    if (!intercept(object, address)) return AudioObjectAddPropertyListenerInternal(object, address, listener, context);
    return listener ? immutableListener(object, address) : kAudioHardwareIllegalOperationError;
}
OSStatus LSBAudioObjectRemovePropertyListener(AudioObjectID object, const AudioObjectPropertyAddress *address,
                                             LSBAudioPropertyListener listener, void *context) {
    if (!intercept(object, address)) return AudioObjectRemovePropertyListener(object, address, listener, context);
    return listener ? immutableListener(object, address) : kAudioHardwareIllegalOperationError;
}
OSStatus LSBAudioObjectAddPropertyListenerBlock(AudioObjectID object, const AudioObjectPropertyAddress *address,
                                               dispatch_queue_t queue, LSBAudioPropertyListenerBlock listener) {
    if (!intercept(object, address)) return AudioObjectAddPropertyListenerBlock(object, address, queue, listener);
    return listener ? immutableListener(object, address) : kAudioHardwareIllegalOperationError;
}
OSStatus LSBAudioObjectRemovePropertyListenerBlock(AudioObjectID object, const AudioObjectPropertyAddress *address,
                                                  dispatch_queue_t queue, LSBAudioPropertyListenerBlock listener) {
    if (!intercept(object, address)) return AudioObjectRemovePropertyListenerBlock(object, address, queue, listener);
    return listener ? immutableListener(object, address) : kAudioHardwareIllegalOperationError;
}

static bool numberEquals(CFTypeRef value, int expected) {
    int number = 0;
    return value && CFGetTypeID(value) == CFNumberGetTypeID() && CFNumberGetValue(value, kCFNumberIntType, &number) && number == expected;
}
static bool supportedComposition(CFDictionaryRef description) {
    CFStringRef uid = CFDictionaryGetValue(description, CFSTR("uid")), name = CFDictionaryGetValue(description, CFSTR("name"));
    CFArrayRef children = CFDictionaryGetValue(description, CFSTR("subdevices"));
    if (!uid || CFGetTypeID(uid) != CFStringGetTypeID() || !CFEqual(uid, name) || !children ||
        CFGetTypeID(children) != CFArrayGetTypeID() || CFArrayGetCount(children) != 1) return false;
    CFDictionaryRef child = CFArrayGetValueAtIndex(children, 0);
    if (CFGetTypeID(child) != CFDictionaryGetTypeID()) return false;
    CFTypeRef childUID = CFDictionaryGetValue(child, CFSTR("uid"));
    if (!childUID || !CFEqual(childUID, CFSTR("LiveSimBooterOutput"))) return false;
    CFTypeRef input = CFDictionaryGetValue(child, CFSTR("channels-in")), output = CFDictionaryGetValue(child, CFSTR("channels-out"));
    CFTypeRef stacked = CFDictionaryGetValue(description, CFSTR("stacked"));
    return (!input || numberEquals(input, 0)) && (!output || numberEquals(output, 2)) &&
        (!stacked || stacked == kCFBooleanFalse || numberEquals(stacked, 0));
}

OSStatus LSBAudioHardwareCreateAggregateDevice(CFDictionaryRef description, AudioObjectID *identifier) {
    if (enabled() && description && CFGetTypeID(description) == CFDictionaryGetTypeID()) {
        CFStringRef name = CFDictionaryGetValue(description, CFSTR("name"));
        if (name && CFGetTypeID(name) == CFStringGetTypeID() && CFStringHasPrefix(name, CFSTR("iOSSimulatorAudioDevice"))) {
            if (!identifier) return kAudioHardwareIllegalOperationError;
            *identifier = 0;
            if (!supportedComposition(description)) return kAudioHardwareUnsupportedOperationError;
            // 23A343 AggregateDevice::buildOrRebuild requires mAggregate and verifies its child channel counts (+0x12aac..0x130c8).
            // The physical device must not publish this composition: resolving its own UID as a child would recurse indefinitely.
            CFDictionaryRef copy = CFPropertyListCreateDeepCopy(NULL, description, kCFPropertyListImmutable);
            if (!copy) return kAudioHardwareUnspecifiedError;
            pthread_mutex_lock(&outputLock);
            bool duplicate = false;
            CFStringRef uid = CFDictionaryGetValue(copy, CFSTR("uid"));
            for (unsigned i = 0; i < LSB_AUDIO_OUTPUT_MAX_AGGREGATES; ++i)
                if (outputAggregates[i].composition && CFEqual(uid, CFDictionaryGetValue(outputAggregates[i].composition, CFSTR("uid")))) {
                    duplicate = true; break;
                }
            for (unsigned i = 0; !duplicate && i < LSB_AUDIO_OUTPUT_MAX_AGGREGATES && nextAggregate < LSB_AUDIO_OUTPUT_AGGREGATE_LIMIT; ++i) {
                if (outputAggregates[i].identifier) continue;
                outputAggregates[i] = (OutputAggregate){nextAggregate, copy};
                *identifier = nextAggregate;
                nextAggregate += 2;
                break;
            }
            pthread_mutex_unlock(&outputLock);
            if (!*identifier) CFRelease(copy);
            OSStatus status = *identifier ? 0 : kAudioHardwareIllegalOperationError;
            traceProperty("create-aggregate", *identifier, &(AudioObjectPropertyAddress){'acom', 'glob', 0}, status);
            return status;
        }
    }
    return AudioHardwareCreateAggregateDevice(description, identifier);
}
OSStatus LSBAudioHardwareDestroyAggregateDevice(AudioObjectID object) {
    if (!enabled() || !aggregateObject(object) || !device(object)) return AudioHardwareDestroyAggregateDevice(object);
    pthread_mutex_lock(&outputLock);
    OutputAggregate *aggregate = findAggregate(object);
    for (unsigned i = 0; aggregate && i < LSB_AUDIO_OUTPUT_MAX_PROCS; ++i) {
        if (outputProcs[i].identifier && outputProcs[i].device == object) {
            pthread_mutex_unlock(&outputLock);
            return kAudioHardwareIllegalOperationError;
        }
    }
    CFDictionaryRef composition = aggregate ? aggregate->composition : NULL;
    if (aggregate) *aggregate = (OutputAggregate){0};
    pthread_mutex_unlock(&outputLock);
    if (composition) CFRelease(composition);
    return composition ? 0 : kAudioHardwareBadDeviceError;
}

OSStatus LSBAudioDeviceCreateIOProcID(AudioObjectID object, LSBAudioDeviceIOProc callback, void *context, LSBAudioDeviceIOProcID *identifier) {
    if (!enabled() || !device(object)) return AudioDeviceCreateIOProcID(object, callback, context, identifier);
    if (!callback || !identifier) return kAudioHardwareIllegalOperationError;
    *identifier = NULL;
    pthread_mutex_lock(&outputLock);
    if (aggregateObject(object) && !findAggregate(object)) { pthread_mutex_unlock(&outputLock); return kAudioHardwareBadDeviceError; }
    OSStatus status = kAudioHardwareIllegalOperationError;
    for (unsigned i = 0; i < LSB_AUDIO_OUTPUT_MAX_PROCS; ++i) {
        if (outputProcs[i].identifier) continue;
        if (!nextIdentifier) break;
        outputProcs[i] = (OutputProc){.identifier = nextIdentifier++, .device = object, .callback = callback, .context = context};
        *identifier = (LSBAudioDeviceIOProcID)outputProcs[i].identifier;
        status = 0;
        break;
    }
    pthread_mutex_unlock(&outputLock);
    return status;
}

static OSStatus stopProc(AudioObjectID object, LSBAudioDeviceIOProcID identifier, bool destroy) {
    pthread_mutex_lock(&outputLock);
    OutputProc *proc = findProc(identifier);
    if (!proc || proc->device != object) { pthread_mutex_unlock(&outputLock); return kAudioHardwareIllegalOperationError; }
    if (proc->running) { proc->running = false; --runningCount; }
    bool self = hasOutputThread && pthread_equal(pthread_self(), outputThread);
    if (destroy) proc->retiring = true;
    if (destroy && self) proc->releaseAfterCallback = true;
    if (!self) while (proc->executing || (!runningCount && outputStream)) pthread_cond_wait(&outputChanged, &outputLock);
    if (destroy && !proc->executing) *proc = (OutputProc){0};
    pthread_mutex_unlock(&outputLock);
    return 0;
}
OSStatus LSBAudioDeviceDestroyIOProcID(AudioObjectID object, LSBAudioDeviceIOProcID identifier) {
    return enabled() && device(object) ? stopProc(object, identifier, true) : AudioDeviceDestroyIOProcID(object, identifier);
}
OSStatus LSBAudioDeviceStop(AudioObjectID object, LSBAudioDeviceIOProcID identifier) {
    return enabled() && device(object) ? stopProc(object, identifier, false) : AudioDeviceStop(object, identifier);
}

OSStatus LSBAudioDeviceStart(AudioObjectID object, LSBAudioDeviceIOProcID identifier) {
    if (!enabled() || !device(object)) return AudioDeviceStart(object, identifier);
    pthread_mutex_lock(&outputLock);
    OutputProc *proc = findProc(identifier);
    if (!proc || proc->device != object) { pthread_mutex_unlock(&outputLock); return kAudioHardwareIllegalOperationError; }
    if (proc->running) { pthread_mutex_unlock(&outputLock); return 0; }
    int error = outputStream ? 0 : LSBGuestAudioOutputOpen(&outputStream);
    if (!error && !outputStream) error = EIO;
    if (!error && !hasOutputThread) {
        pthread_attr_t attributes;
        pthread_attr_init(&attributes);
        pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
        error = pthread_create(&outputThread, &attributes, renderOutput, NULL);
        pthread_attr_destroy(&attributes);
        hasOutputThread = error == 0;
    }
    if (error) {
        if (outputStream) { LSBGuestAudioOutputClose(outputStream); outputStream = NULL; }
        pthread_mutex_unlock(&outputLock);
        return kAudioHardwareNotReadyError;
    }
    proc->running = true;
    ++runningCount;
    pthread_cond_broadcast(&outputChanged);
    pthread_mutex_unlock(&outputLock);
    traceProperty("start-device", object, &(AudioObjectPropertyAddress){'goin', 'glob', 0}, 0);
    return 0;
}

OSStatus LSBAudioDeviceStartAtTime(AudioObjectID object, LSBAudioDeviceIOProcID identifier, AudioTimeStamp *time, UInt32 flags) {
    if (!enabled() || !device(object)) return AudioDeviceStartAtTime(object, identifier, time, flags);
    // AQMEIO_HAL 23A343 +0x121754..0x121814 uses its ordinary Start fallback when 'dspd' is absent, as on this device.
    // Explicit scheduled starts remain unsupported rather than silently starting at the wrong time.
    if (flags || (time && (time->mFlags & (kAudioTimeStampSampleTimeValid | kAudioTimeStampHostTimeValid))))
        return kAudioHardwareUnsupportedOperationError;
    return LSBAudioDeviceStart(object, identifier);
}
OSStatus LSBAudioDeviceGetCurrentTime(AudioObjectID object, AudioTimeStamp *time) {
    if (!enabled() || !device(object)) return AudioDeviceGetCurrentTime(object, time);
    if (!objectExists(object)) return kAudioHardwareBadDeviceError;
    if (!time) return kAudioHardwareIllegalOperationError;
    fillTime(time, mach_absolute_time());
    return 0;
}
OSStatus LSBAudioDeviceTranslateTime(AudioObjectID object, const AudioTimeStamp *input, AudioTimeStamp *output) {
    if (!enabled() || !device(object)) return AudioDeviceTranslateTime(object, input, output);
    if (!objectExists(object)) return kAudioHardwareBadDeviceError;
    if (!input || !output) return kAudioHardwareIllegalOperationError;
    if (!(input->mFlags & kAudioTimeStampHostTimeValid)) return kAudioHardwareUnsupportedOperationError;
    fillTime(output, input->mHostTime);
    return 0;
}
OSStatus LSBAudioDeviceGetNearestStartTime(AudioObjectID object, AudioTimeStamp *time, UInt32 flags) {
    if (!enabled() || !device(object)) return AudioDeviceGetNearestStartTime(object, time, flags);
    if (!objectExists(object)) return kAudioHardwareBadDeviceError;
    if (!time) return kAudioHardwareIllegalOperationError;
    if (flags) return kAudioHardwareUnsupportedOperationError;
    fillTime(time, mach_absolute_time() + ticksPerBlock());
    return 0;
}

#ifndef LSB_AUDIO_OUTPUT_TESTING
#define INTERPOSE(name) {(const void *)&LSB##name, (const void *)&name}
__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *replacement, *original; } interpositions[] = {
    INTERPOSE(AudioObjectExists), INTERPOSE(AudioObjectHasProperty), INTERPOSE(AudioObjectIsPropertySettable), INTERPOSE(AudioObjectGetPropertyDataSize),
    INTERPOSE(AudioObjectGetPropertyData), INTERPOSE(AudioObjectSetPropertyData), INTERPOSE(AudioObjectAddPropertyListener),
    INTERPOSE(AudioObjectAddPropertyListenerInternal), INTERPOSE(AudioObjectRemovePropertyListener),
    INTERPOSE(AudioObjectAddPropertyListenerBlock), INTERPOSE(AudioObjectRemovePropertyListenerBlock),
    INTERPOSE(AudioHardwareCreateAggregateDevice), INTERPOSE(AudioHardwareDestroyAggregateDevice), INTERPOSE(AudioDeviceCreateIOProcID),
    INTERPOSE(AudioDeviceDestroyIOProcID), INTERPOSE(AudioDeviceStart), INTERPOSE(AudioDeviceStartAtTime), INTERPOSE(AudioDeviceStop),
    INTERPOSE(AudioDeviceGetCurrentTime), INTERPOSE(AudioDeviceTranslateTime), INTERPOSE(AudioDeviceGetNearestStartTime)
};
#undef INTERPOSE
#endif
