cmake_minimum_required(VERSION 3.25)

project(Dx12Renderer
    VERSION 0.1.0
    LANGUAGES CXX
)

add_executable(DX12EnvironmentCheck
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
)

if(MSVC)
    target_compile_options(DX12EnvironmentCheck
        PRIVATE
            /W4
            /permissive-
            /Zc:__cplusplus
    )
endif()