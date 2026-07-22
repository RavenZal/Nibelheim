#pragma once

#include <DirectXMath.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace dx12
{

struct Vertex final
{
    float position[3];
    float normal[3];
    float textureCoordinates[2];
    float tangent[4];
};

static_assert(
    sizeof(Vertex) == sizeof(float) * 12,
    "Vertex must contain exactly twelve floats."
);

static_assert(
    offsetof(Vertex, position) == 0,
    "Vertex position must begin at byte offset 0."
);

static_assert(
    offsetof(Vertex, normal) == sizeof(float) * 3,
    "Vertex normal must begin at byte offset 12."
);

static_assert(
    offsetof(Vertex, textureCoordinates) == sizeof(float) * 6,
    "Vertex texture coordinates must begin at byte offset 24."
);

static_assert(
    offsetof(Vertex, tangent) == sizeof(float) * 8,
    "Vertex tangent must begin at byte offset 32."
);

struct MeshData final
{
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    DirectX::XMFLOAT4X4 nodeTransform{};
};

struct MaterialTextureSource final
{
    std::filesystem::path imagePath;
    std::vector<std::uint8_t> embeddedImageBytes;
    std::string mimeType;
    std::uint32_t textureIndex = 0;
    std::uint32_t imageIndex = 0;
    std::uint32_t textureCoordinateSet = 0;
};

struct StaticMaterialData final
{
    DirectX::XMFLOAT4 baseColorFactor{1.0F, 1.0F, 1.0F, 1.0F};
    float metallicFactor = 1.0F;
    float roughnessFactor = 1.0F;
    float normalScale = 1.0F;

    std::optional<MaterialTextureSource> baseColorTexture;
    std::optional<MaterialTextureSource> normalTexture;
    std::optional<MaterialTextureSource> metallicRoughnessTexture;
};

struct StaticModelData final
{
    MeshData mesh;
    StaticMaterialData material;
};

} // namespace dx12
