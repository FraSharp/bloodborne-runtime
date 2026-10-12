// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_fsr4_metal.h"

#ifdef __APPLE__
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <vulkan/vulkan_metal.h>

#include "bbport_toggles.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "fsr4_metal/fsr411_runtime.h"

namespace Vulkan {

namespace {

id<MTLTexture> ExportTexture(VkDevice device, PFN_vkExportMetalObjectsEXT export_fn, VkImage image) {
    if (!device || !export_fn || !image) return nil;
    VkExportMetalTextureInfoEXT tex_info{
        .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT,
        .image = image,
        .plane = VK_IMAGE_ASPECT_PLANE_0_BIT,
    };
    VkExportMetalObjectsInfoEXT info{
        .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
        .pNext = &tex_info,
    };
    export_fn(device, &info);
    return tex_info.mtlTexture;
}

std::string ResolveFsr4AssetDir() {
    if (const char* env = std::getenv("BB_FSR4_NATIVE_DIR")) {
        if (env[0] && [[NSFileManager defaultManager] fileExistsAtPath:[NSString stringWithUTF8String:env]]) {
            return env;
        }
    }
    if (const char* env = std::getenv("BB_FSR411_DIR")) {
        if (env[0] && [[NSFileManager defaultManager] fileExistsAtPath:[NSString stringWithUTF8String:env]]) {
            return env;
        }
    }
    NSMutableArray<NSString*>* candidates = [NSMutableArray array];
    NSString* p1 = [[NSBundle mainBundle] pathForResource:@"fsr4_metal" ofType:nil inDirectory:@"game"];
    if (p1) [candidates addObject:p1];
    NSString* p2 = [[NSBundle mainBundle] pathForResource:@"fsr4_metal" ofType:nil];
    if (p2) [candidates addObject:p2];
    [candidates addObject:@"fsr4_metal"];
    [candidates addObject:@"gpu/fsr4_metal"];
    [candidates addObject:@"../gpu/fsr4_metal"];
    [candidates addObject:@"runtime/gpu/fsr4_metal"];
    if (const char* env = std::getenv("BB_GPU_USER_DIR")) {
        if (env[0]) [candidates addObject:[NSString stringWithFormat:@"%s/fsr4_metal", env]];
    }
    for (NSString* path in candidates) {
        if ([[NSFileManager defaultManager] fileExistsAtPath:path]) {
            return [path UTF8String];
        }
    }
    return "gpu/fsr4_metal";
}

std::string ResolveFsr4CacheDir() {
    std::string cache_dir;
    if (const char* env = std::getenv("BB_GPU_USER_DIR")) {
        if (env[0]) cache_dir = std::string(env) + "/fsr4_cache";
    }
    if (cache_dir.empty()) {
        if (const char* env = std::getenv("BB_DATA_DIR")) {
            if (env[0]) cache_dir = std::string(env) + "/user/fsr4_cache";
        }
    }
    if (cache_dir.empty()) {
        NSArray<NSString*>* paths = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES);
        NSString* base = paths.count ? paths.firstObject : [NSString stringWithUTF8String:"/tmp"];
        cache_dir = [[base stringByAppendingPathComponent:@"bloodborne_mac/fsr4_cache"] UTF8String];
    }
    [[NSFileManager defaultManager] createDirectoryAtPath:[NSString stringWithUTF8String:cache_dir.c_str()]
                              withIntermediateDirectories:YES attributes:nil error:nil];
    return cache_dir;
}

bool HasFsr4Initializer(const std::string& asset_dir) {
    std::vector<std::string> candidates;
    if (const char* env = std::getenv("BB_FSR411_DIR")) {
        if (env[0]) {
            candidates.push_back(std::string(env) + "/initializers.bin");
            candidates.push_back(std::string(env) + "/t1080_m0/initializer.bin");
            candidates.push_back(std::string(env) + "/initializer.bin");
        }
    }
    if (const char* env = std::getenv("BB_GPU_USER_DIR")) {
        if (env[0]) {
            candidates.push_back(std::string(env) + "/fsr4_411/initializers.bin");
            candidates.push_back(std::string(env) + "/fsr4_411/t1080_m0/initializer.bin");
            candidates.push_back(std::string(env) + "/fsr4_411/initializer.bin");
        }
    }
    candidates.push_back(asset_dir + "/initializers.bin");
    candidates.push_back(asset_dir + "/fsr411/initializers.bin");
    candidates.push_back(asset_dir + "/t1080_m0/initializer.bin");
    candidates.push_back("fsr4_411/initializers.bin");
    candidates.push_back("fsr4_411/t1080_m0/initializer.bin");
    candidates.push_back("fsr4_411/initializer.bin");
    for (const auto& path : candidates) {
        NSDictionary* attrs = [[NSFileManager defaultManager] attributesOfItemAtPath:[NSString stringWithUTF8String:path.c_str()] error:nil];
        if (attrs && [attrs fileSize] == 131072) {
            return true;
        }
    }
    return false;
}

class Fsr4MetalBackendImpl final : public Fsr4MetalBackend {
public:
    Fsr4MetalBackendImpl(const Instance& instance, Scheduler& scheduler)
        : instance_{instance}, scheduler_{scheduler} {
        InitMetal();
    }

