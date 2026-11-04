// Offscreen adaptation of MacWSBootingGuide/TestMetalIOSurface; see docs/metal.md. No system framebuffer access.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#import <ImageIO/ImageIO.h>
#import <CoreGraphics/CoreGraphics.h>
#import <simd/simd.h>

#include <stdarg.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#if LSB_PROBE_COMPILER_DIAGNOSTICS
#include <xpc/xpc.h>
extern char *xpc_connection_copy_invalidation_reason(xpc_connection_t connection);
#endif
#if LSB_PROBE_REGISTER_COMPILER
extern void xpc_add_bundle(const char *path, int flags);
#endif
#if defined(LSB_PROBE_METALLIB_HEADER)
#include LSB_PROBE_METALLIB_HEADER
#endif

enum { ProbeWidth = 256, ProbeHeight = 256, ProbeBytesPerPixel = 4 };

typedef struct {
    vector_float4 position;
    vector_float4 color;
} Vertex;

static void Report(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    fputs("METAL_IOSURFACE_PROBE ", stderr);
    vfprintf(stderr, format, arguments);
    fputc('\n', stderr);
    fflush(stderr);
    va_end(arguments);
}

static BOOL Fail(const char *stage, NSError *error) {
    const char *description = error ? error.description.UTF8String : "operation returned nil";
    Report("FAIL stage=%s error=%s", stage, description ?: "unavailable error description");
    return NO;
}

#if LSB_PROBE_COMPILER_DIAGNOSTICS
static char *CopyCompilerInvalidationReason(xpc_connection_t connection) {
    char *reason = xpc_connection_copy_invalidation_reason(connection);
    int savedError = errno;
    const char *name = xpc_connection_get_name(connection);
    if (name && strcmp(name, "com.apple.MTLCompilerService") == 0)
        Report("stage=compiler-invalidation service=%s reason=%.2048s", name, reason ?: "<unavailable>");
    errno = savedError;
    return reason; // Preserve the original allocation and caller ownership.
}

__attribute__((used, section("__DATA,__interpose")))
static const struct { const void *replacement, *original; } CompilerInvalidationInterpose = {
    (const void *)CopyCompilerInvalidationReason, (const void *)xpc_connection_copy_invalidation_reason,
};
#endif

#if LSB_PROBE_REGISTER_COMPILER
static BOOL RegisterCompilerBundle(void) {
    const char *root = getenv("DYLD_ROOT_PATH");
    char canonicalRoot[PATH_MAX], bundle[PATH_MAX], canonicalBundle[PATH_MAX], file[PATH_MAX];
    struct stat info;
    if (!root || root[0] != '/' || !realpath(root, canonicalRoot) || stat(canonicalRoot, &info) || !S_ISDIR(info.st_mode)) {
        Report("FAIL stage=compiler-registration invalid-DYLD_ROOT_PATH");
        return NO;
    }
    size_t rootLength = strlen(canonicalRoot);
    if (rootLength <= 1) {
        Report("FAIL stage=compiler-registration expected-contained-runtime-root");
        return NO;
    }
    int count = snprintf(bundle, sizeof(bundle), "%s/System/Library/Frameworks/Metal.framework/XPCServices/MTLCompilerService.xpc", canonicalRoot);
    if (count < 0 || (size_t)count >= sizeof(bundle) || !realpath(bundle, canonicalBundle) ||
        strncmp(canonicalBundle, canonicalRoot, rootLength) || canonicalBundle[rootLength] != '/' ||
        stat(canonicalBundle, &info) || !S_ISDIR(info.st_mode)) {
        Report("FAIL stage=compiler-registration missing-or-uncontained-bundle");
        return NO;
    }
    const char *required[] = {"Info.plist", "MTLCompilerService"};
    for (size_t index = 0; index < sizeof(required) / sizeof(required[0]); ++index) {
        count = snprintf(file, sizeof(file), "%s/%s", canonicalBundle, required[index]);
        if (count < 0 || (size_t)count >= sizeof(file) || lstat(file, &info) || !S_ISREG(info.st_mode) || access(file, R_OK)) {
            Report("FAIL stage=compiler-registration missing-regular-readable-file=%s", required[index]);
            return NO;
        }
    }
    Report("stage=compiler-registration-request path=%s flags=2 result=void", canonicalBundle);
    xpc_add_bundle(canonicalBundle, 2);
    Report("stage=compiler-registration-returned success-unconfirmed=YES");
    return YES;
}
#endif

