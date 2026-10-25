// SPDX-License-Identifier: GPL-2.0-or-later
// The Metal backend's answers to the renderer's start-up Vulkan queries (mtl_vk_caps.h). ARC.

#import <Metal/Metal.h>

#include <algorithm>
#include <cstdio>
#include <sys/sysctl.h>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_metal/mtl_vk_caps.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace MetalCaps {
namespace {

// ---- The answers ----------------------------------------------------------------------------

struct Field {
    const char* structure;
    const char* name;
    size_t offset, size;
    char kind; ///< B VkBool32, U 32-bit, F float, Q 64-bit, S text, X bytes
};

/// The members of every answered structure (tools/vk_caps_fields.py).
const std::vector<Field>& Fields() {
    static const std::vector<Field> fields = [] {
        std::vector<Field> list;
#define BB_STRUCT(S)
#define BB_FIELD(S, M, K) list.push_back({#S, #M, offsetof(S, M), sizeof(S::M), #K[0]});
#include "video_core/renderer_metal/mtl_vk_caps_fields.inc"
#undef BB_FIELD
#undef BB_STRUCT
        return list;
    }();
    return fields;
}

const Field* FindField(const char* structure, const char* name) {
    for (const Field& f : Fields()) {
        if (!std::strcmp(f.structure, structure) && !std::strcmp(f.name, name)) {
            return &f;
        }
    }
    return nullptr;
}

/// Writes a profile value (its text as the comparison printed it) into a member.
void Store(const Field& f, u8* p, const char* text) {
    switch (f.kind) {
    case 'B':
    case 'U': {
        const u32 v = u32(std::strtoul(text, nullptr, 10));
        std::memcpy(p, &v, 4);
        break;
    }
    case 'F': {
        const float v = std::strtof(text, nullptr);
        std::memcpy(p, &v, 4);
        break;
    }
    case 'Q': {
        const u64 v = std::strtoull(text, nullptr, 10);
        std::memcpy(p, &v, 8);
        break;
    }
    case 'S': {
        std::memset(p, 0, f.size);
        std::string value = text;
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        std::memcpy(p, value.data(), std::min(value.size(), f.size - 1));
        break;
    }
    default:
        for (size_t i = 0; i < f.size && text[2 * i] && text[2 * i + 1]; ++i) {
            const char byte[3] = {text[2 * i], text[2 * i + 1], 0};
            p[i] = u8(std::strtoul(byte, nullptr, 16));
        }
        break;
    }
}

/// The profile's values for one structure (tools/vk_caps_profile.py), written over `base`.
void ApplyProfile(const char* structure, void* base) {
    struct Value {
        const char* structure;
        const char* field;
        const char* text;
    };
    static const Value values[] = {
#define BB_VALUE(S, F, V) {#S, #F, V},
#define BB_FORMAT(...)
#define BB_MEMORY_TYPE(...)
#define BB_MEMORY_HEAP(...)
#define BB_QUEUE_FAMILY(...)
#define BB_EXTENSION(...)
#include "video_core/renderer_metal/mtl_vk_caps_profile.inc"
#undef BB_VALUE
#undef BB_FORMAT
#undef BB_MEMORY_TYPE
#undef BB_MEMORY_HEAP
#undef BB_QUEUE_FAMILY
#undef BB_EXTENSION
    };
    for (const Value& v : values) {
        if (std::strcmp(v.structure, structure) != 0) {
            continue;
        }
        if (const Field* f = FindField(v.structure, v.field)) {
            Store(*f, static_cast<u8*>(base) + f->offset, v.text);
        }
    }
}

id<MTLDevice> Device() {
    static id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    return device;
}

/// The structures of a features or properties chain the Metal backend answers, by type.
struct Answered {
    VkStructureType type;
    const char* name;
};
template <typename T>
constexpr Answered Entry(const char* name) {
    return {VkStructureType(T::structureType), name};
}
const Answered answered[] = {
    Entry<vk::PhysicalDeviceVulkan11Features>("VkPhysicalDeviceVulkan11Features"),
    Entry<vk::PhysicalDeviceVulkan12Features>("VkPhysicalDeviceVulkan12Features"),
    Entry<vk::PhysicalDeviceVulkan13Features>("VkPhysicalDeviceVulkan13Features"),
    Entry<vk::PhysicalDeviceRobustness2FeaturesEXT>("VkPhysicalDeviceRobustness2FeaturesKHR"),
    Entry<vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT>("VkPhysicalDeviceExtendedDynamicState3FeaturesEXT"),
    Entry<vk::PhysicalDevicePrimitiveTopologyListRestartFeaturesEXT>("VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT"),
    Entry<vk::PhysicalDeviceShaderAtomicFloat2FeaturesEXT>("VkPhysicalDeviceShaderAtomicFloat2FeaturesEXT"),
    Entry<vk::PhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR>("VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR"),
    Entry<vk::PhysicalDeviceImage2DViewOf3DFeaturesEXT>("VkPhysicalDeviceImage2DViewOf3DFeaturesEXT"),
    Entry<vk::PhysicalDeviceShaderClockFeaturesKHR>("VkPhysicalDeviceShaderClockFeaturesKHR"),
    Entry<vk::PhysicalDeviceComputeShaderDerivativesFeaturesKHR>("VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR"),
    Entry<vk::PhysicalDeviceShaderMixedFloatDotProductFeaturesVALVE>("VkPhysicalDeviceShaderMixedFloatDotProductFeaturesVALVE"),
    Entry<vk::PhysicalDeviceCooperativeMatrixFeaturesKHR>("VkPhysicalDeviceCooperativeMatrixFeaturesKHR"),
    Entry<vk::PhysicalDeviceShaderFloat8FeaturesEXT>("VkPhysicalDeviceShaderFloat8FeaturesEXT"),
    Entry<vk::PhysicalDeviceVulkan11Properties>("VkPhysicalDeviceVulkan11Properties"),
    Entry<vk::PhysicalDeviceVulkan12Properties>("VkPhysicalDeviceVulkan12Properties"),
    Entry<vk::PhysicalDeviceVulkan13Properties>("VkPhysicalDeviceVulkan13Properties"),
    Entry<vk::PhysicalDevicePushDescriptorPropertiesKHR>("VkPhysicalDevicePushDescriptorProperties"),
    Entry<vk::PhysicalDeviceDriverProperties>("VkPhysicalDeviceDriverProperties"),
};

/// Every structure of the chain after `head` that the Metal backend answers.
void ApplyChain(void* head) {
    for (auto* s = static_cast<VkBaseOutStructure*>(head)->pNext; s; s = s->pNext) {
        for (const Answered& a : answered) {
            if (a.type == s->sType) {
                // The profile covers the members after sType and pNext.
                ApplyProfile(a.name, s);
            }
        }
    }
}

void FillFeatures(VkPhysicalDeviceFeatures2* features) {
    ApplyProfile("VkPhysicalDeviceFeatures", &features->features);
    ApplyChain(features);
}

void FillProperties(VkPhysicalDeviceProperties2* properties) {
    auto& p = properties->properties;
    ApplyProfile("VkPhysicalDeviceProperties", &p);
    ApplyProfile("VkPhysicalDeviceLimits", &p.limits);
    ApplyProfile("VkPhysicalDeviceSparseProperties", &p.sparseProperties);
    ApplyChain(properties);
    // What depends on the device.
    id<MTLDevice> device = Device();
    std::memset(p.deviceName, 0, sizeof(p.deviceName));
    std::strncpy(p.deviceName, device.name.UTF8String, sizeof(p.deviceName) - 1);
    p.limits.maxComputeSharedMemorySize = u32(device.maxThreadgroupMemoryLength);
    p.limits.maxComputeWorkGroupInvocations = u32(device.maxThreadsPerThreadgroup.width);
    for (auto* s = static_cast<VkBaseOutStructure*>(static_cast<void*>(properties))->pNext; s; s = s->pNext) {
        if (s->sType == VkStructureType(vk::PhysicalDeviceVulkan11Properties::structureType)) {
            reinterpret_cast<VkPhysicalDeviceVulkan11Properties*>(s)->maxMemoryAllocationSize = device.maxBufferLength;
        } else if (s->sType == VkStructureType(vk::PhysicalDeviceVulkan13Properties::structureType)) {
            reinterpret_cast<VkPhysicalDeviceVulkan13Properties*>(s)->maxBufferSize = device.maxBufferLength;
        }
    }
}

void FillFormat(VkFormat format, VkFormatProperties2* properties) {
    struct Masks {
        u32 format;
        u64 linear, optimal, buffer;
    };
    static const Masks masks[] = {
#define BB_FORMAT(F, L, O, B) {F, L, O, B},
#define BB_VALUE(...)
#define BB_MEMORY_TYPE(...)
#define BB_MEMORY_HEAP(...)
#define BB_QUEUE_FAMILY(...)
#define BB_EXTENSION(...)
#include "video_core/renderer_metal/mtl_vk_caps_profile.inc"
#undef BB_VALUE
#undef BB_FORMAT
#undef BB_MEMORY_TYPE
#undef BB_MEMORY_HEAP
#undef BB_QUEUE_FAMILY
#undef BB_EXTENSION
    };
    for (const Masks& m : masks) {
        if (m.format != u32(format)) {
            continue;
        }
        properties->formatProperties = {VkFormatFeatureFlags(m.linear), VkFormatFeatureFlags(m.optimal),
                                         VkFormatFeatureFlags(m.buffer)};
        for (auto* s = static_cast<VkBaseOutStructure*>(static_cast<void*>(properties))->pNext; s; s = s->pNext) {
            if (s->sType == VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3) {
                auto* p3 = reinterpret_cast<VkFormatProperties3*>(s);
                p3->linearTilingFeatures = m.linear;
                p3->optimalTilingFeatures = m.optimal;
                p3->bufferFeatures = m.buffer;
            }
        }
    }
}

void FillMemory(VkPhysicalDeviceMemoryProperties* memory) {
    *memory = {};
#define BB_MEMORY_TYPE(F, H) memory->memoryTypes[memory->memoryTypeCount++] = {VkMemoryPropertyFlags(F), H};
#define BB_MEMORY_HEAP(S, F) memory->memoryHeaps[memory->memoryHeapCount++] = {S, VkMemoryHeapFlags(F)};
#define BB_VALUE(...)
#define BB_FORMAT(...)
#define BB_QUEUE_FAMILY(...)
#define BB_EXTENSION(...)
#include "video_core/renderer_metal/mtl_vk_caps_profile.inc"
#undef BB_VALUE
#undef BB_FORMAT
#undef BB_MEMORY_TYPE
#undef BB_MEMORY_HEAP
#undef BB_QUEUE_FAMILY
#undef BB_EXTENSION
    // One heap, the Mac's memory (as MoltenVK reports it).
    u64 bytes = 0;
    size_t size = sizeof(bytes);
    if (memory->memoryHeapCount && sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0) == 0) {
        memory->memoryHeaps[0].size = bytes;
    }
}

