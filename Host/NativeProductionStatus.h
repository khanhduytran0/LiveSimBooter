#pragma once
#include "../ProcessSupport/NativeRuntimePolicy.h"

// Native host only. Cached for this host lifetime; missing/unavailable/non-Boolean MobileGestalt answers remain Unknown.
uint32_t LSBHostNativeProductionStatus(void);