static IOSurfaceRef CreateSurface(void) {
    const size_t bytesPerRow = ProbeWidth * ProbeBytesPerPixel;
    NSDictionary *properties = @{
        (__bridge NSString *)kIOSurfaceWidth: @(ProbeWidth),
        (__bridge NSString *)kIOSurfaceHeight: @(ProbeHeight),
        (__bridge NSString *)kIOSurfaceBytesPerElement: @(ProbeBytesPerPixel),
        (__bridge NSString *)kIOSurfaceBytesPerRow: @(bytesPerRow),
        (__bridge NSString *)kIOSurfaceAllocSize: @(bytesPerRow * ProbeHeight),
        (__bridge NSString *)kIOSurfacePixelFormat: @(0x42475241u), // 'BGRA'; linear, nonplanar, 8 bits per channel.
    };
    Report("stage=IOSurfaceCreate width=%d height=%d format=BGRA8", ProbeWidth, ProbeHeight);
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)properties);
    if (!surface) {
        Fail("IOSurfaceCreate", nil);
        return NULL;
    }
    size_t width = IOSurfaceGetWidth(surface), height = IOSurfaceGetHeight(surface), stride = IOSurfaceGetBytesPerRow(surface);
    if (width != ProbeWidth || height != ProbeHeight || IOSurfaceGetBytesPerElement(surface) != ProbeBytesPerPixel ||
        IOSurfaceGetPixelFormat(surface) != 0x42475241u || stride < bytesPerRow || stride > SIZE_MAX / height ||
        IOSurfaceGetAllocSize(surface) < stride * height) {
        Report("FAIL stage=IOSurface-layout width=%zu height=%zu stride=%zu allocation=%zu", width, height, stride,
               IOSurfaceGetAllocSize(surface));
        CFRelease(surface);
        return NULL;
    }
    Report("stage=IOSurface-created id=%u stride=%zu allocation=%zu", IOSurfaceGetID(surface), stride, IOSurfaceGetAllocSize(surface));
    return surface;
}

static BOOL RenderSurface(IOSurfaceRef surface) {
    Report("stage=MTLCreateSystemDefaultDevice");
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) return Fail("MTLCreateSystemDefaultDevice", nil);
    Report("stage=Metal-device name=%s", device.name.UTF8String ?: "unknown");

    Report("stage=newCommandQueue");
    id<MTLCommandQueue> queue = [device newCommandQueue];
    if (!queue) return Fail("newCommandQueue", nil);


#if !defined(LSB_PROBE_CLEAR_ONLY)
    NSError *error = nil;
#if defined(LSB_PROBE_METALLIB_HEADER)
    Report("stage=newLibraryWithData bytes=%zu", sizeof(LSBProbeMetalLibrary));
    dispatch_data_t data = dispatch_data_create(LSBProbeMetalLibrary, sizeof(LSBProbeMetalLibrary), NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    id<MTLLibrary> library = [device newLibraryWithData:data error:&error];
    if (!library) return Fail("newLibraryWithData", error);
#else
    NSString *source = @"#include <metal_stdlib>\n"
        "using namespace metal;\n"
        "struct Vertex { float4 position; float4 color; };\n"
        "struct VertexOut { float4 position [[position]]; float4 color; };\n"
        "vertex VertexOut vertex_main(uint vertex_id [[vertex_id]], const device Vertex *vertices [[buffer(0)]]) {\n"
        "    VertexOut out; out.position = vertices[vertex_id].position; out.color = vertices[vertex_id].color; return out;\n"
        "}\n"
        "fragment float4 fragment_main(VertexOut in [[stage_in]]) { return in.color; }\n";
    Report("stage=newLibraryWithSource");
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    if (!library) return Fail("newLibraryWithSource", error);
#endif

    id<MTLFunction> vertex = [library newFunctionWithName:@"vertex_main"];
    if (!vertex) return Fail("vertex_main", nil);
    id<MTLFunction> fragment = [library newFunctionWithName:@"fragment_main"];
    if (!fragment) return Fail("fragment_main", nil);

    MTLRenderPipelineDescriptor *pipelineDescriptor = [[MTLRenderPipelineDescriptor alloc] init];
    if (!pipelineDescriptor) return Fail("MTLRenderPipelineDescriptor", nil);
    pipelineDescriptor.vertexFunction = vertex;
    pipelineDescriptor.fragmentFunction = fragment;
    pipelineDescriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    Report("stage=newRenderPipelineState");
    error = nil;
    id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:pipelineDescriptor error:&error];
    if (!pipeline) return Fail("newRenderPipelineState", error);