std::vector<VkQueueFamilyProperties> QueueFamilies() {
    std::vector<VkQueueFamilyProperties> families;
#define BB_QUEUE_FAMILY(F, C, T, W, H, D) families.push_back({VkQueueFlags(F), C, T, {W, H, D}});
#define BB_VALUE(...)
#define BB_FORMAT(...)
#define BB_MEMORY_TYPE(...)
#define BB_MEMORY_HEAP(...)
#define BB_EXTENSION(...)
#include "video_core/renderer_metal/mtl_vk_caps_profile.inc"
#undef BB_VALUE
#undef BB_FORMAT
#undef BB_MEMORY_TYPE
#undef BB_MEMORY_HEAP
#undef BB_QUEUE_FAMILY
#undef BB_EXTENSION
    return families;
}

std::vector<std::string> Extensions() {
    return {
#define BB_EXTENSION(E) E,
#define BB_VALUE(...)
#define BB_FORMAT(...)
#define BB_MEMORY_TYPE(...)
#define BB_MEMORY_HEAP(...)
#define BB_QUEUE_FAMILY(...)
#include "video_core/renderer_metal/mtl_vk_caps_profile.inc"
#undef BB_VALUE
#undef BB_FORMAT
#undef BB_MEMORY_TYPE
#undef BB_MEMORY_HEAP
#undef BB_QUEUE_FAMILY
#undef BB_EXTENSION
    };
}