    ~Fsr4MetalBackendImpl() override = default;

    bool Record(const Fsr4Upscaler::Frame& frame) override {
        if (!valid_ || !runtime_ || !view_) {
            problem_ = "FSR 4.1.1 Metal backend not initialized";
            return false;
        }

        // bbport: Flush Vulkan commands recorded up to this point so Metal reads the current frame
        scheduler_.Flush();

        // Export underlying Metal textures from MoltenVK Vulkan images
        id<MTLTexture> color_tex = ExportTexture(vk_device_, export_objects_, frame.color.image);
        id<MTLTexture> depth_tex = ExportTexture(vk_device_, export_objects_, frame.depth.image);
        id<MTLTexture> motion_tex = ExportTexture(vk_device_, export_objects_, frame.motion.image);
        id<MTLTexture> output_tex = ExportTexture(vk_device_, export_objects_, frame.output.image);

        if (!color_tex || !depth_tex || !motion_tex || !output_tex) {
            problem_ = "Failed to export Metal textures via VK_EXT_metal_objects";
            return false;
        }

        id<MTLCommandBuffer> cmd_buf = [queue_ commandBuffer];
        if (!cmd_buf) {
            problem_ = "Failed to create Metal command buffer";
            return false;
        }

        fsr411::Frame mtl_frame{};
        mtl_frame.color = color_tex;
        mtl_frame.depth = depth_tex;
        mtl_frame.motion = motion_tex;
        mtl_frame.output = output_tex;

        Fsr4InputDesc& desc = mtl_frame.input;
        desc.rw = frame.render_width;
        desc.rh = frame.render_height;
        desc.uw = frame.output.width;
        desc.uh = frame.output.height;
        desc.jitterX = frame.jitter[0];
        desc.jitterY = frame.jitter[1];
        desc.mvScaleX = 1.0f;
        desc.mvScaleY = BbToggle::Disabled(BbToggle::Fsr4MotionYFlip) ? -1.0f : 1.0f;
        desc.preExposure = 1.0f;
        desc.reset = frame.reset ? 1 : 0;
        desc.depthReversed = 1; // Bloodborne/shadps4 uses reversed depth (1 -> near, 0 -> far)
        desc.autoExposure = frame.auto_exposure ? 1 : 0;
        desc.outputResMV = 0;
        desc.jitteredMV = 0;

        desc.geometry.color = { (uint32_t)color_tex.width, (uint32_t)color_tex.height, 0, 0 };
        desc.geometry.depth = { (uint32_t)depth_tex.width, (uint32_t)depth_tex.height, 0, 0 };
        desc.geometry.motion = { (uint32_t)motion_tex.width, (uint32_t)motion_tex.height, 0, 0 };
        desc.geometry.output = { (uint32_t)output_tex.width, (uint32_t)output_tex.height, 0, 0 };

        fsr411::Options options{};
        options.backend = fsr411::Backend::Auto;
        options.profile = fsr411::Profile::Lossy; // Default high-performance Lossy profile
        options.sharpness = frame.sharpen ? frame.sharpness : 0.0f;
        options.rcas = frame.sharpen && frame.sharpness > 0.0f;
        options.ultraPerformance = frame.preset >= 4;

        std::string err;
        if (!runtime_->encode(cmd_buf, *view_, mtl_frame, options, err)) {
            problem_ = "Metal encode failed: " + err;
            return false;
        }

        [cmd_buf commit];
        // bbport: wait for Metal upscaler pass to complete before subsequent Vulkan passes execute
        [cmd_buf waitUntilCompleted];
        problem_.clear();
        return true;
    }

