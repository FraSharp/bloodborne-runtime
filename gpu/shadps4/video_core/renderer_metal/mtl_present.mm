// SPDX-License-Identifier: GPL-2.0-or-later
// The render adapter's Metal swapchain (mtl_present.h). ARC.

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <cstdio>

#include "video_core/renderer_metal/mtl_frame.h"
#include "video_core/renderer_metal/mtl_present.h"
#include "video_core/renderer_metal/mtl_vk_layer.h"

namespace MetalPresent {

void Configure(void* layer_ptr, const LayerConfig& config) {
    CAMetalLayer* layer = (__bridge CAMetalLayer*)layer_ptr;
    layer.device = (__bridge id<MTLDevice>)MetalLayer::MtlDevice();
    // Presents copy into the drawable (a blit), so its texture is not framebuffer-only.
    layer.framebufferOnly = NO;
    layer.maximumDrawableCount = std::clamp<NSUInteger>(config.max_drawables, 2, 3);
    layer.displaySyncEnabled = config.display_sync;
    layer.opaque = YES;
    CGColorSpaceRef space = CGColorSpaceCreateWithName(
        config.hdr ? kCGColorSpaceITUR_2100_PQ : kCGColorSpaceSRGB);
    layer.colorspace = space;
    CGColorSpaceRelease(space);
    layer.pixelFormat = config.hdr ? MTLPixelFormatRGB10A2Unorm : MTLPixelFormatBGRA8Unorm;
    layer.wantsExtendedDynamicRangeContent = config.hdr;
}

std::pair<u32, u32> FitDrawableSize(void* layer_ptr) {
    CAMetalLayer* layer = (__bridge CAMetalLayer*)layer_ptr;
    const CGSize bounds = layer.bounds.size;
    const CGFloat scale = layer.contentsScale;
    const u32 width = u32(std::max<CGFloat>(0, bounds.width * scale));
    const u32 height = u32(std::max<CGFloat>(0, bounds.height * scale));
    if (width && height) {
        layer.drawableSize = CGSizeMake(width, height);
    }
    return {width, height};
}

bool DisplaySupportsHdr(void* layer_ptr) {
    (void)layer_ptr;
    // Every display macOS drives can take extended-range content; whether it shows more than SDR
    // is the display's (as MoltenVK reports the HDR10 surface format on any Mac).
    return true;
}

bool Present(void* layer_ptr, void* image, u32 width, u32 height, std::function<void()> done,
             const std::string& dump_path) {
    @autoreleasepool {
        CAMetalLayer* layer = (__bridge CAMetalLayer*)layer_ptr;
        id<CAMetalDrawable> drawable = [layer nextDrawable];
        id<MTLTexture> source = MetalFrame::ImageTexture(static_cast<VkImage>(image));
        if (!drawable || !source) {
            if (done) {
                done();
            }
            return false;
        }
        id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)MetalLayer::MtlQueue();
        id<MTLCommandBuffer> cmd = [queue commandBuffer];
        cmd.label = @"bbport Metal present";
        id<MTLTexture> target = drawable.texture;
        const NSUInteger copy_width = std::min<NSUInteger>(width, target.width);
        const NSUInteger copy_height = std::min<NSUInteger>(height, target.height);
        id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
        [blit copyFromTexture:source
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(copy_width, copy_height, 1)
                    toTexture:target
             destinationSlice:0
             destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];
        id<MTLBuffer> dump;
        if (!dump_path.empty()) {
            dump = [queue.device newBufferWithLength:copy_width * copy_height * 4
                                             options:MTLResourceStorageModeShared];
            [blit copyFromTexture:target
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(copy_width, copy_height, 1)
                            toBuffer:dump
                   destinationOffset:0
              destinationBytesPerRow:copy_width * 4
            destinationBytesPerImage:copy_width * copy_height * 4];
        }
        [blit endEncoding];
        [cmd presentDrawable:drawable];
        __block std::function<void()> on_done = std::move(done);
        __block std::string path = dump_path;
        [cmd addCompletedHandler:^(id<MTLCommandBuffer>) {
          if (dump) {
              if (FILE* f = std::fopen(path.c_str(), "wb")) {
                  std::fwrite(dump.contents, 1, dump.length, f);
                  std::fclose(f);
              }
          }
          if (on_done) {
              on_done();
          }
        }];
        [cmd commit];
        return true;
    }
}

} // namespace MetalPresent