// ---- The comparison ---------------------------------------------------------------------------

std::string Value(const Field& f, const u8* p) {
    char text[256];
    switch (f.kind) {
    case 'B':
    case 'U': {
        u32 v;
        std::memcpy(&v, p, 4);
        std::snprintf(text, sizeof text, "%u", v);
        return text;
    }
    case 'F': {
        float v;
        std::memcpy(&v, p, 4);
        std::snprintf(text, sizeof text, "%g", v);
        return text;
    }
    case 'Q': {
        u64 v;
        std::memcpy(&v, p, 8);
        std::snprintf(text, sizeof text, "%llu", (unsigned long long)v);
        return text;
    }
    case 'S':
        return std::string{"\""} + std::string(reinterpret_cast<const char*>(p), strnlen(reinterpret_cast<const char*>(p), f.size)) + "\"";
    default: {
        std::string hex;
        for (size_t i = 0; i < f.size && i < 32; ++i) {
            std::snprintf(text, sizeof text, "%02x", p[i]);
            hex += text;
        }
        return hex;
    }
    }
}

/// Compares one structure's members; returns how many differ.
int CompareStructure(const char* structure, const void* moltenvk, const void* metal) {
    int differences = 0;
    for (const Field& f : Fields()) {
        if (std::strcmp(f.structure, structure) != 0) {
            continue;
        }
        const u8* a = static_cast<const u8*>(moltenvk) + f.offset;
        const u8* b = static_cast<const u8*>(metal) + f.offset;
        // Text up to its terminator (drivers leave the rest of the array as it was).
        const bool differ = f.kind == 'S' ? strncmp(reinterpret_cast<const char*>(a),
                                                     reinterpret_cast<const char*>(b), f.size) != 0
                                          : std::memcmp(a, b, f.size) != 0;
        if (differ) {
            std::printf("Metal caps: %s.%s: MoltenVK %s, Metal %s\n", structure, f.name,
                        Value(f, a).c_str(), Value(f, b).c_str());
            ++differences;
        }
    }
    return differences;
}

