#pragma once

#include <vulkan/vulkan.h>

// mtlvk Custom Vulkan Extensions

#define MTLVK_MAKE_TAG(ch0, ch1, ch2, ch3) \
        ((uint32_t)(ch0) | ((uint32_t)(ch1) << 8) \
        | ((uint32_t)(ch2) << 16) | ((uint32_t)(ch3) << 24))

static const VkDriverId MTLVK_DRIVER_ID_MTLVK = MTLVK_MAKE_TAG('m', 't', 'v', 'k');

static const VkStructureType MTLVK_STRUCTURE_TYPE_SHADER_MODULE_D3D12_CREATE_INFO = -12345;

typedef struct MtlvkShaderModuleD3D12CreateInfo {
    VkStructureType sType;
    const void      *pNext;
    const struct D3D12_VERSIONED_ROOT_SIGNATURE_DESC *root_signature_desc;
    VkShaderStageFlagBits stage;
} MtlvkShaderModuleD3D12CreateInfo;

static const VkStructureType MTLVK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_D3D12_CREATE_INFO = -12346;

typedef struct MtlvkPipelineVertexInputStateD3D12CreateInfo {
    VkStructureType sType;
    const void      *pNext;
    const struct D3D12_INPUT_LAYOUT_DESC *input_layout_desc;
} MtlvkPipelineVertexInputStateD3D12CreateInfo;

static const VkStructureType MTLVK_STRUCTURE_TYPE_BUFFER_VIEW_UAV_CREATE_INFO = -12347;

typedef struct MtlvkBufferViewUAVCreateInfo {
    VkStructureType sType;
    const void      *pNext;
    VkBuffer        counter_buffer;
    VkDeviceSize    counter_offset;
} MtlvkBufferViewUAVCreateInfo;

typedef struct MtlvkRootSignatureParameterConstant {
    uint32_t location_in_push_constants;
    uint32_t byte_size;
} MtlvkRootSignatureParameterConstant;

typedef struct MtlvkRootSignatureParameterDescriptor {
    uint32_t descriptor_set;
    uint32_t binding;
    uint32_t array_element;
} MtlvkRootSignatureParameterDescriptor;

typedef struct MtlvkRootSignatureParameterDescriptorTable {
    uint32_t descriptor_set;
    uint32_t binding;
    uint32_t array_element;
    uint32_t dynamic_offset_location_in_push_constants;
} MtlvkRootSignatureParameterDescriptorTable;

typedef enum MtlvkRootSignatureParameterType {
    MTLVK_ROOT_SIGNATURE_PARAMETER_CONSTANT,
    MTLVK_ROOT_SIGNATURE_PARAMETER_DESCRIPTOR,
    MTLVK_ROOT_SIGNATURE_PARAMETER_DESCRIPTOR_TABLE,
} MtlvkRootSignatureParameterType;

typedef struct MtlvkRootSignatureParameter {
    MtlvkRootSignatureParameterType type;
    union {
        MtlvkRootSignatureParameterConstant constant;
        MtlvkRootSignatureParameterDescriptor descriptor;
        MtlvkRootSignatureParameterDescriptorTable descriptor_table;
    };
} MtlvkRootSignatureParameter;

static const VkStructureType MTLVK_STRUCTURE_TYPE_PIPELINE_LAYOUT_D3D12_CREATE_INFO = 98742386;

typedef struct MtlvkPipelineLayoutD3D12CreateInfo {
    VkStructureType sType;
    const void      *pNext;
    uint32_t parameter_count;
    const struct MtlvkRootSignatureParameter *parameters;
} MtlvkPipelineLayoutD3D12CreateInfo;
