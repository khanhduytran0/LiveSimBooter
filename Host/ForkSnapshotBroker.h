#pragma once

#include "../ProcessSupport/SpawnTransport.h"

// Called only after authenticating the offer and the accepting child. The child must exit on failure,
// since a partial exact-address snapshot cannot always be rolled back.
int LSBCopyOfferedForkRanges(mach_port_t parent, mach_port_t child, const LSBForkWireRange *offered, size_t count);
