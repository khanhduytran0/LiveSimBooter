#pragma once
#include <mach/mach.h>

#include <stdint.h>

// Fixed CoreAnimation MIG operations from iOS Simulator 26.0 (23A343), QuartzCore UUID
// 0FE71F43-30A2-379C-A15E-2637DA243279. Unlike the private-API wrappers, these preserve RPC failures.
int LSBRenderServerGetFlags(mach_port_t server, uint32_t *flags);
int LSBRenderServerSetFlags(mach_port_t server, uint32_t mask, uint32_t flags);
int LSBRenderServerGetHUDValue(mach_port_t server, int32_t *value);
int LSBRenderServerSetHUDValue(mach_port_t server, int32_t value);