#endif

    MTLTextureDescriptor *textureDescriptor = [[MTLTextureDescriptor alloc] init];
    if (!textureDescriptor) return Fail("MTLTextureDescriptor", nil);
    textureDescriptor.textureType = MTLTextureType2D;
    textureDescriptor.pixelFormat = MTLPixelFormatBGRA8Unorm;
    textureDescriptor.width = ProbeWidth;
    textureDescriptor.height = ProbeHeight;
    textureDescriptor.storageMode = MTLStorageModeShared;
    textureDescriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    Report("stage=newTextureWithIOSurface");
    id<MTLTexture> texture = [device newTextureWithDescriptor:textureDescriptor iosurface:surface plane:0];
    if (!texture) return Fail("newTextureWithIOSurface", nil);


#if !defined(LSB_PROBE_CLEAR_ONLY)
    const Vertex vertices[] = {
        {{-0.8f, -0.8f, 0, 1}, {1, 0, 0, 1}},
        {{ 0.8f, -0.8f, 0, 1}, {0, 1, 0, 1}},
        {{ 0.0f,  0.8f, 0, 1}, {0, 0, 1, 1}},
    };
    id<MTLBuffer> vertexBuffer = [device newBufferWithBytes:vertices length:sizeof(vertices) options:MTLResourceStorageModeShared];
    if (!vertexBuffer) return Fail("newVertexBuffer", nil);
#endif

    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    if (!pass) return Fail("MTLRenderPassDescriptor", nil);
    pass.colorAttachments[0].texture = texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;

#if defined(LSB_PROBE_CLEAR_ONLY)
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.75, 0.5, 0.25, 1);
#else
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
#endif
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;

    id<MTLCommandBuffer> command = [queue commandBuffer];
    if (!command) return Fail("commandBuffer", nil);
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    if (!encoder) return Fail("renderCommandEncoder", nil);

#if !defined(LSB_PROBE_CLEAR_ONLY)
    [encoder setRenderPipelineState:pipeline];
    [encoder setVertexBuffer:vertexBuffer offset:0 atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
#endif
    [encoder endEncoding];
    Report("stage=commit");
    [command commit];
    Report("stage=waitUntilCompleted");
    [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted || command.error) {
        Report("FAIL stage=command-completion status=%lu", (unsigned long)command.status);
        return Fail("command-completion", command.error);
    }
    Report("stage=GPU-completed");
    return YES;
}

static CGImageRef CopySurfaceImage(IOSurfaceRef surface) {
    size_t width = IOSurfaceGetWidth(surface), height = IOSurfaceGetHeight(surface), stride = IOSurfaceGetBytesPerRow(surface);
    const uint8_t *base = IOSurfaceGetBaseAddress(surface);
    if (!base || stride > (size_t)LONG_MAX / height) {
        Report("FAIL stage=CPU-base-address base=%p stride=%zu", base, stride);
        return NULL;
    }
    const uint8_t *center = base + (height / 2) * stride + (width / 2) * ProbeBytesPerPixel;
    const uint8_t *corner = base;
    Report("stage=CPU-pixels center-BGRA=%u,%u,%u,%u corner-BGRA=%u,%u,%u,%u", center[0], center[1], center[2], center[3],
           corner[0], corner[1], corner[2], corner[3]);
#if defined(LSB_PROBE_CLEAR_ONLY)
    if (center[0] < 63 || center[0] > 65 || center[1] < 127 || center[1] > 129 || center[2] < 190 || center[2] > 192 ||
        center[3] != 255 || memcmp(center, corner, 4)) {
        Report("FAIL stage=CPU-verification expected=uniform-GPU-clear-BGRA-64,128,191,255");
        return NULL;
    }
#else
    if (!(center[0] || center[1] || center[2]) || center[3] != 255 || corner[0] || corner[1] || corner[2] || corner[3] != 255) {
        Report("FAIL stage=CPU-verification expected=nonblack-opaque-center,black-opaque-corner");
        return NULL;
    }

#endif

    // Own a pixel copy so neither PNG encoding nor CGImage lifetime depends on the surface's read lock.
    CFDataRef pixels = CFDataCreate(kCFAllocatorDefault, base, (CFIndex)(stride * height));
    if (!pixels) { Fail("CFDataCreate", nil); return NULL; }
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(pixels);
    CFRelease(pixels);
    if (!provider) { Fail("CGDataProviderCreateWithCFData", nil); return NULL; }
    CGColorSpaceRef colorSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    if (!colorSpace) { CGDataProviderRelease(provider); Fail("CGColorSpaceCreateWithName", nil); return NULL; }
    CGBitmapInfo bitmap = kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst;
    CGImageRef image = CGImageCreate(width, height, 8, 32, stride, colorSpace, bitmap, provider, NULL, false, kCGRenderingIntentDefault);
    CGColorSpaceRelease(colorSpace);
    CGDataProviderRelease(provider);
    if (!image) Fail("CGImageCreate", nil);
    return image;
}

