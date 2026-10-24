// SPDX-License-Identifier: GPL-2.0-or-later
// The render adapter's Metal command stream (mtl_recorder.h).

#include <cstdio>

#include <boost/container/static_vector.hpp>

#include "video_core/renderer_metal/mtl_frame.h"
#include "video_core/renderer_metal/mtl_recorder.h"
#include "video_core/renderer_metal/mtl_vk_layer.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Metal {
namespace {

namespace Backend = VideoCore::Backend;

/// What CommitDrawState hands the encoder: the changed fields of the draw state.
struct DrawStateDelta {
    enum Bit : u32 {
        Viewports = 1u << 0,
        Scissors = 1u << 1,
        DepthTest = 1u << 2,
        DepthWrite = 1u << 3,
        DepthCompare = 1u << 4,
        DepthBiasEnable = 1u << 5,
        DepthBias = 1u << 6,
        StencilTest = 1u << 7,
        FrontOps = 1u << 8,
        BackOps = 1u << 9,
        FrontReference = 1u << 10,
        BackReference = 1u << 11,
        FrontWriteMask = 1u << 12,
        BackWriteMask = 1u << 13,
        FrontCompareMask = 1u << 14,
        BackCompareMask = 1u << 15,
        RasterizerDiscard = 1u << 16,
        CullMode = 1u << 17,
        FrontFace = 1u << 18,
        BlendConstants = 1u << 19,
    };
    u32 bits{};
    Backend::Viewports viewports;
    Backend::Scissors scissors;
    bool depth_test{}, depth_write{}, depth_bias_enable{}, stencil_test{}, rasterizer_discard{};
    Backend::CompareOp depth_compare{};
    float depth_bias[3]{};
    Backend::StencilOps front_ops{}, back_ops{};
    u32 front_reference{}, back_reference{}, front_write_mask{}, back_write_mask{};
    u32 front_compare_mask{}, back_compare_mask{};
    Backend::CullMode cull_mode{};
    Backend::FrontFace front_face{};
    std::array<float, 4> blend_constants{};
};

void SetFace(MetalFrame::Owned::Face& face, const Backend::StencilOps& ops) {
    face.fail = static_cast<VkStencilOp>(ops.fail_op);
    face.pass = static_cast<VkStencilOp>(ops.pass_op);
    face.depth_fail = static_cast<VkStencilOp>(ops.depth_fail_op);
    face.compare = static_cast<VkCompareOp>(ops.compare_op);
}

/// The encoder's side of the draw state (as the layer's vkCmdSet* encodings set it).
void Apply(MetalFrame::Owned& o, const DrawStateDelta& d) {
    using B = DrawStateDelta;
    if (d.bits & B::Viewports) {
        static_assert(sizeof(Backend::Viewport) == sizeof(VkViewport));
        MetalFrame::SetViewports(o, u32(d.viewports.size()),
                                 reinterpret_cast<const VkViewport*>(d.viewports.data()));
    }
    if (d.bits & B::Scissors) {
        static_assert(sizeof(Backend::Rect2D) == sizeof(VkRect2D));
        MetalFrame::SetScissors(o, u32(d.scissors.size()),
                                reinterpret_cast<const VkRect2D*>(d.scissors.data()));
    }
    if (d.bits & B::DepthTest) o.depth_test = d.depth_test;
    if (d.bits & B::DepthWrite) o.depth_write = d.depth_write;
    if (d.bits & B::DepthCompare) o.depth_compare = static_cast<VkCompareOp>(d.depth_compare);
    if (d.bits & B::DepthBiasEnable) o.depth_bias_enable = d.depth_bias_enable;
    if (d.bits & B::DepthBias) std::copy_n(d.depth_bias, 3, o.depth_bias);
    if (d.bits & B::StencilTest) o.stencil_test = d.stencil_test;
    if (d.bits & B::FrontOps) SetFace(o.front, d.front_ops);
    if (d.bits & B::BackOps) SetFace(o.back, d.back_ops);
    if (d.bits & B::FrontReference) o.front.reference = d.front_reference;
    if (d.bits & B::BackReference) o.back.reference = d.back_reference;
    if (d.bits & B::FrontWriteMask) o.front.write_mask = d.front_write_mask;
    if (d.bits & B::BackWriteMask) o.back.write_mask = d.back_write_mask;
    if (d.bits & B::FrontCompareMask) o.front.compare_mask = d.front_compare_mask;
    if (d.bits & B::BackCompareMask) o.back.compare_mask = d.back_compare_mask;
    if (d.bits & B::RasterizerDiscard) o.rasterizer_discard = d.rasterizer_discard;
    if (d.bits & B::CullMode) o.cull_mode = static_cast<VkCullModeFlags>(d.cull_mode);
    if (d.bits & B::FrontFace) o.front_face = static_cast<VkFrontFace>(d.front_face);
    if (d.bits & B::BlendConstants) {
        std::copy_n(d.blend_constants.data(), 4, o.blend_constants);
    }
}

MetalFrame::Owned* Encoder(vk::CommandBuffer cmdbuf) {
    auto* o = MetalLayer::OwnedFor(static_cast<VkCommandBuffer>(cmdbuf));
    if (!o) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            std::printf("Metal recorder: a command buffer the Metal frame does not own (skipped)\n");
        }
    }
    return o;
}

class FrameRecorder final : public Vulkan::CommandRecorder {
public:
    using Vulkan::CommandRecorder::CommandRecorder;

