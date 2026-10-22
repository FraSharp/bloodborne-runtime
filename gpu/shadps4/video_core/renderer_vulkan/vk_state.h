// The Vulkan backend's view of the render adapter's pipeline state (docs/RENDER_ADAPTER.md, A1).
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <bit>

#include "video_core/renderer_backend/resource.h"
#include "video_core/renderer_backend/state.h"
#include "video_core/renderer_vulkan/vk_format.h"

namespace Vulkan {

#define BB_BACKEND_STATE_CHECK(vk_type, list)                                                      \
    list(BB_BACKEND_STATE_CHECK_ONE_##vk_type)
#define BB_BACKEND_STATE_CHECK_ONE(vk_type, name, value)                                           \
    static_assert(static_cast<s64>(vk::vk_type::e##name) == value, #vk_type " value of " #name);
#define BB_BACKEND_STATE_CHECK_ONE_BlendFactor(name, value) BB_BACKEND_STATE_CHECK_ONE(BlendFactor, name, value)
#define BB_BACKEND_STATE_CHECK_ONE_BlendOp(name, value) BB_BACKEND_STATE_CHECK_ONE(BlendOp, name, value)
#define BB_BACKEND_STATE_CHECK_ONE_CompareOp(name, value) BB_BACKEND_STATE_CHECK_ONE(CompareOp, name, value)
#define BB_BACKEND_STATE_CHECK_ONE_StencilOp(name, value) BB_BACKEND_STATE_CHECK_ONE(StencilOp, name, value)
#define BB_BACKEND_STATE_CHECK_ONE_CullModeFlagBits(name, value) BB_BACKEND_STATE_CHECK_ONE(CullModeFlagBits, name, value)
#define BB_BACKEND_STATE_CHECK_ONE_FrontFace(name, value) BB_BACKEND_STATE_CHECK_ONE(FrontFace, name, value)
#define BB_BACKEND_STATE_CHECK_ONE_PrimitiveTopology(name, value) BB_BACKEND_STATE_CHECK_ONE(PrimitiveTopology, name, value)
#define BB_BACKEND_STATE_CHECK_ONE_PolygonMode(name, value) BB_BACKEND_STATE_CHECK_ONE(PolygonMode, name, value)
#define BB_BACKEND_STATE_CHECK_ONE_LogicOp(name, value) BB_BACKEND_STATE_CHECK_ONE(LogicOp, name, value)
#define BB_BACKEND_STATE_CHECK_ONE_Filter(name, value) BB_BACKEND_STATE_CHECK_ONE(Filter, name, value)
#define BB_BACKEND_STATE_CHECK_ONE_SamplerMipmapMode(name, value) BB_BACKEND_STATE_CHECK_ONE(SamplerMipmapMode, name, value)
#define BB_BACKEND_STATE_CHECK_ONE_SamplerAddressMode(name, value) BB_BACKEND_STATE_CHECK_ONE(SamplerAddressMode, name, value)
#define BB_BACKEND_STATE_CHECK_ONE_BorderColor(name, value) BB_BACKEND_STATE_CHECK_ONE(BorderColor, name, value)
BB_BACKEND_STATE_CHECK(BlendFactor, BB_BACKEND_BLEND_FACTORS)
BB_BACKEND_STATE_CHECK(BlendOp, BB_BACKEND_BLEND_OPS)
BB_BACKEND_STATE_CHECK(CompareOp, BB_BACKEND_COMPARE_OPS)
BB_BACKEND_STATE_CHECK(StencilOp, BB_BACKEND_STENCIL_OPS)
BB_BACKEND_STATE_CHECK(CullModeFlagBits, BB_BACKEND_CULL_MODES)
BB_BACKEND_STATE_CHECK(FrontFace, BB_BACKEND_FRONT_FACES)
BB_BACKEND_STATE_CHECK(PrimitiveTopology, BB_BACKEND_PRIMITIVE_TOPOLOGIES)
BB_BACKEND_STATE_CHECK(PolygonMode, BB_BACKEND_POLYGON_MODES)
BB_BACKEND_STATE_CHECK(LogicOp, BB_BACKEND_LOGIC_OPS)
BB_BACKEND_STATE_CHECK(Filter, BB_BACKEND_FILTERS)
BB_BACKEND_STATE_CHECK(SamplerMipmapMode, BB_BACKEND_MIPMAP_MODES)
BB_BACKEND_STATE_CHECK(SamplerAddressMode, BB_BACKEND_ADDRESS_MODES)
BB_BACKEND_STATE_CHECK(BorderColor, BB_BACKEND_BORDER_COLORS)
static_assert(static_cast<s32>(vk::BorderColor::eFloatCustomEXT) ==
              static_cast<s32>(Backend::BorderColor::FloatCustom));
#undef BB_BACKEND_STATE_CHECK_ONE_BlendFactor
#undef BB_BACKEND_STATE_CHECK_ONE_BlendOp
#undef BB_BACKEND_STATE_CHECK_ONE_CompareOp
#undef BB_BACKEND_STATE_CHECK_ONE_StencilOp
#undef BB_BACKEND_STATE_CHECK_ONE_CullModeFlagBits
#undef BB_BACKEND_STATE_CHECK_ONE_FrontFace
#undef BB_BACKEND_STATE_CHECK_ONE_PrimitiveTopology
#undef BB_BACKEND_STATE_CHECK_ONE_PolygonMode
#undef BB_BACKEND_STATE_CHECK_ONE_LogicOp
#undef BB_BACKEND_STATE_CHECK_ONE_Filter
#undef BB_BACKEND_STATE_CHECK_ONE_SamplerMipmapMode
#undef BB_BACKEND_STATE_CHECK_ONE_SamplerAddressMode
#undef BB_BACKEND_STATE_CHECK_ONE_BorderColor
#undef BB_BACKEND_STATE_CHECK_ONE
#undef BB_BACKEND_STATE_CHECK
static_assert(static_cast<u32>(vk::ColorComponentFlagBits::eR) == u32(Backend::ColorWriteMask::R) &&
              static_cast<u32>(vk::ColorComponentFlagBits::eG) == u32(Backend::ColorWriteMask::G) &&
              static_cast<u32>(vk::ColorComponentFlagBits::eB) == u32(Backend::ColorWriteMask::B) &&
              static_cast<u32>(vk::ColorComponentFlagBits::eA) == u32(Backend::ColorWriteMask::A));
static_assert(static_cast<s32>(vk::ImageType::e1D) == s32(Backend::ImageType::Image1D) &&
              static_cast<s32>(vk::ImageType::e2D) == s32(Backend::ImageType::Image2D) &&
              static_cast<s32>(vk::ImageType::e3D) == s32(Backend::ImageType::Image3D));
static_assert(u32(vk::ImageUsageFlagBits::eTransferSrc) == u32(Backend::ImageUse::TransferSource) &&
              u32(vk::ImageUsageFlagBits::eTransferDst) == u32(Backend::ImageUse::TransferDestination) &&
              u32(vk::ImageUsageFlagBits::eSampled) == u32(Backend::ImageUse::Sampled) &&
              u32(vk::ImageUsageFlagBits::eStorage) == u32(Backend::ImageUse::Storage) &&
              u32(vk::ImageUsageFlagBits::eColorAttachment) == u32(Backend::ImageUse::ColorAttachment) &&
              u32(vk::ImageUsageFlagBits::eDepthStencilAttachment) ==
                  u32(Backend::ImageUse::DepthStencilAttachment) &&
              u32(vk::ImageUsageFlagBits::eAttachmentFeedbackLoopEXT) ==
                  u32(Backend::ImageUse::AttachmentFeedbackLoop));
// Pipeline keys hold write masks; the serialized shader cache depends on their size.
static_assert(sizeof(Backend::ColorWriteMask) == sizeof(vk::ColorComponentFlags));

// The values are Vulkan's, so the conversions are casts.
[[nodiscard]] constexpr vk::BlendFactor ToVk(Backend::BlendFactor v) noexcept {
    return static_cast<vk::BlendFactor>(v);
}
[[nodiscard]] constexpr vk::BlendOp ToVk(Backend::BlendOp v) noexcept {
    return static_cast<vk::BlendOp>(v);
}
[[nodiscard]] constexpr vk::CompareOp ToVk(Backend::CompareOp v) noexcept {
    return static_cast<vk::CompareOp>(v);
}
[[nodiscard]] constexpr vk::StencilOp ToVk(Backend::StencilOp v) noexcept {
    return static_cast<vk::StencilOp>(v);
}
[[nodiscard]] constexpr vk::CullModeFlags ToVk(Backend::CullMode v) noexcept {
    return vk::CullModeFlags(static_cast<VkCullModeFlags>(v));
}
[[nodiscard]] constexpr vk::FrontFace ToVk(Backend::FrontFace v) noexcept {
    return static_cast<vk::FrontFace>(v);
}
[[nodiscard]] constexpr vk::PrimitiveTopology ToVk(Backend::PrimitiveTopology v) noexcept {
    return static_cast<vk::PrimitiveTopology>(v);
}
[[nodiscard]] constexpr vk::PolygonMode ToVk(Backend::PolygonMode v) noexcept {
    return static_cast<vk::PolygonMode>(v);
}
[[nodiscard]] constexpr vk::LogicOp ToVk(Backend::LogicOp v) noexcept {
    return static_cast<vk::LogicOp>(v);
}
[[nodiscard]] constexpr vk::Filter ToVk(Backend::Filter v) noexcept {
    return static_cast<vk::Filter>(v);
}
[[nodiscard]] constexpr vk::SamplerMipmapMode ToVk(Backend::MipmapMode v) noexcept {
    return static_cast<vk::SamplerMipmapMode>(v);
}
[[nodiscard]] constexpr vk::SamplerAddressMode ToVk(Backend::AddressMode v) noexcept {
    return static_cast<vk::SamplerAddressMode>(v);
}
[[nodiscard]] constexpr vk::BorderColor ToVk(Backend::BorderColor v) noexcept {
    return static_cast<vk::BorderColor>(v);
}
[[nodiscard]] constexpr vk::ImageType ToVk(Backend::ImageType v) noexcept {
    return static_cast<vk::ImageType>(v);
}
[[nodiscard]] constexpr vk::ImageUsageFlags ToVk(Backend::ImageUse v) noexcept {
    return vk::ImageUsageFlags(static_cast<VkImageUsageFlags>(v));
}
[[nodiscard]] constexpr Backend::ImageUse FromVk(vk::ImageUsageFlags v) noexcept {
    return static_cast<Backend::ImageUse>(static_cast<VkImageUsageFlags>(v));
}
[[nodiscard]] constexpr vk::ColorComponentFlags ToVk(Backend::ColorWriteMask v) noexcept {
    return vk::ColorComponentFlags(static_cast<VkColorComponentFlags>(v));
}
template <std::size_t N>
[[nodiscard]] inline std::array<vk::ColorComponentFlags, N> ToVk(
    const std::array<Backend::ColorWriteMask, N>& masks) noexcept {
    return std::bit_cast<std::array<vk::ColorComponentFlags, N>>(masks);
}

} // namespace Vulkan
