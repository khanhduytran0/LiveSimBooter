#include "NativeSignerLoader.h"
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LSBLoadNativeSigner(const audit_token_t *host, mach_port_t *endpoint, LSBNativeSignerRunFunction *run) {
    if (!host || !host->val[5] || !endpoint || !run) return EINVAL;
    *endpoint = MACH_PORT_NULL;
    *run = NULL;
    char executable[PATH_MAX], canonical[PATH_MAX], library[PATH_MAX];
    uint32_t size = sizeof(executable);
    if (_NSGetExecutablePath(executable, &size)) return ENAMETOOLONG;
    if (!realpath(executable, canonical)) return errno;
    char *end = strrchr(canonical, '/');
    if (!end) return EINVAL;
    *end = 0;
    int length = snprintf(library, sizeof(library), "%s/Frameworks/NativeSigner/NativeSigner.dylib", canonical);
    if (length < 0 || length >= (int)sizeof(library)) return ENAMETOOLONG;
    if (!realpath(library, executable) || strcmp(library, executable)) return EACCES;
    void *handle = dlopen(library, RTLD_NOW | RTLD_LOCAL);
    if (!handle) return ENOEXEC;
    LSBNativeSignerPrepareFunction prepare = (LSBNativeSignerPrepareFunction)dlsym(handle, "LSBNativeSignerPrepare");
    LSBNativeSignerRunFunction entry = (LSBNativeSignerRunFunction)dlsym(handle, "LSBNativeSignerRun");
    Dl_info prepareImage = {0}, runImage = {0};
    if (!prepare || !entry || !dladdr((void *)prepare, &prepareImage) || !dladdr((void *)entry, &runImage) ||
        prepareImage.dli_fbase != runImage.dli_fbase || !prepareImage.dli_fname ||
        !realpath(prepareImage.dli_fname, executable) || strcmp(executable, library)) return ENOEXEC;
    int error = prepare(host, endpoint);
    if (!error) *run = entry;
    return error;
}