int CompareFeatures(VkPhysicalDevice physical) {
    using Chain = vk::StructureChain<
        vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan11Features,
        vk::PhysicalDeviceVulkan12Features, vk::PhysicalDeviceVulkan13Features,
        vk::PhysicalDeviceRobustness2FeaturesEXT, vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT,
        vk::PhysicalDevicePrimitiveTopologyListRestartFeaturesEXT,
        vk::PhysicalDeviceShaderAtomicFloat2FeaturesEXT,
        vk::PhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR,
        vk::PhysicalDeviceImage2DViewOf3DFeaturesEXT, vk::PhysicalDeviceShaderClockFeaturesKHR,
        vk::PhysicalDeviceComputeShaderDerivativesFeaturesKHR,
        vk::PhysicalDeviceShaderMixedFloatDotProductFeaturesVALVE,
        vk::PhysicalDeviceCooperativeMatrixFeaturesKHR, vk::PhysicalDeviceShaderFloat8FeaturesEXT>;
    Chain moltenvk, metal;
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceFeatures2(
        physical, reinterpret_cast<VkPhysicalDeviceFeatures2*>(&moltenvk.get<vk::PhysicalDeviceFeatures2>()));
    FillFeatures(reinterpret_cast<VkPhysicalDeviceFeatures2*>(&metal.get<vk::PhysicalDeviceFeatures2>()));
    int n = CompareStructure("VkPhysicalDeviceFeatures", &moltenvk.get<vk::PhysicalDeviceFeatures2>().features,
                             &metal.get<vk::PhysicalDeviceFeatures2>().features);
#define BB_COMPARE(T, S) n += CompareStructure(#S, &moltenvk.get<T>(), &metal.get<T>());
    BB_COMPARE(vk::PhysicalDeviceVulkan11Features, VkPhysicalDeviceVulkan11Features)
    BB_COMPARE(vk::PhysicalDeviceVulkan12Features, VkPhysicalDeviceVulkan12Features)
    BB_COMPARE(vk::PhysicalDeviceVulkan13Features, VkPhysicalDeviceVulkan13Features)
    BB_COMPARE(vk::PhysicalDeviceRobustness2FeaturesEXT, VkPhysicalDeviceRobustness2FeaturesKHR)
    BB_COMPARE(vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT, VkPhysicalDeviceExtendedDynamicState3FeaturesEXT)
    BB_COMPARE(vk::PhysicalDevicePrimitiveTopologyListRestartFeaturesEXT, VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT)
    BB_COMPARE(vk::PhysicalDeviceShaderAtomicFloat2FeaturesEXT, VkPhysicalDeviceShaderAtomicFloat2FeaturesEXT)
    BB_COMPARE(vk::PhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR, VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR)
    BB_COMPARE(vk::PhysicalDeviceImage2DViewOf3DFeaturesEXT, VkPhysicalDeviceImage2DViewOf3DFeaturesEXT)
    BB_COMPARE(vk::PhysicalDeviceShaderClockFeaturesKHR, VkPhysicalDeviceShaderClockFeaturesKHR)
    BB_COMPARE(vk::PhysicalDeviceComputeShaderDerivativesFeaturesKHR, VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR)
    BB_COMPARE(vk::PhysicalDeviceShaderMixedFloatDotProductFeaturesVALVE, VkPhysicalDeviceShaderMixedFloatDotProductFeaturesVALVE)
    BB_COMPARE(vk::PhysicalDeviceCooperativeMatrixFeaturesKHR, VkPhysicalDeviceCooperativeMatrixFeaturesKHR)
    BB_COMPARE(vk::PhysicalDeviceShaderFloat8FeaturesEXT, VkPhysicalDeviceShaderFloat8FeaturesEXT)
#undef BB_COMPARE
    return n;
}

