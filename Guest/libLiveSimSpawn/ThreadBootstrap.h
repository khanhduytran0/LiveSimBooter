#ifndef LSB_THREAD_BOOTSTRAP_H
#define LSB_THREAD_BOOTSTRAP_H

#include <stddef.h>
#include <stdint.h>

// Private syscall ABIs used by the selected ARM64 simulator libpthread. Registration's native result is preserved.
int LSBThreadRegister(uintptr_t start, uintptr_t workqueueStart, int pthreadSize, void *data, size_t size, uint64_t dispatchOffset);
void *LSBThreadCreate(void *(*function)(void *), void *argument, void *stack, void *thread, unsigned flags);
uintptr_t LSBThreadSignPC(uintptr_t start, uint32_t flags);

#endif
