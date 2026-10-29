// Presentation through Metal (present.h): a frame is copied into one of three shared buffers and blitted into the layer's next drawable, whose size is the frame's; the
// layer stretches it over the view (nearest neighbour when enlarging). A frame that arrives while the GPU still reads all three buffers is dropped
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <string.h>
#include "present.h"

enum { RING = 3 };
static CAMetalLayer *g_layer;
static id<MTLCommandQueue> g_queue;
static id<MTLBuffer> g_buf[RING];
static int g_next;
static dispatch_semaphore_t g_free;   // buffers the GPU is done with; command buffers complete in order, so the next in the ring is the one freed

int present_init(void *layer) {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (!dev) return 0;
    g_layer = (__bridge CAMetalLayer *)layer;
    g_layer.device = dev; g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm; g_layer.opaque = YES;
    g_layer.framebufferOnly = NO;   // a blit destination
    g_layer.magnificationFilter = kCAFilterNearest;
    CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB); g_layer.colorspace = srgb; CGColorSpaceRelease(srgb);
    g_queue = [dev newCommandQueue];
    g_free = dispatch_semaphore_create(RING);
    return g_queue != nil;
}

void present_frame(const uint32_t *bgra, int w, int h) {
    if (!g_queue || w <= 0 || h <= 0) return;
    @autoreleasepool {
        size_t row = (size_t)w * 4, size = row * (size_t)h;
        if (dispatch_semaphore_wait(g_free, DISPATCH_TIME_NOW)) return;
        int i = g_next; g_next = (g_next + 1) % RING;
        if (g_buf[i].length < size) g_buf[i] = [g_queue.device newBufferWithLength:size options:MTLResourceStorageModeShared];
        if (!g_buf[i]) { dispatch_semaphore_signal(g_free); return; }
        memcpy(g_buf[i].contents, bgra, size);
        if (!CGSizeEqualToSize(g_layer.drawableSize, CGSizeMake(w, h))) g_layer.drawableSize = CGSizeMake(w, h);
        id<CAMetalDrawable> d = [g_layer nextDrawable];
        if (!d) { dispatch_semaphore_signal(g_free); return; }
        id<MTLCommandBuffer> cb = [g_queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromBuffer:g_buf[i] sourceOffset:0 sourceBytesPerRow:row sourceBytesPerImage:size sourceSize:MTLSizeMake((NSUInteger)w, (NSUInteger)h, 1)
                   toTexture:d.texture destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
        dispatch_semaphore_t done = g_free;
        [cb addCompletedHandler:^(id<MTLCommandBuffer> b) { (void)b; dispatch_semaphore_signal(done); }];
        [cb presentDrawable:d]; [cb commit];
    }
}
