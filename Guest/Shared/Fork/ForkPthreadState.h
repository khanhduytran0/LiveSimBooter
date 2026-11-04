// Native pthread atfork registrations, verified on 23B50 and 24A435. Native-cache callbacks must match the observer's existing entries.
#pragma once
#include <stdint.h>
enum { ForkPthreadMaxRegistrations = 32 };
typedef struct { uint64_t pc, image; uint8_t uuid[16]; } ForkPthreadTarget; // Bit 0 of image marks a cached owner.
typedef struct { ForkPthreadTarget functions[3]; } ForkPthreadRegistration;
typedef struct { uint32_t count, reserved; ForkPthreadRegistration entries[ForkPthreadMaxRegistrations]; } ForkPthreadState;
_Static_assert(sizeof(ForkPthreadState) == 3080, "fork pthread registration wire layout");