int CompareProperties(VkPhysicalDevice physical) {
    using Chain = vk::StructureChain<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceVulkan11Properties,
                                     vk::PhysicalDeviceVulkan12Properties, vk::PhysicalDeviceVulkan13Properties,
                                     vk::PhysicalDevicePushDescriptorPropertiesKHR,
                                     vk::PhysicalDeviceDriverProperties>;
    Chain moltenvk, metal;
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceProperties2(
        physical, reinterpret_cast<VkPhysicalDeviceProperties2*>(&moltenvk.get<vk::PhysicalDeviceProperties2>()));
    FillProperties(reinterpret_cast<VkPhysicalDeviceProperties2*>(&metal.get<vk::PhysicalDeviceProperties2>()));
    const auto& a = moltenvk.get<vk::PhysicalDeviceProperties2>().properties;
    const auto& b = metal.get<vk::PhysicalDeviceProperties2>().properties;
    int n = CompareStructure("VkPhysicalDeviceProperties", &a, &b);
    n += CompareStructure("VkPhysicalDeviceLimits", &a.limits, &b.limits);
    n += CompareStructure("VkPhysicalDeviceSparseProperties", &a.sparseProperties, &b.sparseProperties);
#define BB_COMPARE(T, S) n += CompareStructure(#S, &moltenvk.get<T>(), &metal.get<T>());
    BB_COMPARE(vk::PhysicalDeviceVulkan11Properties, VkPhysicalDeviceVulkan11Properties)
    BB_COMPARE(vk::PhysicalDeviceVulkan12Properties, VkPhysicalDeviceVulkan12Properties)
    BB_COMPARE(vk::PhysicalDeviceVulkan13Properties, VkPhysicalDeviceVulkan13Properties)
    BB_COMPARE(vk::PhysicalDevicePushDescriptorPropertiesKHR, VkPhysicalDevicePushDescriptorProperties)
    BB_COMPARE(vk::PhysicalDeviceDriverProperties, VkPhysicalDeviceDriverProperties)
#undef BB_COMPARE
    return n;
}

