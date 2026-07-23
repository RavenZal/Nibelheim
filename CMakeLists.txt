cmake_minimum_required(VERSION 3.25)

project(Dx12Renderer
    VERSION 0.1.0
    LANGUAGES CXX
)

add_executable(DX12EnvironmentCheck
    src/GltfLoader.cpp
    src/GltfLoader.h
    src/HResult.h
    src/MeshData.h
    src/WicImageLoader.cpp
    src/WicImageLoader.h
    src/main.cpp
)

target_compile_features(DX12EnvironmentCheck
    PRIVATE
        cxx_std_20
)

target_compile_definitions(DX12EnvironmentCheck
    PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

target_link_libraries(DX12EnvironmentCheck
    PRIVATE
        d3d12
        dxgi
        dxguid
        ole32
        windowscodecs
)

if(NOT CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
    message(FATAL_ERROR
        "The selected generator did not report a Windows SDK version. "
        "Configure this project with the msvc-x64 preset."
    )
endif()

cmake_host_system_information(
    RESULT WINDOWS_KITS_ROOT
    QUERY WINDOWS_REGISTRY
        "HKLM/SOFTWARE/Microsoft/Windows Kits/Installed Roots"
    VALUE KitsRoot10
    VIEW 32
    ERROR_VARIABLE WINDOWS_KITS_ERROR
)

if(WINDOWS_KITS_ERROR OR NOT WINDOWS_KITS_ROOT)
    message(FATAL_ERROR
        "Could not locate the Windows 10/11 SDK installation root: "
        "${WINDOWS_KITS_ERROR}"
    )
endif()

file(TO_CMAKE_PATH
    "${WINDOWS_KITS_ROOT}"
    WINDOWS_KITS_ROOT
)

set(DXC_EXECUTABLE
    "${WINDOWS_KITS_ROOT}/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64/dxc.exe"
)

if(NOT EXISTS "${DXC_EXECUTABLE}")
    message(FATAL_ERROR
        "Could not find the x64 DXC executable for Windows SDK "
        "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}: "
        "${DXC_EXECUTABLE}"
    )
endif()

message(STATUS "DXC executable: ${DXC_EXECUTABLE}")

set(TRIANGLE_SHADER_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Triangles.hlsl"
)

set(TONE_MAPPING_SHADER_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/ToneMapping.hlsl"
)

if(NOT EXISTS "${TRIANGLE_SHADER_SOURCE}")
    message(FATAL_ERROR
        "Triangle shader source was not found: ${TRIANGLE_SHADER_SOURCE}"
    )
endif()

if(NOT EXISTS "${TONE_MAPPING_SHADER_SOURCE}")
    message(FATAL_ERROR
        "Tone-mapping shader source was not found: ${TONE_MAPPING_SHADER_SOURCE}"
    )
endif()

set(TRIANGLE_SHADER_OUTPUT_DIRECTORY
    "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/shaders"
)

set(TRIANGLE_VERTEX_SHADER
    "${TRIANGLE_SHADER_OUTPUT_DIRECTORY}/TrianglesVS.cso"
)

set(HDR_PIXEL_SHADER
    "${TRIANGLE_SHADER_OUTPUT_DIRECTORY}/HdrForwardPS.cso"
)

set(SHADOW_VERTEX_SHADER
    "${TRIANGLE_SHADER_OUTPUT_DIRECTORY}/ShadowVS.cso"
)

set(TONE_MAPPING_VERTEX_SHADER
    "${TRIANGLE_SHADER_OUTPUT_DIRECTORY}/ToneMappingVS.cso"
)

set(TONE_MAPPING_PIXEL_SHADER
    "${TRIANGLE_SHADER_OUTPUT_DIRECTORY}/ToneMappingPS.cso"
)

set(DXC_CONFIGURATION_ARGUMENTS
    "$<$<CONFIG:Debug>:-Zi>"
    "$<$<CONFIG:Debug>:-Qembed_debug>"
    "$<$<CONFIG:Debug>:-Od>"
    "$<$<NOT:$<CONFIG:Debug>>:-O3>"
)

add_custom_command(
    OUTPUT "${TRIANGLE_VERTEX_SHADER}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${TRIANGLE_SHADER_OUTPUT_DIRECTORY}"
    COMMAND "${DXC_EXECUTABLE}"
        -nologo
        -HV 2021
        ${DXC_CONFIGURATION_ARGUMENTS}
        -E VSMain
        -T vs_6_0
        -Fo "${TRIANGLE_VERTEX_SHADER}"
        "${TRIANGLE_SHADER_SOURCE}"
    DEPENDS "${TRIANGLE_SHADER_SOURCE}"
    COMMENT "Compiling Triangles.hlsl vertex shader"
    VERBATIM
    COMMAND_EXPAND_LISTS
)

add_custom_command(
    OUTPUT "${SHADOW_VERTEX_SHADER}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${TRIANGLE_SHADER_OUTPUT_DIRECTORY}"
    COMMAND "${DXC_EXECUTABLE}"
        -nologo
        -HV 2021
        ${DXC_CONFIGURATION_ARGUMENTS}
        -E VSShadow
        -T vs_6_0
        -Fo "${SHADOW_VERTEX_SHADER}"
        "${TRIANGLE_SHADER_SOURCE}"
    DEPENDS "${TRIANGLE_SHADER_SOURCE}"
    COMMENT "Compiling Triangles.hlsl shadow vertex shader"
    VERBATIM
    COMMAND_EXPAND_LISTS
)

add_custom_command(
    OUTPUT "${TONE_MAPPING_VERTEX_SHADER}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${TRIANGLE_SHADER_OUTPUT_DIRECTORY}"
    COMMAND "${DXC_EXECUTABLE}"
        -nologo
        -HV 2021
        ${DXC_CONFIGURATION_ARGUMENTS}
        -E VSMain
        -T vs_6_0
        -Fo "${TONE_MAPPING_VERTEX_SHADER}"
        "${TONE_MAPPING_SHADER_SOURCE}"
    DEPENDS "${TONE_MAPPING_SHADER_SOURCE}"
    COMMENT "Compiling ToneMapping.hlsl fullscreen vertex shader"
    VERBATIM
    COMMAND_EXPAND_LISTS
)

add_custom_command(
    OUTPUT "${TONE_MAPPING_PIXEL_SHADER}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${TRIANGLE_SHADER_OUTPUT_DIRECTORY}"
    COMMAND "${DXC_EXECUTABLE}"
        -nologo
        -HV 2021
        ${DXC_CONFIGURATION_ARGUMENTS}
        -E PSMain
        -T ps_6_0
        -Fo "${TONE_MAPPING_PIXEL_SHADER}"
        "${TONE_MAPPING_SHADER_SOURCE}"
    DEPENDS "${TONE_MAPPING_SHADER_SOURCE}"
    COMMENT "Compiling ToneMapping.hlsl pixel shader"
    VERBATIM
    COMMAND_EXPAND_LISTS
)

add_custom_command(
    OUTPUT "${HDR_PIXEL_SHADER}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${TRIANGLE_SHADER_OUTPUT_DIRECTORY}"
    COMMAND "${DXC_EXECUTABLE}"
        -nologo
        -HV 2021
        ${DXC_CONFIGURATION_ARGUMENTS}
        -E PSMainHdr
        -T ps_6_0
        -Fo "${HDR_PIXEL_SHADER}"
        "${TRIANGLE_SHADER_SOURCE}"
    DEPENDS "${TRIANGLE_SHADER_SOURCE}"
    COMMENT "Compiling Triangles.hlsl HDR forward pixel shader"
    VERBATIM
    COMMAND_EXPAND_LISTS
)

add_custom_target(DX12Shaders
    DEPENDS
        "${TRIANGLE_VERTEX_SHADER}"
        "${SHADOW_VERTEX_SHADER}"
        "${HDR_PIXEL_SHADER}"
        "${TONE_MAPPING_VERTEX_SHADER}"
        "${TONE_MAPPING_PIXEL_SHADER}"
)

add_dependencies(DX12EnvironmentCheck DX12Shaders)

set(LEARNING_GLTF_ASSET
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/models/learning_cube.gltf"
)

if(NOT EXISTS "${LEARNING_GLTF_ASSET}")
    message(FATAL_ERROR
        "Static glTF learning asset was not found: ${LEARNING_GLTF_ASSET}"
    )
endif()

add_custom_command(
    TARGET DX12EnvironmentCheck
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "$<TARGET_FILE_DIR:DX12EnvironmentCheck>/assets/models"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${LEARNING_GLTF_ASSET}"
        "$<TARGET_FILE_DIR:DX12EnvironmentCheck>/assets/models/learning_cube.gltf"
    COMMENT "Copying the static glTF learning asset"
    VERBATIM
)

if(MSVC)
    target_compile_options(DX12EnvironmentCheck
        PRIVATE
            /W4
            /permissive-
            /Zc:__cplusplus
    )
endif()
