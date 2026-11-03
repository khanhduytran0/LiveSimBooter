#pragma once

#include "ForkMapPlan.h"
#include <mach/mach.h>

#ifdef __cplusplus
extern "C" {
#endif

// Both task rights and inventories belong to the trusted coordinator. Parent ranges are a quiescent snapshot;
// the unpublished child is parked. Keep these conditions true until the caller commits or discards the child.
// No operation overwrites or deallocates an existing mapping. On any error discard the child: a signed mapping
// installed before a later failure may be permanent, so deallocating the individual steps cannot roll it back.
int LSBCloneForkRanges(vm_map_t sourceTask, vm_map_t childTask, const LSBForkMapRange *source, size_t sourceCount,
                       const LSBForkMapRange *child, size_t childCount);

#ifdef __cplusplus
}
#endif
