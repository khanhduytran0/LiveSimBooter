#ifndef LSB_AUDIO_OUTPUT_COMPATIBILITY_H
#define LSB_AUDIO_OUTPUT_COMPATIBILITY_H

#include <CoreAudio/AudioHardwareBase.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>

#define LSB_AUDIO_OUTPUT_ENABLE_ENV "LSB_AUDIO_OUTPUT"
enum { LSB_AUDIO_OUTPUT_DEVICE = 0x4c534201, LSB_AUDIO_OUTPUT_STREAM = 0x4c534202, LSB_AUDIO_OUTPUT_FRAMES = 512,
       LSB_AUDIO_OUTPUT_RATE = 48000, LSB_AUDIO_OUTPUT_CHANNELS = 2, LSB_AUDIO_OUTPUT_MAX_PROCS = 8,
       LSB_AUDIO_OUTPUT_AGGREGATE_FIRST = 0x4c534300, LSB_AUDIO_OUTPUT_AGGREGATE_LIMIT = 0x4c535300, LSB_AUDIO_OUTPUT_MAX_AGGREGATES = 8 };

// AudioHardware.h is omitted from the Simulator SDK. These are the public HAL C ABI used by 23A343 AudioToolbox.
typedef OSStatus (*LSBAudioDeviceIOProc)(AudioObjectID, const AudioTimeStamp *, const AudioBufferList *, const AudioTimeStamp *,
                                        AudioBufferList *, const AudioTimeStamp *, void *);
typedef LSBAudioDeviceIOProc LSBAudioDeviceIOProcID;
typedef OSStatus (*LSBAudioPropertyListener)(AudioObjectID, UInt32, const AudioObjectPropertyAddress *, void *);
typedef void (^LSBAudioPropertyListenerBlock)(UInt32, const AudioObjectPropertyAddress *);

#define LSB_AUDIO_DECLARATIONS(prefix) \
Boolean prefix##AudioObjectExists(AudioObjectID); \
Boolean prefix##AudioObjectHasProperty(AudioObjectID, const AudioObjectPropertyAddress *); \
OSStatus prefix##AudioObjectIsPropertySettable(AudioObjectID, const AudioObjectPropertyAddress *, Boolean *); \
OSStatus prefix##AudioObjectGetPropertyDataSize(AudioObjectID, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32 *); \
OSStatus prefix##AudioObjectGetPropertyData(AudioObjectID, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32 *, void *); \
OSStatus prefix##AudioObjectSetPropertyData(AudioObjectID, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32, const void *); \
OSStatus prefix##AudioObjectAddPropertyListener(AudioObjectID, const AudioObjectPropertyAddress *, LSBAudioPropertyListener, void *); \
OSStatus prefix##AudioObjectAddPropertyListenerInternal(AudioObjectID, const AudioObjectPropertyAddress *, LSBAudioPropertyListener, void *); \
OSStatus prefix##AudioObjectRemovePropertyListener(AudioObjectID, const AudioObjectPropertyAddress *, LSBAudioPropertyListener, void *); \
OSStatus prefix##AudioObjectAddPropertyListenerBlock(AudioObjectID, const AudioObjectPropertyAddress *, dispatch_queue_t, LSBAudioPropertyListenerBlock); \
OSStatus prefix##AudioObjectRemovePropertyListenerBlock(AudioObjectID, const AudioObjectPropertyAddress *, dispatch_queue_t, LSBAudioPropertyListenerBlock); \
OSStatus prefix##AudioHardwareCreateAggregateDevice(CFDictionaryRef, AudioObjectID *); \
OSStatus prefix##AudioHardwareDestroyAggregateDevice(AudioObjectID); \
OSStatus prefix##AudioDeviceCreateIOProcID(AudioObjectID, LSBAudioDeviceIOProc, void *, LSBAudioDeviceIOProcID *); \
OSStatus prefix##AudioDeviceDestroyIOProcID(AudioObjectID, LSBAudioDeviceIOProcID); \
OSStatus prefix##AudioDeviceStart(AudioObjectID, LSBAudioDeviceIOProcID); \
OSStatus prefix##AudioDeviceStartAtTime(AudioObjectID, LSBAudioDeviceIOProcID, AudioTimeStamp *, UInt32); \
OSStatus prefix##AudioDeviceStop(AudioObjectID, LSBAudioDeviceIOProcID); \
OSStatus prefix##AudioDeviceGetCurrentTime(AudioObjectID, AudioTimeStamp *); \
OSStatus prefix##AudioDeviceTranslateTime(AudioObjectID, const AudioTimeStamp *, AudioTimeStamp *); \
OSStatus prefix##AudioDeviceGetNearestStartTime(AudioObjectID, AudioTimeStamp *, UInt32)

LSB_AUDIO_DECLARATIONS();
LSB_AUDIO_DECLARATIONS(LSB);
#undef LSB_AUDIO_DECLARATIONS

#endif