    /// The changed draw state as one command (the Vulkan recorder records one per field, and
    /// the layer translated each back).
    void CommitDrawState() const override {
        auto& s = scheduler.GetDynamicState();
        auto& dirty = s.dirty_state;
        using B = DrawStateDelta;
        DrawStateDelta d;
        if (dirty.viewports) {
            dirty.viewports = false;
            d.bits |= B::Viewports;
            d.viewports = s.viewports;
        }
        if (dirty.scissors) {
            dirty.scissors = false;
            d.bits |= B::Scissors;
            d.scissors = s.scissors;
        }
        if (dirty.depth_test_enabled) {
            dirty.depth_test_enabled = false;
            d.bits |= B::DepthTest;
            d.depth_test = s.depth_test_enabled;
        }
        if (dirty.depth_write_enabled) {
            dirty.depth_write_enabled = false;
            d.bits |= B::DepthWrite;
            d.depth_write = s.depth_write_enabled;
        }
        if (s.depth_test_enabled && dirty.depth_compare_op) {
            dirty.depth_compare_op = false;
            d.bits |= B::DepthCompare;
            d.depth_compare = s.depth_compare_op;
        }
        // No depth bounds test in Metal.
        dirty.depth_bounds_test_enabled = false;
        if (s.depth_bounds_test_enabled) {
            dirty.depth_bounds = false;
        }
        if (dirty.depth_bias_enabled) {
            dirty.depth_bias_enabled = false;
            d.bits |= B::DepthBiasEnable;
            d.depth_bias_enable = s.depth_bias_enabled;
        }
        if (s.depth_bias_enabled && dirty.depth_bias) {
            dirty.depth_bias = false;
            d.bits |= B::DepthBias;
            d.depth_bias[0] = s.depth_bias_constant;
            d.depth_bias[1] = s.depth_bias_clamp;
            d.depth_bias[2] = s.depth_bias_slope;
        }
        if (dirty.stencil_test_enabled) {
            dirty.stencil_test_enabled = false;
            d.bits |= B::StencilTest;
            d.stencil_test = s.stencil_test_enabled;
        }
        if (s.stencil_test_enabled) {
            // (Bit-fields: no references to them.)
#define BB_TAKE(flag, bit)                                                                         \
    if (dirty.flag) {                                                                              \
        dirty.flag = false;                                                                        \
        d.bits |= bit;                                                                             \
    }
            BB_TAKE(stencil_front_ops, B::FrontOps)
            BB_TAKE(stencil_back_ops, B::BackOps)
            BB_TAKE(stencil_front_reference, B::FrontReference)
            BB_TAKE(stencil_back_reference, B::BackReference)
            BB_TAKE(stencil_front_write_mask, B::FrontWriteMask)
            BB_TAKE(stencil_back_write_mask, B::BackWriteMask)
            BB_TAKE(stencil_front_compare_mask, B::FrontCompareMask)
            BB_TAKE(stencil_back_compare_mask, B::BackCompareMask)
#undef BB_TAKE
            d.front_ops = s.stencil_front_ops;
            d.back_ops = s.stencil_back_ops;
            d.front_reference = s.stencil_front_reference;
            d.back_reference = s.stencil_back_reference;
            d.front_write_mask = s.stencil_front_write_mask;
            d.back_write_mask = s.stencil_back_write_mask;
            d.front_compare_mask = s.stencil_front_compare_mask;
            d.back_compare_mask = s.stencil_back_compare_mask;
        }
        // Primitive restart is always on for strips in Metal; no line width.
        dirty.primitive_restart_enable = false;
        dirty.line_width = false;
        if (dirty.rasterizer_discard_enable) {
            dirty.rasterizer_discard_enable = false;
            d.bits |= B::RasterizerDiscard;
            d.rasterizer_discard = s.rasterizer_discard_enable;
        }
        if (dirty.cull_mode) {
            dirty.cull_mode = false;
            d.bits |= B::CullMode;
            d.cull_mode = s.cull_mode;
        }
        if (dirty.front_face) {
            dirty.front_face = false;
            d.bits |= B::FrontFace;
            d.front_face = s.front_face;
        }
        if (dirty.blend_constants) {
            dirty.blend_constants = false;
            d.bits |= B::BlendConstants;
            d.blend_constants = s.blend_constants;
        }
        // No dynamic write masks or feedback-loop layouts in the Metal frame; the flags clear as
        // the Vulkan recorder clears them.
        dirty.color_write_masks = false;
        if (dirty.feedback_loop_enabled &&
            scheduler.GetInstance().IsAttachmentFeedbackLoopLayoutSupported()) {
            dirty.feedback_loop_enabled = false;
        }
        if (!d.bits) {
            return;
        }
        scheduler.Record([d = std::move(d)](vk::CommandBuffer cmdbuf) {
            if (auto* o = Encoder(cmdbuf)) {
                Apply(*o, d);
            }
        });
    }
};

} // namespace

std::unique_ptr<Vulkan::CommandRecorder> MakeFrameRecorder(Vulkan::Scheduler& scheduler,
                                                           Vulkan::DescriptorHeap& descriptor_heap,
                                                           vk::Device device) {
    std::printf("Metal recorder: the draw state is encoded in Metal directly\n");
    return std::make_unique<FrameRecorder>(scheduler, descriptor_heap, device);
}

} // namespace Metal
