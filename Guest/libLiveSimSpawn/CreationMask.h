#ifndef LSB_CREATION_MASK_H
#define LSB_CREATION_MASK_H

#include <stdint.h>
#include <sys/types.h>

// Returns an errno value and preserves errno. Darwin 25 uses an early snapshot maintained by the umask interposer.
// Direct umask syscalls or calls bypassing interposition cannot update that shadow and are unsupported on older kernels.
int LSBReadCreationMask(uint32_t *mask);
mode_t LSBUmask(mode_t mask);

#endif