static BOOL VerifyAndWriteSurface(IOSurfaceRef surface, NSString *path) {
    Report("stage=IOSurfaceLock");
    kern_return_t result = IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
    if (result != KERN_SUCCESS) { Report("FAIL stage=IOSurfaceLock result=0x%08x", result); return NO; }
    CGImageRef image = CopySurfaceImage(surface);
    result = IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
    if (result != KERN_SUCCESS) {
        Report("FAIL stage=IOSurfaceUnlock result=0x%08x", result);
        if (image) CGImageRelease(image);
        return NO;
    }
    if (!image) return NO;

    NSURL *url = [NSURL fileURLWithPath:path];
    if (!url) { CGImageRelease(image); return Fail("fileURLWithPath", nil); }
    Report("stage=PNG-write path=%s", url.path.UTF8String ?: "unknown");
    CGImageDestinationRef destination = CGImageDestinationCreateWithURL((__bridge CFURLRef)url, CFSTR("public.png"), 1, NULL);
    if (!destination) { CGImageRelease(image); return Fail("CGImageDestinationCreateWithURL", nil); }
    CGImageDestinationAddImage(destination, image, NULL);
    BOOL written = CGImageDestinationFinalize(destination);
    CFRelease(destination);
    CGImageRelease(image);
    if (!written) return Fail("CGImageDestinationFinalize", nil);
    Report("PASS rendered=256x256 verified-pixels=YES png=%s", url.path.UTF8String ?: "unknown");
    return YES;
}

int main(int argc, const char *argv[]) {
    alarm(60); // Bound unavailable compiler/service waits in the device diagnostic.
    @autoreleasepool {
        BOOL enableSimApple5 = YES;
        const char *output = "metal-iosurface.png";
        BOOL hasOutput = NO;
        for (int index = 1; index < argc; ++index) {
            if (strcmp(argv[index], "--no-sim-apple5") == 0) enableSimApple5 = NO;
            else if (argv[index][0] == '-' || hasOutput) {
                Report("usage: %s [--no-sim-apple5] [output.png]", argv[0]);
                return 2;
            } else { output = argv[index]; hasOutput = YES; }
        }
        NSString *path = [[NSString alloc] initWithUTF8String:output];
        if (!path.length) { Report("FAIL stage=output-path invalid UTF-8 or empty path"); return 2; }
        IOSurfaceRef surface = NULL;
        int status = 1;
        @try {
#if LSB_PROBE_REGISTER_COMPILER
            if (!RegisterCompilerBundle()) return 1;
#endif
            Report("stage=preferences EnableSimApple5=%s", enableSimApple5 ? "YES" : "NO");
            CFPreferencesSetAppValue(CFSTR("EnableSimApple5"), enableSimApple5 ? kCFBooleanTrue : kCFBooleanFalse, CFSTR("com.apple.Metal"));
            CFPropertyListRef preference = CFPreferencesCopyAppValue(CFSTR("EnableSimApple5"), CFSTR("com.apple.Metal"));
            BOOL validPreference = preference && CFEqual(preference, enableSimApple5 ? kCFBooleanTrue : kCFBooleanFalse);
            if (preference) CFRelease(preference);
            if (!validPreference) Report("FAIL stage=preferences readback did not match requested value");
            else {
                surface = CreateSurface();
                if (surface && RenderSurface(surface) && VerifyAndWriteSurface(surface, path)) status = 0;
            }
        } @catch (NSException *exception) {
            Report("FAIL stage=exception name=%s reason=%s", exception.name.UTF8String ?: "unknown", exception.reason.UTF8String ?: "unknown");
        } @finally {
            if (surface) CFRelease(surface);
        }
        return status;
    }
}
