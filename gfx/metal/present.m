// Presentation through Metal (present.h): each frame is the layer's next drawable, cleared to black, as there is nothing to draw before phase 1
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include "present.h"

static CAMetalLayer *g_layer;
static id<MTLCommandQueue> g_queue;

int present_init(void *layer) {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (!dev) return 0;
    g_layer = (__bridge CAMetalLayer *)layer;
    g_layer.device = dev; g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm; g_layer.opaque = YES;
    g_queue = [dev newCommandQueue];
    return g_queue != nil;
}

void present_frame(void) {
    if (!g_queue) return;
    @autoreleasepool {
        id<CAMetalDrawable> d = [g_layer nextDrawable];
        if (!d) return;   // none free (every drawable still on screen): this frame is skipped
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = d.texture; pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        pass.colorAttachments[0].loadAction = MTLLoadActionClear; pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLCommandBuffer> cb = [g_queue commandBuffer];
        [[cb renderCommandEncoderWithDescriptor:pass] endEncoding];
        [cb presentDrawable:d]; [cb commit];
    }
}
