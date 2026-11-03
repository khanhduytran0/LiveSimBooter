#ifndef DyldSimLoader_h
#define DyldSimLoader_h

#include <mach-o/loader.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const struct mach_header *header;
    size_t mappedSize;
    uintptr_t slide;
    uintptr_t entry;
} LSBMappedImage;

// Trusted native bootstrap metadata, configured before mapping either image. Zero selects the ordinary low-cache path.
// The selected nonzero address must agree with the immutable cache data backing; guest environment variables cannot select it.
int LSBConfigureRuntimeCacheBase(uint64_t actualBase);
int LSBConfigureRuntimeNativeSupport(unsigned enabled);

// Read the planned guest environment during native preparation, before changing TLS or entering the simulator.
static inline bool LSBTraceHandoffRequested(const char *const environment[]) {
    const char prefix[] = "LSB_TRACE_HANDOFF=";
    for (const char *const *entry = environment; entry && *entry; ++entry) {
        size_t index = 0;
        while (prefix[index] && (*entry)[index] == prefix[index]) ++index;
        if (!prefix[index]) return (*entry)[index] == '1' && (*entry)[index + 1] == '\0';
    }
    return false;
}

bool LSBMapDyldSim(const char *path, LSBMappedImage *image, char *error, size_t errorSize);
bool LSBMapSimExecutable(const char *path, LSBMappedImage *image, char *error, size_t errorSize);
// The native-support profile maps guest executables in the selected high address window. dyld_sim and legacy profiles use the low image range.
bool LSBMapDyldSimForRuntime(const char *path, const char *rootPath, LSBMappedImage *image, char *error, size_t errorSize);
bool LSBMapSimExecutableForRuntime(const char *path, const char *rootPath, LSBMappedImage *image, char *error, size_t errorSize);
// The fork observer uses a separate executable slot, leaving the ordinary main image address available for its parent.
bool LSBMapForkBootstrapForRuntime(const char *path, const char *rootPath, LSBMappedImage *image, char *error, size_t errorSize);
// Only unload before entering dyld_sim; it retains pointers into both mapped images.
void LSBUnloadImage(LSBMappedImage *image);

// The image, executable, helpers and argument storage must survive the simulator runtime.
// Retains or detaches the host cache according to native policy, calls dyld_sim's prepare entry, then invokes its LC_MAIN wrapper.
// Returns only on a pre-entry failure. If the wrapper returns, exits via a raw syscall without host cleanup.
int LSBEnterDyldSim(const LSBMappedImage *image, const struct mach_header *executable, const char *executablePath,
                   const char *dyldPath, const char *rootPath,
                   int argc, const char *const argv[], const char *const envp[], const char *const apple[]);

#ifdef __cplusplus
}
#endif

#endif
