#pragma once

#include "MeshData.h"

#include <filesystem>

namespace dx12
{

// Loads the deliberately small single-mesh, single-material subset used by
// the current learning milestone. Image URIs are resolved but image bytes are
// not decoded yet. Unsupported glTF features fail with a precise error.
[[nodiscard]] StaticModelData LoadStaticGltfModel(
    const std::filesystem::path& filePath
);

} // namespace dx12