/// Every core format (1 .. VK_FORMAT_ASTC_12x12_SRGB_BLOCK): the three feature masks.
int CompareFormats(VkPhysicalDevice physical) {
    int n = 0;
    for (u32 f = 1; f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK; ++f) {
        VkFormatProperties3 a3{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3}, b3{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
        VkFormatProperties2 a{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, &a3}, b{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, &b3};
        VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceFormatProperties2(physical, VkFormat(f), &a);
        FillFormat(VkFormat(f), &b);
        const auto report = [&](const char* what, u64 x, u64 y) {
            if (x != y) {
                std::printf("Metal caps: format %u (%s) %s: MoltenVK %#llx, Metal %#llx (MoltenVK only "
                            "%#llx, Metal only %#llx)\n",
                            f, vk::to_string(vk::Format(f)).c_str(), what, (unsigned long long)x,
                            (unsigned long long)y, (unsigned long long)(x & ~y), (unsigned long long)(y & ~x));
                ++n;
            }
        };
        report("linear", a3.linearTilingFeatures, b3.linearTilingFeatures);
        report("optimal", a3.optimalTilingFeatures, b3.optimalTilingFeatures);
        report("buffer", a3.bufferFeatures, b3.bufferFeatures);
    }
    return n;
}

int CompareMemoryAndQueues(VkPhysicalDevice physical) {
    int n = 0;
    VkPhysicalDeviceMemoryProperties a{}, b{};
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceMemoryProperties(physical, &a);
    FillMemory(&b);
    if (a.memoryTypeCount != b.memoryTypeCount || a.memoryHeapCount != b.memoryHeapCount) {
        std::printf("Metal caps: memory: MoltenVK %u types in %u heaps, Metal %u in %u\n",
                    a.memoryTypeCount, a.memoryHeapCount, b.memoryTypeCount, b.memoryHeapCount);
        ++n;
    }
    for (u32 i = 0; i < a.memoryTypeCount; ++i) {
        if (i >= b.memoryTypeCount || a.memoryTypes[i].propertyFlags != b.memoryTypes[i].propertyFlags ||
            a.memoryTypes[i].heapIndex != b.memoryTypes[i].heapIndex) {
            std::printf("Metal caps: memory type %u: MoltenVK flags %#x heap %u\n", i,
                        a.memoryTypes[i].propertyFlags, a.memoryTypes[i].heapIndex);
            ++n;
        }
    }
    for (u32 i = 0; i < a.memoryHeapCount; ++i) {
        if (i >= b.memoryHeapCount || a.memoryHeaps[i].size != b.memoryHeaps[i].size ||
            a.memoryHeaps[i].flags != b.memoryHeaps[i].flags) {
            std::printf("Metal caps: memory heap %u: MoltenVK %llu bytes flags %#x\n", i,
                        (unsigned long long)a.memoryHeaps[i].size, a.memoryHeaps[i].flags);
            ++n;
        }
    }
    u32 count = 0;
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    const auto ours = QueueFamilies();
    for (u32 i = 0; i < count; ++i) {
        const bool same = i < ours.size() && std::memcmp(&families[i], &ours[i], sizeof(families[i])) == 0;
        if (!same) {
            std::printf("Metal caps: queue family %u: MoltenVK flags %#x count %u timestamp bits %u "
                        "granularity %ux%ux%u\n",
                        i, families[i].queueFlags, families[i].queueCount, families[i].timestampValidBits,
                        families[i].minImageTransferGranularity.width,
                        families[i].minImageTransferGranularity.height,
                        families[i].minImageTransferGranularity.depth);
            ++n;
        }
    }
    if (ours.size() > count) {
        std::printf("Metal caps: Metal has %zu queue families, MoltenVK %u\n", ours.size(), count);
        ++n;
    }
    return n;
}

int CompareExtensions(VkPhysicalDevice physical) {
    u32 count = 0;
    VULKAN_HPP_DEFAULT_DISPATCHER.vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> list(count);
    VULKAN_HPP_DEFAULT_DISPATCHER.vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, list.data());
    std::set<std::string> moltenvk, metal;
    for (const auto& e : list) {
        moltenvk.insert(e.extensionName);
    }
    for (const auto& e : Extensions()) {
        metal.insert(e);
    }
    std::string only_moltenvk, only_metal;
    int missing = 0, extra = 0;
    for (const auto& e : moltenvk) {
        if (!metal.contains(e)) {
            only_moltenvk += " " + e;
            ++missing;
        }
    }
    for (const auto& e : metal) {
        if (!moltenvk.contains(e)) {
            only_metal += " " + e;
            ++extra;
        }
    }
    if (missing) {
        std::printf("Metal caps: extensions only MoltenVK has (%d of %zu):%s\n", missing,
                    moltenvk.size(), only_moltenvk.c_str());
    }
    if (extra) {
        std::printf("Metal caps: extensions only Metal has (%d):%s\n", extra, only_metal.c_str());
    }
    const int n = missing + extra;
    return n;
}

} // namespace

void CompareIfAsked(void* physical_device) {
    const char* env = std::getenv("BB_METAL_CAPS");
    if (!env || std::strcmp(env, "compare") != 0 || !physical_device) {
        return;
    }
    const auto physical = static_cast<VkPhysicalDevice>(physical_device);
    @autoreleasepool {
        const int features = CompareFeatures(physical);
        const int properties = CompareProperties(physical);
        const int formats = CompareFormats(physical);
        const int memory = CompareMemoryAndQueues(physical);
        const int extensions = CompareExtensions(physical);
        std::printf("Metal caps: %d differences from MoltenVK (features %d, properties %d, formats %d, "
                    "memory and queues %d, extensions %d)\n",
                    features + properties + formats + memory + extensions, features, properties,
                    formats, memory, extensions);
    }
}

} // namespace MetalCaps
