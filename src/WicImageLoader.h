#pragma once

#include "MeshData.h"

#include <cstdint>
#include <vector>

namespace dx12
{

struct DecodedImage final
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t rowStride = 0;
    std::vector<std::uint8_t> rgba8Pixels;
};

// Decodes one external or embedded glTF image to tightly packed RGBA8.
// COM and WIC objects are local to the call and are released before return.
[[nodiscard]] DecodedImage DecodeWicImage(
    const MaterialTextureSource& source
);

} // namespace dx12
