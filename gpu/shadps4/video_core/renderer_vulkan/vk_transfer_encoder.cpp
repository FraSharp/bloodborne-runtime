// The Vulkan runtime's transfer commands as Vulkan commands (vk_transfer_encoder.h).
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/vk_transfer_encoder.h"

namespace Vulkan {
namespace {

class VulkanEncoder final : public TransferEncoder {
public:
    void CopyBuffer(vk::CommandBuffer cmd, vk::Buffer src, vk::Buffer dst,
                    std::span<const vk::BufferCopy> regions) const override {
        cmd.copyBuffer(src, dst, u32(regions.size()), regions.data());
    }
    void FillBuffer(vk::CommandBuffer cmd, vk::Buffer dst, u64 offset, u64 size,
                    u32 value) const override {
        cmd.fillBuffer(dst, offset, size, value);
    }
    void UpdateBuffer(vk::CommandBuffer cmd, vk::Buffer dst, u64 offset,
                      std::span<const u8> data) const override {
        cmd.updateBuffer(dst, offset, data.size_bytes(), data.data());
    }
    void CopyBufferToImage(vk::CommandBuffer cmd, vk::Buffer src, vk::Image dst,
                           std::span<const vk::BufferImageCopy> regions) const override {
        cmd.copyBufferToImage(src, dst, vk::ImageLayout::eTransferDstOptimal, u32(regions.size()),
                              regions.data());
    }
    void CopyImageToBuffer(vk::CommandBuffer cmd, vk::Image src, vk::Buffer dst,
                           std::span<const vk::BufferImageCopy> regions) const override {
        cmd.copyImageToBuffer(src, vk::ImageLayout::eTransferSrcOptimal, dst, u32(regions.size()),
                              regions.data());
    }
    void CopyImage(vk::CommandBuffer cmd, vk::Image src, vk::Image dst,
                   std::span<const vk::ImageCopy> regions) const override {
        cmd.copyImage(src, vk::ImageLayout::eTransferSrcOptimal, dst,
                      vk::ImageLayout::eTransferDstOptimal, u32(regions.size()), regions.data());
    }
    void ResolveImage(vk::CommandBuffer cmd, vk::Image src, vk::Image dst,
                      const vk::ImageResolve& region) const override {
        cmd.resolveImage(src, vk::ImageLayout::eTransferSrcOptimal, dst,
                         vk::ImageLayout::eTransferDstOptimal, region);
    }
    void ClearColorImage(vk::CommandBuffer cmd, vk::Image image, const vk::ClearColorValue& color,
                         const vk::ImageSubresourceRange& range) const override {
        cmd.clearColorImage(image, vk::ImageLayout::eTransferDstOptimal, color, range);
    }
    void Barrier(vk::CommandBuffer cmd, const vk::DependencyInfo& info) const override {
        cmd.pipelineBarrier2(info);
    }
};

} // namespace

const TransferEncoder& VulkanTransferEncoder() {
    static const VulkanEncoder encoder;
    return encoder;
}

} // namespace Vulkan