    const char* Problem() const noexcept override {
        return problem_.empty() ? nullptr : problem_.c_str();
    }

private:
    void InitMetal() {
        if (!instance_.IsMetalObjectsSupported()) {
            problem_ = "MoltenVK lacks VK_EXT_metal_objects extension";
            return;
        }

        vk_device_ = instance_.GetDevice();
        export_objects_ = reinterpret_cast<PFN_vkExportMetalObjectsEXT>(
            instance_.GetDevice().getProcAddr("vkExportMetalObjectsEXT"));
        if (!export_objects_) {
            problem_ = "vkExportMetalObjectsEXT function pointer is null";
            return;
        }

        VkExportMetalDeviceInfoEXT device_info{
            .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_DEVICE_INFO_EXT,
        };
        VkExportMetalCommandQueueInfoEXT queue_info{
            .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_COMMAND_QUEUE_INFO_EXT,
            .pNext = &device_info,
            .queue = instance_.GetGraphicsQueue(),
        };
        VkExportMetalObjectsInfoEXT objects{
            .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
            .pNext = &queue_info,
        };
        export_objects_(vk_device_, &objects);

        device_ = device_info.mtlDevice;
        queue_ = queue_info.mtlCommandQueue;

        if (!device_ || !queue_) {
            problem_ = "Failed to export MTLDevice or MTLCommandQueue";
            return;
        }

        std::string root = ResolveFsr4AssetDir();
        std::string build_dir = ResolveFsr4CacheDir();

        runtime_ = std::make_unique<fsr411::Runtime>(device_, root, build_dir);
        view_ = std::make_unique<fsr411::View>();
        valid_ = true;
    }

    const Instance& instance_;
    Scheduler& scheduler_;
    VkDevice vk_device_{VK_NULL_HANDLE};
    PFN_vkExportMetalObjectsEXT export_objects_{nullptr};
    id<MTLDevice> device_{nil};
    id<MTLCommandQueue> queue_{nil};
    std::unique_ptr<fsr411::Runtime> runtime_;
    std::unique_ptr<fsr411::View> view_;
    bool valid_{false};
    std::string problem_;
};

} // namespace

bool Fsr4MetalBackend::IsSupported(const Instance& instance) {
    if (!instance.IsMetalObjectsSupported()) {
        return false;
    }
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device || ![device supportsFamily:MTLGPUFamilyApple7]) {
        return false;
    }
    std::string root = ResolveFsr4AssetDir();
    bool has_helpers = [[NSFileManager defaultManager] fileExistsAtPath:[NSString stringWithUTF8String:(root + "/fsr411_helpers.metallib").c_str()]] ||
                       [[NSFileManager defaultManager] fileExistsAtPath:[NSString stringWithUTF8String:(root + "/fsr411_helpers.metal").c_str()]];
    bool has_simd = [[NSFileManager defaultManager] fileExistsAtPath:[NSString stringWithUTF8String:(root + "/fsr411_simd.metallib").c_str()]] ||
                    [[NSFileManager defaultManager] fileExistsAtPath:[NSString stringWithUTF8String:(root + "/fsr411_simd.metal").c_str()]];
    if (!has_helpers || !has_simd) {
        return false;
    }
    if (!HasFsr4Initializer(root)) {
        return false;
    }
    return true;
}

std::unique_ptr<Fsr4MetalBackend> Fsr4MetalBackend::Create(const Instance& instance, Scheduler& scheduler) {
    return std::make_unique<Fsr4MetalBackendImpl>(instance, scheduler);
}

} // namespace Vulkan
#endif
