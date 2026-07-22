#include "HResult.h"
#include "GltfLoader.h"
#include "WicImageLoader.h"

#include <d3d12.h>
#include <wrl/client.h>
#include <Windows.h>
#include <stdexcept>
#include <dxgi.h>
#include <dxgi1_4.h>
#include <DirectXMath.h>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>

#include <array>

#include <filesystem>
#include <string>

#include <fstream>
#include <vector>

#include <cstring>

namespace
{

struct WindowState final
{
    bool resizePending = false;
    bool isMinimized = false;
    UINT pendingWidth = 0;
    UINT pendingHeight = 0;
};

struct CameraState final
{
    DirectX::XMFLOAT3 position{0.0f, 0.0f, -3.0f};
    float yawRadians = 0.0f;
    float pitchRadians = 0.0f;
};

struct PbrConstants final
{
    DirectX::XMFLOAT4 baseColorFactor;
    DirectX::XMFLOAT3 cameraWorldPosition;
    float metallicFactor = 1.0F;
    DirectX::XMFLOAT3 directionalLightDirection;
    float roughnessFactor = 1.0F;
    DirectX::XMFLOAT3 directionalLightColor;
    float normalScale = 1.0F;
    float directionalLightIntensity = 1.0F;
    float ambientIntensity = 0.03F;
    float modelHandedness = 1.0F;
    float padding = 0.0F;
};

static_assert(
    sizeof(PbrConstants) == sizeof(float) * 20,
    "PBR root constants must contain exactly twenty 32-bit values."
);

struct MaterialTextureUpload final
{
    dx12::DecodedImage image;
    DXGI_FORMAT srvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    std::wstring resourceName;
    std::wstring uploadName;
    Microsoft::WRL::ComPtr<ID3D12Resource> texture;
    Microsoft::WRL::ComPtr<ID3D12Resource> uploadBuffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rowCount = 0;
    UINT64 rowSize = 0;
    UINT64 uploadBufferSize = 0;
};

//file
[[nodiscard]] std::filesystem::path GetExecutableDirectory()
{
    constexpr DWORD pathBufferSize = 32768;

    std::wstring executablePath(
        pathBufferSize,
        L'\0'
    );

    SetLastError(ERROR_SUCCESS);

    const DWORD pathLength = GetModuleFileNameW(
        nullptr,
        executablePath.data(),
        pathBufferSize
    );

    if (pathLength == 0)
    {
        const DWORD error = GetLastError();

        dx12::ThrowIfFailed(
            HRESULT_FROM_WIN32(error),
            "GetModuleFileNameW"
        );
    }

    if (pathLength >= pathBufferSize)
    {
        throw std::runtime_error(
            "The executable path was truncated."
        );
    }

    executablePath.resize(pathLength);

    return std::filesystem::path(
        executablePath
    ).parent_path();
}

//read binary file
[[nodiscard]] std::vector<char> ReadBinaryFile(
    const std::filesystem::path& filePath)
{
    std::ifstream file(
        filePath,
        std::ios::binary | std::ios::ate
    );

    if (!file.is_open())
    {
        throw std::runtime_error(
            "Failed to open binary file: " +
            filePath.string()
        );
    }

    const std::streampos endPosition =
        file.tellg();

    if (endPosition <= 0)
    {
        throw std::runtime_error(
            "Binary file is empty or its size could not be read: " +
            filePath.string()
        );
    }

    const auto fileSize =
        static_cast<std::size_t>(endPosition);

    std::vector<char> fileBytes(fileSize);

    file.seekg(
        0,
        std::ios::beg
    );

    if (!file.read(
            fileBytes.data(),
            static_cast<std::streamsize>(fileBytes.size())))
    {
        throw std::runtime_error(
            "Failed to read the complete binary file: " +
            filePath.string()
        );
    }

    return fileBytes;
}

struct TransformConstants final
{
    DirectX::XMFLOAT4X4 model;
    DirectX::XMFLOAT4X4 view;
    DirectX::XMFLOAT4X4 projection;
    DirectX::XMFLOAT4X4 normalMatrix;
};

static_assert(
    sizeof(TransformConstants) == sizeof(float) * 64,
    "TransformConstants must contain exactly four 4x4 float matrices."
);

constexpr UINT constantBufferAlignment =
    static_cast<UINT>(
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT
    );

[[nodiscard]] constexpr UINT AlignConstantBufferSize(
    UINT byteSize) noexcept
{
    return (
        byteSize +
        constantBufferAlignment -
        1U
    ) & ~(constantBufferAlignment - 1U);
}

constexpr UINT transformConstantBufferSize =
    AlignConstantBufferSize(
        static_cast<UINT>(
            sizeof(TransformConstants)
        )
    );

static_assert(
    transformConstantBufferSize == 256,
    "The transform constant-buffer allocation must be 256 bytes."
);

class ScopedEventHandle final
{
public:
    explicit ScopedEventHandle(HANDLE handle = nullptr) noexcept
        : handle_(handle)
    {
    }

    ~ScopedEventHandle() noexcept
    {
        if (handle_ != nullptr)
        {
            CloseHandle(handle_);
        }
    }

    ScopedEventHandle(const ScopedEventHandle&) = delete;
    ScopedEventHandle& operator=(const ScopedEventHandle&) = delete;

    [[nodiscard]] HANDLE get() const noexcept
    {
        return handle_;
    }

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return handle_ != nullptr;
    }

private:
    HANDLE handle_ = nullptr;
};
} // namespace

LRESULT CALLBACK WindowProcedure(
    HWND window,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    switch (message)
    {
    case WM_NCCREATE:
    {
        auto* creationData =
            reinterpret_cast<CREATESTRUCTW*>(lParam);

        auto* windowState =
            static_cast<WindowState*>(
                creationData->lpCreateParams
            );

        if (windowState == nullptr)
        {
            return FALSE;
        }

        SetLastError(ERROR_SUCCESS);

        const LONG_PTR previousValue =
            SetWindowLongPtrW(
                window,
                GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(windowState)
            );

        const DWORD error = GetLastError();

        if (previousValue == 0 &&
            error != ERROR_SUCCESS)
        {
            std::cerr
                << "SetWindowLongPtrW failed: "
                << dx12::FormatHResult(
                    HRESULT_FROM_WIN32(error)
                )
                << '\n';

            return FALSE;
        }

        return TRUE;
    }

    case WM_SIZE:
    {
        auto* windowState =
            reinterpret_cast<WindowState*>(
                GetWindowLongPtrW(
                    window,
                    GWLP_USERDATA
                )
            );

        if (windowState == nullptr)
        {
            return DefWindowProcW(
                window,
                message,
                wParam,
                lParam
            );
        }

        const UINT newWidth =
            static_cast<UINT>(LOWORD(lParam));

        const UINT newHeight =
            static_cast<UINT>(HIWORD(lParam));

        if (wParam == SIZE_MINIMIZED ||
            newWidth == 0 ||
            newHeight == 0)
        {
            windowState->isMinimized = true;
            windowState->resizePending = false;
            return 0;
        }

        windowState->isMinimized = false;
        windowState->pendingWidth = newWidth;
        windowState->pendingHeight = newHeight;
        windowState->resizePending = true;

        return 0;
    }

    case WM_CLOSE:
        if (DestroyWindow(window) == FALSE)
        {
            const DWORD error = GetLastError();

            const std::string errorString =
                dx12::FormatHResult(
                    HRESULT_FROM_WIN32(error)
                );

            std::cout
                << "DestroyWindow failed: "
                << error
                << errorString
                << '\n';

            PostQuitMessage(EXIT_FAILURE);
        }

        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(
            window,
            message,
            wParam,
            lParam
        );
    }
}

int main()
{
    try
    {
#if defined(_DEBUG)
        Microsoft::WRL::ComPtr<ID3D12Debug> debugController;
        dx12::ThrowIfFailed(
            D3D12GetDebugInterface(
                IID_PPV_ARGS(debugController.GetAddressOf())),
            "D3D12GetDebugInterface");
        debugController->EnableDebugLayer();
        std::cout << "Direct3D 12 Debug Layer enabled.\n";
#endif
        //WINDOW
        HINSTANCE instance = GetModuleHandleW(nullptr);        
        if (instance == nullptr)
        {
            const DWORD error = GetLastError();
            const HRESULT result = HRESULT_FROM_WIN32(error);
            dx12::ThrowIfFailed(result, "GetModuleHandleW");
        }

        constexpr wchar_t windowClassName[] =
            L"DX12RendererWindowClass";
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(WNDCLASSEXW);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = WindowProcedure;
        windowClass.hInstance = instance;
        windowClass.hCursor =
            LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); //MAKEINTRESOURCEW(32512) means IDC_ARROW
        if(windowClass.hCursor == nullptr)
        {
            const DWORD error = GetLastError();
            const HRESULT result = HRESULT_FROM_WIN32(error);
            dx12::ThrowIfFailed(result, "LoadCursorW");            
        }
        windowClass.lpszClassName = windowClassName;               
        if (RegisterClassExW(&windowClass) == 0)
        {
            const DWORD error = GetLastError();
            const HRESULT result = HRESULT_FROM_WIN32(error);
            dx12::ThrowIfFailed(result, "RegisterClassExW");
        }

        WindowState windowState{};

        HWND window =CreateWindowExW(
            0,
            windowClassName,
            L"DirectX 12 Renderer",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            1280,
            720,
            nullptr,
            nullptr,
            instance,
            &windowState);  
        
        if (window == nullptr)
        {
            const DWORD error = GetLastError();
            const HRESULT result = HRESULT_FROM_WIN32(error);
            dx12::ThrowIfFailed(result, "CreateWindowExW");
        }

        //DXGI
        UINT dxgiFactoryFlags = 0;
#if defined(_DEBUG)
        dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
        
        Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
        dx12::ThrowIfFailed(
            CreateDXGIFactory2(
                dxgiFactoryFlags, 
                IID_PPV_ARGS(&factory)),
                "CreateDXGIFactory2"
        );
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;

        for (UINT adapterIndex = 0;;adapterIndex++)
        {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> currentAdapter;
            HRESULT hr = factory->EnumAdapters1(
                adapterIndex,
                &currentAdapter
            );

            if (hr == DXGI_ERROR_NOT_FOUND)
            {
                break;
            }

            dx12::ThrowIfFailed(
                hr,
                "IDXGIFactory4::EnumAdapters1"
            );

            //no failure
            DXGI_ADAPTER_DESC1 desc{};
            dx12::ThrowIfFailed(
                currentAdapter->GetDesc1(&desc),
                "IDXGIAdapter1::GetDesc1"
            );
            
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
            {
                continue; //find next one
            }
            
            HRESULT deviceCheck = D3D12CreateDevice(
                currentAdapter.Get(),
                D3D_FEATURE_LEVEL_11_0,
                __uuidof(ID3D12Device),
                nullptr
            );

            if (SUCCEEDED(deviceCheck))
            {
                adapter = currentAdapter;
                std::wcout << L"Selected adapter: " << desc.Description << L'\n';
                break;
            }
        }
        
        if (adapter == nullptr)
        {
            throw std::runtime_error(
                "No suitable hardware adapter supporting Direct3D 12 was found."
            );
        }
                
        //Device
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        constexpr D3D_FEATURE_LEVEL minimumFeatureLevel =
            D3D_FEATURE_LEVEL_11_0;

        dx12::ThrowIfFailed(
            D3D12CreateDevice(
                adapter.Get(),
                minimumFeatureLevel,
                IID_PPV_ARGS(device.GetAddressOf())),
            "D3D12CreateDevice");

        std::cout << "Direct3D 12 device created successfully "
                     "(minimum feature level 11_0).\n";

        //Load Shader by HLSL CSO
        //Cheak Path Before Compilation
        const std::filesystem::path executableDirectory =
        GetExecutableDirectory();
        
        const std::filesystem::path vertexShaderPath =
        executableDirectory /
        L"shaders" /
        L"TrianglesVS.cso";
        
        const std::filesystem::path pixelShaderPath =
        executableDirectory /
        L"shaders" /
        L"TrianglesPS.cso";
        
        const std::vector<char> vertexShaderBytes =
            ReadBinaryFile(vertexShaderPath);

        const std::vector<char> pixelShaderBytes =
            ReadBinaryFile(pixelShaderPath);

        std::cout
            << "Vertex shader loaded: "
            << vertexShaderBytes.size()
            << " bytes.\n";

        std::cout
            << "Pixel shader loaded: "
            << pixelShaderBytes.size()
            << " bytes.\n";

        const D3D12_SHADER_BYTECODE vertexShaderBytecode{
            .pShaderBytecode = vertexShaderBytes.data(),
            .BytecodeLength = vertexShaderBytes.size()
        };

        const D3D12_SHADER_BYTECODE pixelShaderBytecode{
            .pShaderBytecode = pixelShaderBytes.data(),
            .BytecodeLength = pixelShaderBytes.size()
        };

        // Root parameter 0 keeps the per-frame transform CBV at b0. Root
        // parameter 1 exposes one contiguous three-SRV table at t0-t2. Root
        // parameter 2 supplies twenty 32-bit PBR values at b1 without a
        // separate constant-buffer allocation.
        D3D12_DESCRIPTOR_RANGE materialTextureRange{};
        materialTextureRange.RangeType =
            D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        materialTextureRange.NumDescriptors = 3;
        materialTextureRange.BaseShaderRegister = 0;
        materialTextureRange.RegisterSpace = 0;
        materialTextureRange.OffsetInDescriptorsFromTableStart =
            D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        std::array<D3D12_ROOT_PARAMETER, 3> rootParameters{};

        D3D12_ROOT_PARAMETER& transformRootParameter =
            rootParameters[0];

        transformRootParameter.ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_CBV;

        transformRootParameter.Descriptor.ShaderRegister = 0;
        transformRootParameter.Descriptor.RegisterSpace = 0;

        transformRootParameter.ShaderVisibility =
            D3D12_SHADER_VISIBILITY_VERTEX;

        D3D12_ROOT_PARAMETER& materialTextureRootParameter =
            rootParameters[1];

        materialTextureRootParameter.ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        materialTextureRootParameter.DescriptorTable.NumDescriptorRanges = 1;
        materialTextureRootParameter.DescriptorTable.pDescriptorRanges =
            &materialTextureRange;
        materialTextureRootParameter.ShaderVisibility =
            D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_PARAMETER& materialConstantsRootParameter =
            rootParameters[2];
        materialConstantsRootParameter.ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        materialConstantsRootParameter.Constants.ShaderRegister = 1;
        materialConstantsRootParameter.Constants.RegisterSpace = 0;
        materialConstantsRootParameter.Constants.Num32BitValues = 20;
        // The pixel shader consumes the Material, camera, and light values.
        // The vertex shader consumes modelHandedness so mirrored node
        // transforms preserve the tangent frame orientation.
        materialConstantsRootParameter.ShaderVisibility =
            D3D12_SHADER_VISIBILITY_ALL;

        // A static sampler is embedded in the root signature because the
        // current three-texture Material table uses one fixed sampling policy.
        // No separate sampler descriptor heap is required for it.
        D3D12_STATIC_SAMPLER_DESC materialSampler{};
        materialSampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        materialSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        materialSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        materialSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        materialSampler.MipLODBias = 0.0f;
        materialSampler.MaxAnisotropy = 1;
        materialSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        materialSampler.BorderColor =
            D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        materialSampler.MinLOD = 0.0f;
        materialSampler.MaxLOD = D3D12_FLOAT32_MAX;
        materialSampler.ShaderRegister = 0;
        materialSampler.RegisterSpace = 0;
        materialSampler.ShaderVisibility =
            D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rootSignatureDescription{};

        rootSignatureDescription.NumParameters =
            static_cast<UINT>(rootParameters.size());
        rootSignatureDescription.pParameters =
            rootParameters.data();

        rootSignatureDescription.NumStaticSamplers = 1;
        rootSignatureDescription.pStaticSamplers = &materialSampler;

        rootSignatureDescription.Flags =
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        Microsoft::WRL::ComPtr<ID3DBlob>
            serializedRootSignature;

        Microsoft::WRL::ComPtr<ID3DBlob>
            rootSignatureDiagnostics;

        const HRESULT rootSignatureSerializationResult =
            D3D12SerializeRootSignature(
                &rootSignatureDescription,
                D3D_ROOT_SIGNATURE_VERSION_1,
                serializedRootSignature.GetAddressOf(),
                rootSignatureDiagnostics.GetAddressOf());
                
        if (rootSignatureDiagnostics != nullptr &&
            rootSignatureDiagnostics->GetBufferSize() > 0)
        {
            std::cerr
                << "Root signature serialization diagnostics:\n";

            std::cerr.write(
                static_cast<const char*>(
                    rootSignatureDiagnostics->GetBufferPointer()
                ),
                static_cast<std::streamsize>(
                    rootSignatureDiagnostics->GetBufferSize()
                )
            );

            std::cerr << '\n';
        }    

        dx12::ThrowIfFailed(
            rootSignatureSerializationResult,
            "D3D12SerializeRootSignature"
        );

            //Signature
        Microsoft::WRL::ComPtr<ID3D12RootSignature>
            rootSignature;

        dx12::ThrowIfFailed(
            device->CreateRootSignature(
                0,
                serializedRootSignature->GetBufferPointer(),
                serializedRootSignature->GetBufferSize(),
                IID_PPV_ARGS(rootSignature.GetAddressOf())
            ),
            "ID3D12Device::CreateRootSignature"
        );

        dx12::ThrowIfFailed(
            rootSignature->SetName(
                L"Triangle Root Signature"
            ),
            "ID3D12RootSignature::SetName"
        );

        std::cout
        << "Triangle root signature created successfully.\n";

        // Load one deliberately constrained glTF 2.0 static mesh. The loader
        // produces the same 48-byte vertex layout already verified by the
        // previous milestone and normalizes indices to 32-bit values.
        const std::filesystem::path staticMeshPath =
            GetExecutableDirectory() /
            "assets" /
            "models" /
            "learning_cube.gltf";

        dx12::StaticModelData staticModel =
            dx12::LoadStaticGltfModel(staticMeshPath);
        const dx12::MeshData& staticMesh = staticModel.mesh;

        if (staticMesh.vertices.size() >
                std::numeric_limits<UINT>::max() / sizeof(dx12::Vertex) ||
            staticMesh.indices.size() >
                std::numeric_limits<UINT>::max() / sizeof(std::uint32_t))
        {
            throw std::runtime_error(
                "The loaded static glTF mesh is too large for a D3D12 "
                "buffer view."
            );
        }

        std::cout
            << "Static glTF mesh loaded: "
            << staticMeshPath.string()
            << ", "
            << staticMesh.vertices.size()
            << " vertices, "
            << staticMesh.indices.size()
            << " indices.\n";

        dx12::StaticMaterialData& staticMaterial =
            staticModel.material;
        std::cout
            << "Static glTF material factors: baseColor=("
            << staticMaterial.baseColorFactor.x << ", "
            << staticMaterial.baseColorFactor.y << ", "
            << staticMaterial.baseColorFactor.z << ", "
            << staticMaterial.baseColorFactor.w << "), metallic="
            << staticMaterial.metallicFactor
            << ", roughness="
            << staticMaterial.roughnessFactor
            << ", normalScale="
            << staticMaterial.normalScale
            << ".\n";

        const auto logMaterialTexture = [](
            const char* label,
            const std::optional<dx12::MaterialTextureSource>& texture)
        {
            if (!texture.has_value())
            {
                std::cout
                    << "Static glTF material "
                    << label
                    << ": not specified; the future upload milestone will "
                       "use the glTF default texture.\n";
                return;
            }

            std::cout
                << "Static glTF material "
                << label
                << ": texture index "
                << texture->textureIndex
                << ", image index "
                << texture->imageIndex
                << ", TEXCOORD_"
                << texture->textureCoordinateSet;

            if (!texture->embeddedImageBytes.empty())
            {
                std::cout
                    << ", embedded "
                    << texture->mimeType
                    << ", "
                    << texture->embeddedImageBytes.size()
                    << " encoded bytes.\n";
            }
            else
            {
                std::cout
                    << ", resolved image path "
                    << texture->imagePath.string()
                    << ".\n";
            }
        };

        logMaterialTexture(
            "Base Color",
            staticMaterial.baseColorTexture
        );
        logMaterialTexture(
            "Normal",
            staticMaterial.normalTexture
        );
        logMaterialTexture(
            "Metallic-Roughness",
            staticMaterial.metallicRoughnessTexture
        );
        std::cout << std::flush;

        const DirectX::XMFLOAT3 directionalLightDirection{
            0.4F,
            -1.0F,
            0.3F
        };
        const DirectX::XMFLOAT3 directionalLightColor{
            1.0F,
            0.95F,
            0.85F
        };
        constexpr float directionalLightIntensity = 2.5F;
        constexpr float ambientIntensity = 0.03F;

        std::cout
            << "Forward PBR directional light: direction=(0.4, -1, 0.3), "
               "color=(1, 0.95, 0.85), intensity="
            << directionalLightIntensity
            << ", ambient="
            << ambientIntensity
            << ".\n";

        // Static geometry is copied once from a CPU-visible upload resource
        // into a GPU-oriented default-heap resource. The upload resource must
        // remain alive until the GPU copy has reached its Fence value.
        D3D12_HEAP_PROPERTIES vertexDefaultHeapProperties{};

        vertexDefaultHeapProperties.Type =
            D3D12_HEAP_TYPE_DEFAULT;

        vertexDefaultHeapProperties.CPUPageProperty =
            D3D12_CPU_PAGE_PROPERTY_UNKNOWN;

        vertexDefaultHeapProperties.MemoryPoolPreference =
            D3D12_MEMORY_POOL_UNKNOWN;

        vertexDefaultHeapProperties.CreationNodeMask = 1;
        vertexDefaultHeapProperties.VisibleNodeMask = 1;

        D3D12_HEAP_PROPERTIES vertexUploadHeapProperties{};

        vertexUploadHeapProperties.Type =
            D3D12_HEAP_TYPE_UPLOAD;

        vertexUploadHeapProperties.CPUPageProperty =
            D3D12_CPU_PAGE_PROPERTY_UNKNOWN;

        vertexUploadHeapProperties.MemoryPoolPreference =
            D3D12_MEMORY_POOL_UNKNOWN;

        vertexUploadHeapProperties.CreationNodeMask = 1;
        vertexUploadHeapProperties.VisibleNodeMask = 1;

        const UINT vertexBufferSize = static_cast<UINT>(
            staticMesh.vertices.size() * sizeof(dx12::Vertex)
        );

        const UINT indexBufferSize = static_cast<UINT>(
            staticMesh.indices.size() * sizeof(std::uint32_t)
        );

        D3D12_RESOURCE_DESC vertexBufferDescription{};

        vertexBufferDescription.Dimension =
            D3D12_RESOURCE_DIMENSION_BUFFER;

        vertexBufferDescription.Alignment = 0;

        vertexBufferDescription.Width =
            vertexBufferSize;

        vertexBufferDescription.Height = 1;
        vertexBufferDescription.DepthOrArraySize = 1;
        vertexBufferDescription.MipLevels = 1;

        vertexBufferDescription.Format =
            DXGI_FORMAT_UNKNOWN;

        vertexBufferDescription.SampleDesc.Count = 1;
        vertexBufferDescription.SampleDesc.Quality = 0;

        vertexBufferDescription.Layout =
            D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        vertexBufferDescription.Flags =
            D3D12_RESOURCE_FLAG_NONE;

        D3D12_RESOURCE_DESC indexBufferDescription =
            vertexBufferDescription;

        indexBufferDescription.Width = indexBufferSize;

        Microsoft::WRL::ComPtr<ID3D12Resource>
            triangleVertexBuffer;

        Microsoft::WRL::ComPtr<ID3D12Resource>
            triangleVertexUploadBuffer;

        Microsoft::WRL::ComPtr<ID3D12Resource>
            triangleIndexBuffer;

        Microsoft::WRL::ComPtr<ID3D12Resource>
            triangleIndexUploadBuffer;

        dx12::ThrowIfFailed(
            device->CreateCommittedResource(
                &vertexDefaultHeapProperties,
                D3D12_HEAP_FLAG_NONE,
                &vertexBufferDescription,
                D3D12_RESOURCE_STATE_COMMON,
                nullptr,
                IID_PPV_ARGS(
                    triangleVertexBuffer.GetAddressOf()
                )
            ),
            "ID3D12Device::CreateCommittedResource "
            "for triangle vertex buffer"
        );

        dx12::ThrowIfFailed(
            triangleVertexBuffer->SetName(
                L"Static glTF Mesh Vertex Buffer"
            ),
            "ID3D12Resource::SetName for triangle vertex buffer"
        );

        dx12::ThrowIfFailed(
            device->CreateCommittedResource(
                &vertexUploadHeapProperties,
                D3D12_HEAP_FLAG_NONE,
                &vertexBufferDescription,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(
                    triangleVertexUploadBuffer.GetAddressOf()
                )
            ),
            "ID3D12Device::CreateCommittedResource "
            "for triangle vertex upload buffer"
        );

        dx12::ThrowIfFailed(
            triangleVertexUploadBuffer->SetName(
                L"Static glTF Mesh Vertex Upload Buffer"
            ),
            "ID3D12Resource::SetName "
            "for triangle vertex upload buffer"
        );

        dx12::ThrowIfFailed(
            device->CreateCommittedResource(
                &vertexDefaultHeapProperties,
                D3D12_HEAP_FLAG_NONE,
                &indexBufferDescription,
                D3D12_RESOURCE_STATE_COMMON,
                nullptr,
                IID_PPV_ARGS(
                    triangleIndexBuffer.GetAddressOf()
                )
            ),
            "ID3D12Device::CreateCommittedResource "
            "for triangle index buffer"
        );

        dx12::ThrowIfFailed(
            triangleIndexBuffer->SetName(
                L"Static glTF Mesh Index Buffer"
            ),
            "ID3D12Resource::SetName for triangle index buffer"
        );

        dx12::ThrowIfFailed(
            device->CreateCommittedResource(
                &vertexUploadHeapProperties,
                D3D12_HEAP_FLAG_NONE,
                &indexBufferDescription,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(
                    triangleIndexUploadBuffer.GetAddressOf()
                )
            ),
            "ID3D12Device::CreateCommittedResource "
            "for triangle index upload buffer"
        );

        dx12::ThrowIfFailed(
            triangleIndexUploadBuffer->SetName(
                L"Static glTF Mesh Index Upload Buffer"
            ),
            "ID3D12Resource::SetName "
            "for triangle index upload buffer"
        );

        const auto createSolidImage = [](
            std::array<std::uint8_t, 4> rgba) -> dx12::DecodedImage
        {
            dx12::DecodedImage image;
            image.width = 1;
            image.height = 1;
            image.rowStride = 4;
            image.rgba8Pixels.assign(rgba.begin(), rgba.end());
            return image;
        };

        const auto decodeOrDefault = [&createSolidImage](
            const std::optional<dx12::MaterialTextureSource>& source,
            std::array<std::uint8_t, 4> defaultRgba)
        {
            return source.has_value()
                ? dx12::DecodeWicImage(*source)
                : createSolidImage(defaultRgba);
        };

        constexpr std::size_t materialTextureCount = 3;
        std::array<MaterialTextureUpload, materialTextureCount>
            materialTextures{};

        materialTextures[0].image = decodeOrDefault(
            staticMaterial.baseColorTexture,
            {255, 255, 255, 255}
        );
        materialTextures[0].srvFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        materialTextures[0].resourceName = L"glTF Base Color Texture";
        materialTextures[0].uploadName = L"glTF Base Color Upload Buffer";

        materialTextures[1].image = decodeOrDefault(
            staticMaterial.normalTexture,
            {128, 128, 255, 255}
        );
        materialTextures[1].srvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        materialTextures[1].resourceName = L"glTF Normal Texture";
        materialTextures[1].uploadName = L"glTF Normal Upload Buffer";

        materialTextures[2].image = decodeOrDefault(
            staticMaterial.metallicRoughnessTexture,
            {255, 255, 255, 255}
        );
        materialTextures[2].srvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        materialTextures[2].resourceName =
            L"glTF Metallic-Roughness Texture";
        materialTextures[2].uploadName =
            L"glTF Metallic-Roughness Upload Buffer";

        const auto releaseEncodedImage = [](
            std::optional<dx12::MaterialTextureSource>& source)
        {
            if (source.has_value())
            {
                std::vector<std::uint8_t>{}.swap(
                    source->embeddedImageBytes
                );
            }
        };
        releaseEncodedImage(staticMaterial.baseColorTexture);
        releaseEncodedImage(staticMaterial.normalTexture);
        releaseEncodedImage(staticMaterial.metallicRoughnessTexture);

        for (MaterialTextureUpload& materialTexture : materialTextures)
        {
            const dx12::DecodedImage& image = materialTexture.image;
            const UINT64 expectedRowSize =
                static_cast<UINT64>(image.width) * 4U;
            if (image.rowStride != expectedRowSize ||
                image.rgba8Pixels.size() !=
                    static_cast<std::size_t>(image.rowStride) * image.height)
            {
                throw std::runtime_error(
                    "Decoded material image is not tightly packed RGBA8."
                );
            }

            D3D12_RESOURCE_DESC textureDescription{};
            textureDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            textureDescription.Alignment = 0;
            textureDescription.Width = image.width;
            textureDescription.Height = image.height;
            textureDescription.DepthOrArraySize = 1;
            textureDescription.MipLevels = 1;
            textureDescription.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
            textureDescription.SampleDesc.Count = 1;
            textureDescription.SampleDesc.Quality = 0;
            textureDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            textureDescription.Flags = D3D12_RESOURCE_FLAG_NONE;

            dx12::ThrowIfFailed(
                device->CreateCommittedResource(
                    &vertexDefaultHeapProperties,
                    D3D12_HEAP_FLAG_NONE,
                    &textureDescription,
                    D3D12_RESOURCE_STATE_COPY_DEST,
                    nullptr,
                    IID_PPV_ARGS(materialTexture.texture.GetAddressOf())
                ),
                "CreateCommittedResource for glTF material texture"
            );
            dx12::ThrowIfFailed(
                materialTexture.texture->SetName(
                    materialTexture.resourceName.c_str()
                ),
                "SetName for glTF material texture"
            );

            device->GetCopyableFootprints(
                &textureDescription,
                0,
                1,
                0,
                &materialTexture.footprint,
                &materialTexture.rowCount,
                &materialTexture.rowSize,
                &materialTexture.uploadBufferSize
            );
            if (materialTexture.rowCount != image.height ||
                materialTexture.rowSize != expectedRowSize)
            {
                throw std::runtime_error(
                    "Unexpected copyable footprint for glTF material texture."
                );
            }

            D3D12_RESOURCE_DESC uploadDescription =
                vertexBufferDescription;
            uploadDescription.Width = materialTexture.uploadBufferSize;
            dx12::ThrowIfFailed(
                device->CreateCommittedResource(
                    &vertexUploadHeapProperties,
                    D3D12_HEAP_FLAG_NONE,
                    &uploadDescription,
                    D3D12_RESOURCE_STATE_GENERIC_READ,
                    nullptr,
                    IID_PPV_ARGS(materialTexture.uploadBuffer.GetAddressOf())
                ),
                "CreateCommittedResource for glTF material upload buffer"
            );
            dx12::ThrowIfFailed(
                materialTexture.uploadBuffer->SetName(
                    materialTexture.uploadName.c_str()
                ),
                "SetName for glTF material upload buffer"
            );
        }

        D3D12_DESCRIPTOR_HEAP_DESC materialSrvHeapDescription{};
        materialSrvHeapDescription.Type =
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        materialSrvHeapDescription.NumDescriptors =
            static_cast<UINT>(materialTextures.size());
        materialSrvHeapDescription.Flags =
            D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> materialSrvHeap;
        dx12::ThrowIfFailed(
            device->CreateDescriptorHeap(
                &materialSrvHeapDescription,
                IID_PPV_ARGS(materialSrvHeap.GetAddressOf())
            ),
            "CreateDescriptorHeap for glTF material SRVs"
        );
        dx12::ThrowIfFailed(
            materialSrvHeap->SetName(L"glTF Material SRV Heap"),
            "SetName for glTF material SRV heap"
        );

        const UINT materialSrvDescriptorSize =
            device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV
            );
        D3D12_CPU_DESCRIPTOR_HANDLE materialSrvHandle =
            materialSrvHeap->GetCPUDescriptorHandleForHeapStart();
        for (const MaterialTextureUpload& materialTexture : materialTextures)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDescription{};
            srvDescription.Format = materialTexture.srvFormat;
            srvDescription.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDescription.Shader4ComponentMapping =
                D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDescription.Texture2D.MostDetailedMip = 0;
            srvDescription.Texture2D.MipLevels = 1;
            srvDescription.Texture2D.PlaneSlice = 0;
            srvDescription.Texture2D.ResourceMinLODClamp = 0.0F;
            device->CreateShaderResourceView(
                materialTexture.texture.Get(),
                &srvDescription,
                materialSrvHandle
            );
            materialSrvHandle.ptr += materialSrvDescriptorSize;
        }

        // Copy the CPU vertex array into the temporary upload resource. The
        // GPU copy into triangleVertexBuffer is recorded after the main
        // graphics command list is created.
        D3D12_RANGE noCpuReads{
            .Begin = 0,
            .End = 0
        };

        void* mappedVertexData = nullptr;
        dx12::ThrowIfFailed(
            triangleVertexUploadBuffer->Map(
                0,
                &noCpuReads,
                &mappedVertexData
            ),
            "ID3D12Resource::Map for triangle vertex upload buffer"
        );

        if (mappedVertexData == nullptr)
        {
            throw std::runtime_error(
                "Triangle vertex upload buffer mapping returned "
                "a null pointer."
            );
        }

        std::memcpy(
            mappedVertexData,
            staticMesh.vertices.data(),
            vertexBufferSize
        );

        D3D12_RANGE writtenRange{
            .Begin = 0,
            .End = vertexBufferSize
        };

        triangleVertexUploadBuffer->Unmap(
            0,
            &writtenRange
        );

        void* mappedIndexData = nullptr;
        dx12::ThrowIfFailed(
            triangleIndexUploadBuffer->Map(
                0,
                &noCpuReads,
                &mappedIndexData
            ),
            "ID3D12Resource::Map for triangle index upload buffer"
        );

        if (mappedIndexData == nullptr)
        {
            throw std::runtime_error(
                "Triangle index upload buffer mapping returned "
                "a null pointer."
            );
        }

        std::memcpy(
            mappedIndexData,
            staticMesh.indices.data(),
            indexBufferSize
        );

        D3D12_RANGE indexWrittenRange{
            .Begin = 0,
            .End = indexBufferSize
        };

        triangleIndexUploadBuffer->Unmap(
            0,
            &indexWrittenRange
        );

        // Each decoded image is tightly packed, while each D3D12 upload row
        // uses the device-provided aligned RowPitch.
        for (MaterialTextureUpload& materialTexture : materialTextures)
        {
            void* mappedTextureData = nullptr;
            dx12::ThrowIfFailed(
                materialTexture.uploadBuffer->Map(
                    0,
                    &noCpuReads,
                    &mappedTextureData
                ),
                "Map glTF material texture upload buffer"
            );
            if (mappedTextureData == nullptr)
            {
                throw std::runtime_error(
                    "Material texture upload mapping returned null."
                );
            }

            auto* const destinationBytes =
                static_cast<std::uint8_t*>(mappedTextureData) +
                materialTexture.footprint.Offset;
            const dx12::DecodedImage& image = materialTexture.image;
            for (UINT row = 0; row < materialTexture.rowCount; ++row)
            {
                std::memcpy(
                    destinationBytes +
                        static_cast<std::size_t>(row) *
                        materialTexture.footprint.Footprint.RowPitch,
                    image.rgba8Pixels.data() +
                        static_cast<std::size_t>(row) * image.rowStride,
                    image.rowStride
                );
            }

            const SIZE_T writtenEnd =
                static_cast<SIZE_T>(materialTexture.footprint.Offset) +
                static_cast<SIZE_T>(materialTexture.rowCount - 1U) *
                    materialTexture.footprint.Footprint.RowPitch +
                image.rowStride;
            const D3D12_RANGE writtenTextureRange{
                .Begin = static_cast<SIZE_T>(materialTexture.footprint.Offset),
                .End = writtenEnd
            };
            materialTexture.uploadBuffer->Unmap(0, &writtenTextureRange);

            std::cout
                << "Decoded glTF material texture: "
                << image.width
                << " x "
                << image.height
                << ", RGBA8 source row "
                << image.rowStride
                << " bytes, D3D12 upload RowPitch "
                << materialTexture.footprint.Footprint.RowPitch
                << " bytes.\n";
        }
        
            //explain buffer
        D3D12_VERTEX_BUFFER_VIEW
        triangleVertexBufferView{};

        triangleVertexBufferView.BufferLocation =
            triangleVertexBuffer->GetGPUVirtualAddress();

        triangleVertexBufferView.SizeInBytes =
            vertexBufferSize;

        triangleVertexBufferView.StrideInBytes =
            static_cast<UINT>(sizeof(dx12::Vertex));

        D3D12_INDEX_BUFFER_VIEW triangleIndexBufferView{};

        triangleIndexBufferView.BufferLocation =
            triangleIndexBuffer->GetGPUVirtualAddress();

        triangleIndexBufferView.SizeInBytes =
            indexBufferSize;

        triangleIndexBufferView.Format =
            DXGI_FORMAT_R32_UINT;

        std::cout
            << "Static glTF default and upload index buffers created: "
            << staticMesh.indices.size()
            << " indices, "
            << indexBufferSize
            << " bytes; GPU upload pending.\n";

        std::cout
            << "Static glTF default and upload vertex buffers created: "
            << staticMesh.vertices.size()
            << " vertices, "
            << vertexBufferSize
            << " bytes; GPU upload pending.\n";

        constexpr std::array<
            D3D12_INPUT_ELEMENT_DESC,
            4
        > triangleInputElements{{
            {
                "POSITION",
                0,
                DXGI_FORMAT_R32G32B32_FLOAT,
                0,
                static_cast<UINT>(offsetof(dx12::Vertex, position)),
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
                0
            },
            {
                "NORMAL",
                0,
                DXGI_FORMAT_R32G32B32_FLOAT,
                0,
                static_cast<UINT>(offsetof(dx12::Vertex, normal)),
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
                0
            },
            {
                "TEXCOORD",
                0,
                DXGI_FORMAT_R32G32_FLOAT,
                0,
                static_cast<UINT>(
                    offsetof(dx12::Vertex, textureCoordinates)
                ),
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
                0
            },
            {
                "TANGENT",
                0,
                DXGI_FORMAT_R32G32B32A32_FLOAT,
                0,
                static_cast<UINT>(offsetof(dx12::Vertex, tangent)),
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
                0
            }
        }};

        D3D12_BLEND_DESC blendDescription{};

        blendDescription.AlphaToCoverageEnable = FALSE;
        blendDescription.IndependentBlendEnable = FALSE;

        D3D12_RENDER_TARGET_BLEND_DESC&
            renderTargetBlend =
                blendDescription.RenderTarget[0];

        renderTargetBlend.BlendEnable = FALSE;
        renderTargetBlend.LogicOpEnable = FALSE;

        renderTargetBlend.SrcBlend = D3D12_BLEND_ONE;
        renderTargetBlend.DestBlend = D3D12_BLEND_ZERO;
        renderTargetBlend.BlendOp = D3D12_BLEND_OP_ADD;

        renderTargetBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
        renderTargetBlend.DestBlendAlpha = D3D12_BLEND_ZERO;
        renderTargetBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;

        renderTargetBlend.LogicOp = D3D12_LOGIC_OP_NOOP;

        renderTargetBlend.RenderTargetWriteMask =
            D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_RASTERIZER_DESC rasterizerDescription{};

        rasterizerDescription.FillMode =
            D3D12_FILL_MODE_SOLID;

        rasterizerDescription.CullMode =
            D3D12_CULL_MODE_BACK;

        rasterizerDescription.FrontCounterClockwise =
            FALSE;

        rasterizerDescription.DepthBias =
            D3D12_DEFAULT_DEPTH_BIAS;

        rasterizerDescription.DepthBiasClamp =
            D3D12_DEFAULT_DEPTH_BIAS_CLAMP;

        rasterizerDescription.SlopeScaledDepthBias =
            D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;

        rasterizerDescription.DepthClipEnable = TRUE;
        rasterizerDescription.MultisampleEnable = FALSE;
        rasterizerDescription.AntialiasedLineEnable = FALSE;
        rasterizerDescription.ForcedSampleCount = 0;

        rasterizerDescription.ConservativeRaster =
            D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

        D3D12_DEPTH_STENCIL_DESC
            depthStencilDescription{};

        depthStencilDescription.DepthEnable = TRUE;

        depthStencilDescription.DepthWriteMask =
            D3D12_DEPTH_WRITE_MASK_ALL;

        depthStencilDescription.DepthFunc =
            D3D12_COMPARISON_FUNC_LESS;

        depthStencilDescription.StencilEnable = FALSE;

        depthStencilDescription.StencilReadMask =
            D3D12_DEFAULT_STENCIL_READ_MASK;

        depthStencilDescription.StencilWriteMask =
            D3D12_DEFAULT_STENCIL_WRITE_MASK;

        const D3D12_DEPTH_STENCILOP_DESC
            disabledStencilOperations{
                D3D12_STENCIL_OP_KEEP,
                D3D12_STENCIL_OP_KEEP,
                D3D12_STENCIL_OP_KEEP,
                D3D12_COMPARISON_FUNC_ALWAYS
            };

        depthStencilDescription.FrontFace =
            disabledStencilOperations;

        depthStencilDescription.BackFace =
            disabledStencilOperations;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC
            pipelineDescription{};
        pipelineDescription.pRootSignature =
            rootSignature.Get();
        pipelineDescription.VS =
            vertexShaderBytecode;
        pipelineDescription.PS =
            pixelShaderBytecode;

        pipelineDescription.BlendState =
            blendDescription;

        pipelineDescription.RasterizerState =
            rasterizerDescription;

        pipelineDescription.DepthStencilState =
            depthStencilDescription;

        pipelineDescription.SampleMask = UINT_MAX;

        pipelineDescription.InputLayout = {
            triangleInputElements.data(),
            static_cast<UINT>(
                triangleInputElements.size()
            )
        };        

        pipelineDescription.PrimitiveTopologyType =
            D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

        pipelineDescription.NumRenderTargets = 1;

        pipelineDescription.RTVFormats[0] =
            DXGI_FORMAT_R8G8B8A8_UNORM;

        pipelineDescription.DSVFormat =
            DXGI_FORMAT_D32_FLOAT;

        pipelineDescription.SampleDesc.Count = 1;
        pipelineDescription.SampleDesc.Quality = 0;

        pipelineDescription.IBStripCutValue =
            D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;

        pipelineDescription.NodeMask = 0;

        pipelineDescription.CachedPSO.pCachedBlob = nullptr;
        pipelineDescription.CachedPSO.CachedBlobSizeInBytes = 0;

        pipelineDescription.Flags =
            D3D12_PIPELINE_STATE_FLAG_NONE;

        Microsoft::WRL::ComPtr<ID3D12PipelineState>
            trianglePipelineState;

        dx12::ThrowIfFailed(
            device->CreateGraphicsPipelineState(
                &pipelineDescription,
                IID_PPV_ARGS(
                    trianglePipelineState.GetAddressOf()
                )
            ),
            "ID3D12Device::CreateGraphicsPipelineState"
        );

        dx12::ThrowIfFailed(
            trianglePipelineState->SetName(
                L"Triangle Graphics Pipeline State"
            ),
            "ID3D12PipelineState::SetName"
        );

        std::cout
            << "Triangle graphics pipeline state "
            "created successfully.\n";




        //Device Queue of Command
        D3D12_COMMAND_QUEUE_DESC commandQueueDescription{};
        commandQueueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        commandQueueDescription.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        commandQueueDescription.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        commandQueueDescription.NodeMask = 0;
        
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> CommandQueue;
        dx12::ThrowIfFailed(
            device->CreateCommandQueue(
                &commandQueueDescription,
                IID_PPV_ARGS(CommandQueue.GetAddressOf())
            ),"CreateCommandQueue"
        );

        dx12::ThrowIfFailed(
            CommandQueue->SetName(L"Main Graphics Command Queue"),
            "ID3D12CommandQueue::SetName"
        );
        std::cout << "ID3D12CommandQueue Create Success" << "\n";

        //swap Chain
        RECT clientRect;
        if(!GetClientRect(window, &clientRect))
        {
            const DWORD error = GetLastError();

            dx12::ThrowIfFailed(
                HRESULT_FROM_WIN32(error),
                "GetClientRect"
            );
        }
        
        int Width = clientRect.right - clientRect.left;
        int Height = clientRect.bottom - clientRect.top;

        if(Width <= 0 || Height <=0)
        {
            throw std::runtime_error(
                "The window client area must have a positive width and height."
            );  
        }
        
        UINT swapChainWidth =
            static_cast<UINT>(Width);

        UINT swapChainHeight =
            static_cast<UINT>(Height);

        constexpr UINT swapChainBufferCount = 2;

        DXGI_SWAP_CHAIN_DESC1 chainDesc1{};
        chainDesc1.Width = swapChainWidth;
        chainDesc1.Height = swapChainHeight;
        chainDesc1.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        chainDesc1.Stereo = FALSE;
        chainDesc1.SampleDesc.Count = 1;
        chainDesc1.SampleDesc.Quality = 0;
        chainDesc1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        chainDesc1.BufferCount = swapChainBufferCount;
        chainDesc1.Scaling = DXGI_SCALING_STRETCH;
        chainDesc1.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        chainDesc1.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        chainDesc1.Flags = 0;
        
        Microsoft::WRL::ComPtr<IDXGISwapChain1> _IDXGISwapChain1;
        dx12::ThrowIfFailed(
            factory->CreateSwapChainForHwnd(
                CommandQueue.Get(),
                window,
                &chainDesc1,
                nullptr,
                nullptr,
                _IDXGISwapChain1.GetAddressOf()
            ),
            "CreateSwapChainForHwnd"
        );       
        
        Microsoft::WRL::ComPtr<IDXGISwapChain3> _IDXGISwapChain3;
        dx12::ThrowIfFailed(
            _IDXGISwapChain1.As(&_IDXGISwapChain3),
            "Find IDXGISwapChain3"
        );
        std::cout << "IDXGISwapChain3 Create Success" << "\n";
        
        //RTV
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> desHeap;
        
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heapDesc.NumDescriptors = swapChainBufferCount;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        heapDesc.NodeMask = 0;
        dx12::ThrowIfFailed(
            device->CreateDescriptorHeap(
                &heapDesc,
                IID_PPV_ARGS(desHeap.GetAddressOf())
            ),
            "CreateDescriptorHeap"
        );
        
        dx12::ThrowIfFailed(
            desHeap->SetName(L"RTV Heap"),
            "desHeap::SetName"
        );

        const UINT rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        
        const D3D12_CPU_DESCRIPTOR_HANDLE rtvHeapStart =
            desHeap->GetCPUDescriptorHandleForHeapStart();
        
        D3D12_CPU_DESCRIPTOR_HANDLE rtvCreationHandle =
            rtvHeapStart;
        
        std::array<
            Microsoft::WRL::ComPtr<ID3D12Resource>,
            swapChainBufferCount
        > backBuffers;

        constexpr const wchar_t* backBufferNames[swapChainBufferCount] = {
            L"Swap Chain Back Buffer 0",
            L"Swap Chain Back Buffer 1"
        };

        for (UINT bufferIndex = 0;
            bufferIndex < swapChainBufferCount;
            ++bufferIndex)
        {
            dx12::ThrowIfFailed(
                _IDXGISwapChain3->GetBuffer(
                    bufferIndex,
                    IID_PPV_ARGS(
                        backBuffers[bufferIndex].ReleaseAndGetAddressOf()
                    )
                ),
                "IDXGISwapChain3::GetBuffer"
            );

            dx12::ThrowIfFailed(
                backBuffers[bufferIndex]->SetName(
                    backBufferNames[bufferIndex]
                ),
                "ID3D12Resource::SetName"
            );

            device->CreateRenderTargetView(
                backBuffers[bufferIndex].Get(),
                nullptr,
                rtvCreationHandle
            );

            rtvCreationHandle.ptr +=
                static_cast<SIZE_T>(rtvDescriptorSize);
        }
        std::cout << "RTV Create Success" << "\n";

        // One CPU-only DSV descriptor is sufficient because this renderer uses
        // one depth texture for the serial work submitted to the direct queue.
        D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDescription{};
        dsvHeapDescription.Type =
            D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsvHeapDescription.NumDescriptors = 1;
        dsvHeapDescription.Flags =
            D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        dsvHeapDescription.NodeMask = 0;

        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>
            dsvHeap;

        dx12::ThrowIfFailed(
            device->CreateDescriptorHeap(
                &dsvHeapDescription,
                IID_PPV_ARGS(dsvHeap.GetAddressOf())
            ),
            "ID3D12Device::CreateDescriptorHeap for DSV"
        );

        dx12::ThrowIfFailed(
            dsvHeap->SetName(L"Main DSV Heap"),
            "ID3D12DescriptorHeap::SetName for DSV heap"
        );

        const D3D12_CPU_DESCRIPTOR_HANDLE depthStencilView =
            dsvHeap->GetCPUDescriptorHandleForHeapStart();

        auto createDepthBuffer =
            [&](UINT width, UINT height)
            {
                if (width == 0 || height == 0)
                {
                    throw std::runtime_error(
                        "Depth-buffer dimensions must be positive."
                    );
                }

                D3D12_HEAP_PROPERTIES depthHeapProperties{};
                depthHeapProperties.Type =
                    D3D12_HEAP_TYPE_DEFAULT;
                depthHeapProperties.CPUPageProperty =
                    D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
                depthHeapProperties.MemoryPoolPreference =
                    D3D12_MEMORY_POOL_UNKNOWN;
                depthHeapProperties.CreationNodeMask = 1;
                depthHeapProperties.VisibleNodeMask = 1;

                D3D12_RESOURCE_DESC depthBufferDescription{};
                depthBufferDescription.Dimension =
                    D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                depthBufferDescription.Alignment = 0;
                depthBufferDescription.Width = width;
                depthBufferDescription.Height = height;
                depthBufferDescription.DepthOrArraySize = 1;
                depthBufferDescription.MipLevels = 1;
                depthBufferDescription.Format =
                    DXGI_FORMAT_D32_FLOAT;
                depthBufferDescription.SampleDesc.Count = 1;
                depthBufferDescription.SampleDesc.Quality = 0;
                depthBufferDescription.Layout =
                    D3D12_TEXTURE_LAYOUT_UNKNOWN;
                depthBufferDescription.Flags =
                    D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

                D3D12_CLEAR_VALUE optimizedClearValue{};
                optimizedClearValue.Format =
                    DXGI_FORMAT_D32_FLOAT;
                optimizedClearValue.DepthStencil.Depth = 1.0f;
                optimizedClearValue.DepthStencil.Stencil = 0;

                Microsoft::WRL::ComPtr<ID3D12Resource>
                    createdDepthBuffer;

                dx12::ThrowIfFailed(
                    device->CreateCommittedResource(
                        &depthHeapProperties,
                        D3D12_HEAP_FLAG_NONE,
                        &depthBufferDescription,
                        D3D12_RESOURCE_STATE_DEPTH_WRITE,
                        &optimizedClearValue,
                        IID_PPV_ARGS(
                            createdDepthBuffer.GetAddressOf()
                        )
                    ),
                    "ID3D12Device::CreateCommittedResource "
                    "for depth buffer"
                );

                dx12::ThrowIfFailed(
                    createdDepthBuffer->SetName(L"Main Depth Buffer"),
                    "ID3D12Resource::SetName for depth buffer"
                );

                D3D12_DEPTH_STENCIL_VIEW_DESC
                    depthStencilViewDescription{};
                depthStencilViewDescription.Format =
                    DXGI_FORMAT_D32_FLOAT;
                depthStencilViewDescription.ViewDimension =
                    D3D12_DSV_DIMENSION_TEXTURE2D;
                depthStencilViewDescription.Flags =
                    D3D12_DSV_FLAG_NONE;
                depthStencilViewDescription.Texture2D.MipSlice = 0;

                // CreateDepthStencilView returns void. The Debug Layer checks
                // that the resource, format, and descriptor agree.
                device->CreateDepthStencilView(
                    createdDepthBuffer.Get(),
                    &depthStencilViewDescription,
                    depthStencilView
                );

                return createdDepthBuffer;
            };

        Microsoft::WRL::ComPtr<ID3D12Resource> depthBuffer =
            createDepthBuffer(
                swapChainWidth,
                swapChainHeight
            );

        std::cout
            << "Depth buffer and DSV created: "
            << swapChainWidth
            << " x "
            << swapChainHeight
            << ", DXGI_FORMAT_D32_FLOAT.\n";

        // One transform Constant Buffer belongs to each swap-chain/back-buffer
        // slot. The CPU updates a slot only after that slot's Fence value has
        // completed, so the GPU never observes the CPU overwriting in-flight data.
        std::array<
            Microsoft::WRL::ComPtr<ID3D12Resource>,
            swapChainBufferCount
        > transformConstantBuffers;

        std::array<
            std::byte*,
            swapChainBufferCount
        > mappedTransformConstantData{};

        constexpr const wchar_t*
            transformConstantBufferNames[swapChainBufferCount] = {
                L"Transform Constant Buffer 0",
                L"Transform Constant Buffer 1"
            };

        D3D12_HEAP_PROPERTIES
            transformConstantBufferHeapProperties{};

        transformConstantBufferHeapProperties.Type =
            D3D12_HEAP_TYPE_UPLOAD;

        transformConstantBufferHeapProperties.CPUPageProperty =
            D3D12_CPU_PAGE_PROPERTY_UNKNOWN;

        transformConstantBufferHeapProperties.MemoryPoolPreference =
            D3D12_MEMORY_POOL_UNKNOWN;

        transformConstantBufferHeapProperties.CreationNodeMask = 1;
        transformConstantBufferHeapProperties.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC
            transformConstantBufferDescription{};

        transformConstantBufferDescription.Dimension =
            D3D12_RESOURCE_DIMENSION_BUFFER;

        transformConstantBufferDescription.Alignment = 0;

        transformConstantBufferDescription.Width =
            transformConstantBufferSize;

        transformConstantBufferDescription.Height = 1;
        transformConstantBufferDescription.DepthOrArraySize = 1;
        transformConstantBufferDescription.MipLevels = 1;

        transformConstantBufferDescription.Format =
            DXGI_FORMAT_UNKNOWN;

        transformConstantBufferDescription.SampleDesc.Count = 1;
        transformConstantBufferDescription.SampleDesc.Quality = 0;

        transformConstantBufferDescription.Layout =
            D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        transformConstantBufferDescription.Flags =
            D3D12_RESOURCE_FLAG_NONE;

        // {0, 0} tells D3D12 that the CPU does not intend to read any
        // bytes from the mapped upload resource.
        const D3D12_RANGE transformConstantBufferNoCpuReads{
            .Begin = 0,
            .End = 0
        };

        for (UINT bufferIndex = 0;
            bufferIndex < swapChainBufferCount;
            ++bufferIndex)
        {
            dx12::ThrowIfFailed(
                device->CreateCommittedResource(
                    &transformConstantBufferHeapProperties,
                    D3D12_HEAP_FLAG_NONE,
                    &transformConstantBufferDescription,
                    D3D12_RESOURCE_STATE_GENERIC_READ,
                    nullptr,
                    IID_PPV_ARGS(
                        transformConstantBuffers[bufferIndex]
                            .GetAddressOf()
                    )
                ),
                "ID3D12Device::CreateCommittedResource "
                "for transform constant buffer"
            );

            dx12::ThrowIfFailed(
                transformConstantBuffers[bufferIndex]->SetName(
                    transformConstantBufferNames[bufferIndex]
                ),
                "ID3D12Resource::SetName "
                "for transform constant buffer"
            );

            void* mappedData = nullptr;

            dx12::ThrowIfFailed(
                transformConstantBuffers[bufferIndex]->Map(
                    0,
                    &transformConstantBufferNoCpuReads,
                    &mappedData
                ),
                "ID3D12Resource::Map "
                "for transform constant buffer"
            );

            if (mappedData == nullptr)
            {
                throw std::runtime_error(
                    "Transform constant-buffer mapping "
                    "returned a null pointer."
                );
            }

            mappedTransformConstantData[bufferIndex] =
                static_cast<std::byte*>(mappedData);
        }

        std::cout
            << "Created and persistently mapped "
            << swapChainBufferCount
            << " transform constant buffers, "
            << transformConstantBufferSize
            << " bytes each.\n";        

        //Command Allocator
        // One command allocator for each swap-chain back buffer.
        std::array<
            Microsoft::WRL::ComPtr<ID3D12CommandAllocator>,
            swapChainBufferCount
        > commandAllocators;

        constexpr const wchar_t*
            commandAllocatorNames[swapChainBufferCount] = {
                L"Frame Command Allocator 0",
                L"Frame Command Allocator 1"
            };

        for (UINT frameIndex = 0;
            frameIndex < swapChainBufferCount;
            ++frameIndex)
        {
            dx12::ThrowIfFailed(
                device->CreateCommandAllocator(
                    D3D12_COMMAND_LIST_TYPE_DIRECT,
                    IID_PPV_ARGS(
                        commandAllocators[frameIndex].GetAddressOf()
                    )
                ),
                "ID3D12Device::CreateCommandAllocator"
            );

            dx12::ThrowIfFailed(
                commandAllocators[frameIndex]->SetName(
                    commandAllocatorNames[frameIndex]
                ),
                "ID3D12CommandAllocator::SetName"
            );
        }

        std::cout << "Frame command allocators created successfully.\n";

        //Command List
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> CommandList;
        dx12::ThrowIfFailed(
            device->CreateCommandList(
                0,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                commandAllocators[0].Get(),
                nullptr,
                IID_PPV_ARGS(CommandList.GetAddressOf())
            ),"CreateCommandList"
        );

        dx12::ThrowIfFailed(
            CommandList->SetName(L"Main Graphics Command List"),
            "ID3D12GraphicsCommandList::SetName"
        );

        // The command list is created in the recording state. Copy the static
        // vertex bytes from the temporary upload heap into the default heap,
        // then make the destination legal for input-assembler vertex reads.
        // The input assembler cannot read the default-heap index buffer until
        // the copy has completed and its state is changed to INDEX_BUFFER.
        CommandList->CopyBufferRegion(
            triangleVertexBuffer.Get(),
            0,
            triangleVertexUploadBuffer.Get(),
            0,
            vertexBufferSize
        );

        D3D12_RESOURCE_BARRIER vertexBufferReadyBarrier{};
        vertexBufferReadyBarrier.Type =
            D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        vertexBufferReadyBarrier.Flags =
            D3D12_RESOURCE_BARRIER_FLAG_NONE;
        vertexBufferReadyBarrier.Transition.pResource =
            triangleVertexBuffer.Get();
        vertexBufferReadyBarrier.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        vertexBufferReadyBarrier.Transition.StateBefore =
            D3D12_RESOURCE_STATE_COPY_DEST;
        vertexBufferReadyBarrier.Transition.StateAfter =
            D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;

        CommandList->ResourceBarrier(
            1,
            &vertexBufferReadyBarrier
        );

        CommandList->CopyBufferRegion(
            triangleIndexBuffer.Get(),
            0,
            triangleIndexUploadBuffer.Get(),
            0,
            indexBufferSize
        );

        D3D12_RESOURCE_BARRIER indexBufferReadyBarrier{};
        indexBufferReadyBarrier.Type =
            D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        indexBufferReadyBarrier.Flags =
            D3D12_RESOURCE_BARRIER_FLAG_NONE;
        indexBufferReadyBarrier.Transition.pResource =
            triangleIndexBuffer.Get();
        indexBufferReadyBarrier.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        indexBufferReadyBarrier.Transition.StateBefore =
            D3D12_RESOURCE_STATE_COPY_DEST;
        indexBufferReadyBarrier.Transition.StateAfter =
            D3D12_RESOURCE_STATE_INDEX_BUFFER;

        CommandList->ResourceBarrier(
            1,
            &indexBufferReadyBarrier
        );

        for (MaterialTextureUpload& materialTexture : materialTextures)
        {
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = materialTexture.texture.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.SubresourceIndex = 0;

            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = materialTexture.uploadBuffer.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = materialTexture.footprint;

            CommandList->CopyTextureRegion(
                &destination,
                0,
                0,
                0,
                &source,
                nullptr
            );

            D3D12_RESOURCE_BARRIER textureReadyBarrier{};
            textureReadyBarrier.Type =
                D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            textureReadyBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            textureReadyBarrier.Transition.pResource =
                materialTexture.texture.Get();
            textureReadyBarrier.Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            textureReadyBarrier.Transition.StateBefore =
                D3D12_RESOURCE_STATE_COPY_DEST;
            textureReadyBarrier.Transition.StateAfter =
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            CommandList->ResourceBarrier(1, &textureReadyBarrier);
        }

        dx12::ThrowIfFailed(
            CommandList->Close(),
            "Close static-resource upload command list"
        );

        std::cout
            << "Vertex, index, and texture copies plus state transitions "
            << "recorded.\n";

        

        //Fence
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        dx12::ThrowIfFailed(
            device->CreateFence(
                0,
                D3D12_FENCE_FLAG_NONE,
                IID_PPV_ARGS(fence.GetAddressOf())
            ),"CreateFence"
        );
        std::cout << "ID3D12Fence Create Success" << "\n";

        dx12::ThrowIfFailed(
            fence->SetName(L"Main Fence"),
            "ID3D12Fence::SetName"
        );

        //Event
        
        std::array<UINT64, swapChainBufferCount> frameFenceValues{};
        UINT64 nextFenceValue = 1;

        ScopedEventHandle fenceEvent{
            CreateEventW(
                nullptr,
                FALSE, // Automatic-reset event.
                FALSE, // Initially non-signaled.
                nullptr
            )
        };

        if (!fenceEvent)
        {
            const DWORD error = GetLastError();

            dx12::ThrowIfFailed(
                HRESULT_FROM_WIN32(error),
                "CreateEventW"
            );
        }

        // Submit the one-time static-resource uploads before rendering begins.
        // Their upload buffers cannot be released merely because
        // ExecuteCommandLists returned; the GPU consumes them asynchronously.
        ID3D12CommandList* const vertexUploadCommandLists[] = {
            CommandList.Get()
        };

        CommandQueue->ExecuteCommandLists(
            1,
            vertexUploadCommandLists
        );

        if (nextFenceValue == UINT64_MAX)
        {
            throw std::runtime_error(
                "Fence value space was exhausted before vertex upload."
            );
        }

        const UINT64 vertexUploadFenceValue =
            nextFenceValue;

        dx12::ThrowIfFailed(
            CommandQueue->Signal(
                fence.Get(),
                vertexUploadFenceValue
            ),
            "Signal vertex-buffer upload Fence"
        );

        ++nextFenceValue;

        const UINT64 vertexUploadCompletedValue =
            fence->GetCompletedValue();

        if (vertexUploadCompletedValue == UINT64_MAX)
        {
            throw std::runtime_error(
                "ID3D12Fence::GetCompletedValue indicates device "
                "removal during vertex-buffer upload."
            );
        }

        if (vertexUploadCompletedValue < vertexUploadFenceValue)
        {
            dx12::ThrowIfFailed(
                fence->SetEventOnCompletion(
                    vertexUploadFenceValue,
                    fenceEvent.get()
                ),
                "SetEventOnCompletion for vertex-buffer upload"
            );

            const DWORD vertexUploadWaitResult =
                WaitForSingleObject(
                    fenceEvent.get(),
                    INFINITE
                );

            if (vertexUploadWaitResult == WAIT_FAILED)
            {
                const DWORD error = GetLastError();

                dx12::ThrowIfFailed(
                    HRESULT_FROM_WIN32(error),
                    "WaitForSingleObject for vertex-buffer upload"
                );
            }

            if (vertexUploadWaitResult != WAIT_OBJECT_0)
            {
                throw std::runtime_error(
                    "Vertex-buffer upload wait returned an unexpected "
                    "result."
                );
            }
        }

        // This Fence covers every copy recorded in the initialization command
        // list, so all temporary upload resources are now safe to release.
        triangleVertexUploadBuffer.Reset();
        triangleIndexUploadBuffer.Reset();
        for (MaterialTextureUpload& materialTexture : materialTextures)
        {
            materialTexture.uploadBuffer.Reset();
            std::vector<std::uint8_t>{}.swap(
                materialTexture.image.rgba8Pixels
            );
        }

        std::cout
            << "Static geometry and texture uploads completed at Fence value "
            << vertexUploadFenceValue
            << "; temporary upload buffers were released.\n";
        std::cout << std::flush;

        //window
        //ShowWindow             
        ShowWindow(window, SW_SHOW);
        if(UpdateWindow(window) == FALSE)
        {
            const DWORD error = GetLastError();
            const HRESULT result = HRESULT_FROM_WIN32(error);
            dx12::ThrowIfFailed(result, "UpdateWindow");            
        }

        MSG message{};
        bool isRunning = true;

        CameraState cameraState{};

        float triangleRotationRadians = 0.0F;
        constexpr float rotationRadiansPerSecond = 0.6F;
        constexpr float cameraMovementUnitsPerSecond = 2.5F;
        constexpr float mouseRadiansPerPixel = 0.0025F;
        constexpr float maximumFrameDeltaSeconds = 0.1F;
        constexpr float maximumPitchRadians =
            DirectX::XM_PIDIV2 - 0.01F;

        auto previousFrameTime =
            std::chrono::steady_clock::now();

        POINT previousMousePosition{};
        bool hasPreviousMousePosition = false;

        // ================================== //
        //          MAIN ROUND
        // ================================== //
        
        while (isRunning)
        {
            // Process every message currently waiting in the queue.
            while (PeekMessageW(
                &message,
                nullptr,
                0,
                0,
                PM_REMOVE))
            {
                if (message.message == WM_QUIT)
                {
                    isRunning = false;
                    break;
                }

                TranslateMessage(&message);
                DispatchMessageW(&message);
            }

            // Do not submit another frame after WM_QUIT.
            if (!isRunning)
            {
                break;
            }

            if (windowState.isMinimized)
            {
                if (WaitMessage() == FALSE)
                {
                    const DWORD error = GetLastError();

                    dx12::ThrowIfFailed(
                        HRESULT_FROM_WIN32(error),
                        "WaitMessage"
                    );
                }

                continue;
            }

            if (windowState.resizePending)
            {
                const UINT requestedWidth =
                    windowState.pendingWidth;

                const UINT requestedHeight =
                    windowState.pendingHeight;

                if (requestedWidth == 0 ||
                    requestedHeight == 0)
                {
                    windowState.isMinimized = true;
                    windowState.resizePending = false;
                    continue;
                }

                if (requestedWidth == swapChainWidth &&
                    requestedHeight == swapChainHeight)
                {
                    windowState.resizePending = false;
                }
                else
                {
                    // Stop before releasing the old swap-chain buffers.
                    // The last submitted Fence value represents all earlier work
                    // because commands and Fence signals execute in queue order.
                    const UINT64 lastSubmittedFenceValue =
                        nextFenceValue - 1;

                    if (lastSubmittedFenceValue != 0)
                    {
                        const UINT64 completedValue =
                            fence->GetCompletedValue();

                        if (completedValue == UINT64_MAX)
                        {
                            throw std::runtime_error(
                                "ID3D12Fence::GetCompletedValue indicates "
                                "device removal during swap-chain resize."
                            );
                        }

                        if (completedValue <
                            lastSubmittedFenceValue)
                        {
                            dx12::ThrowIfFailed(
                                fence->SetEventOnCompletion(
                                    lastSubmittedFenceValue,
                                    fenceEvent.get()
                                ),
                                "Resize ID3D12Fence::SetEventOnCompletion"
                            );

                            const DWORD waitResult =
                                WaitForSingleObject(
                                    fenceEvent.get(),
                                    INFINITE
                                );

                            if (waitResult == WAIT_FAILED)
                            {
                                const DWORD error =
                                    GetLastError();

                                dx12::ThrowIfFailed(
                                    HRESULT_FROM_WIN32(error),
                                    "Resize WaitForSingleObject"
                                );
                            }

                            if (waitResult != WAIT_OBJECT_0)
                            {
                                throw std::runtime_error(
                                    "Resize WaitForSingleObject returned "
                                    "an unexpected result."
                                );
                            }
                        }
                    }

                    // ResizeBuffers requires the application to release every
                    // reference obtained through IDXGISwapChain::GetBuffer.
                    for (auto& backBuffer : backBuffers)
                    {
                        backBuffer.Reset();
                    }

                    // The queue drain above proves that no in-flight draw can
                    // still reference the old-size depth texture.
                    depthBuffer.Reset();

                    dx12::ThrowIfFailed(
                        _IDXGISwapChain3->ResizeBuffers(
                            swapChainBufferCount,
                            requestedWidth,
                            requestedHeight,
                            DXGI_FORMAT_R8G8B8A8_UNORM,
                            0
                        ),
                        "IDXGISwapChain3::ResizeBuffers"
                    );

                    D3D12_CPU_DESCRIPTOR_HANDLE
                        resizedRtvHandle = rtvHeapStart;

                    for (UINT bufferIndex = 0;
                        bufferIndex < swapChainBufferCount;
                        ++bufferIndex)
                    {
                        dx12::ThrowIfFailed(
                            _IDXGISwapChain3->GetBuffer(
                                bufferIndex,
                                IID_PPV_ARGS(
                                    backBuffers[bufferIndex]
                                        .ReleaseAndGetAddressOf()
                                )
                            ),
                            "Resize IDXGISwapChain3::GetBuffer"
                        );

                        dx12::ThrowIfFailed(
                            backBuffers[bufferIndex]->SetName(
                                backBufferNames[bufferIndex]
                            ),
                            "Resize ID3D12Resource::SetName"
                        );

                        device->CreateRenderTargetView(
                            backBuffers[bufferIndex].Get(),
                            nullptr,
                            resizedRtvHandle
                        );

                        resizedRtvHandle.ptr +=
                            static_cast<SIZE_T>(
                                rtvDescriptorSize
                            );
                    }

                    // The resized buffers have never been submitted. The allocator
                    // reuse is also safe because all earlier queue work was drained.
                    frameFenceValues.fill(0);

                    swapChainWidth = requestedWidth;
                    swapChainHeight = requestedHeight;

                    // ResizeBuffers only recreates color back buffers. The
                    // application must explicitly recreate its depth texture.
                    depthBuffer = createDepthBuffer(
                        swapChainWidth,
                        swapChainHeight
                    );

                    windowState.resizePending = false;

                    std::cout
                        << "Swap chain resized to "
                        << swapChainWidth
                        << " x "
                        << swapChainHeight
                        << ".\n";
                }
            }

            // Query this every frame. Do not assume buffer 0 and buffer 1
            // alternate in a way controlled directly by the application.
            const UINT currentBackBufferIndex =
                _IDXGISwapChain3->GetCurrentBackBufferIndex();

            if (currentBackBufferIndex >= swapChainBufferCount)
            {
                throw std::runtime_error(
                    "Swap-chain back-buffer index is out of range."
                );
            }

            // This is the Fence milestone associated with the previous use
            // of the current back buffer and its command allocator.
            const UINT64 frameFenceValue =
                frameFenceValues[currentBackBufferIndex];

            // A value of zero means this frame resource has never been submitted.
            if (frameFenceValue != 0)
            {
                const UINT64 completedValue =
                    fence->GetCompletedValue();

                if (completedValue == UINT64_MAX)
                {
                    throw std::runtime_error(
                        "ID3D12Fence::GetCompletedValue indicates "
                        "device removal."
                    );
                }

                // completedValue is what the GPU has finished.
                // frameFenceValue is what this frame resource needs.
                if (completedValue < frameFenceValue)
                {
                    dx12::ThrowIfFailed(
                        fence->SetEventOnCompletion(
                            frameFenceValue,
                            fenceEvent.get()
                        ),
                        "ID3D12Fence::SetEventOnCompletion"
                    );

                    const DWORD waitResult =
                        WaitForSingleObject(
                            fenceEvent.get(),
                            INFINITE
                        );

                    if (waitResult == WAIT_FAILED)
                    {
                        const DWORD error = GetLastError();

                        dx12::ThrowIfFailed(
                            HRESULT_FROM_WIN32(error),
                            "WaitForSingleObject"
                        );
                    }

                    if (waitResult != WAIT_OBJECT_0)
                    {
                        throw std::runtime_error(
                            "WaitForSingleObject returned "
                            "an unexpected result."
                        );
                    }
                }
            }

            // Reaching this point means that the current frame slot has either
            // never been submitted or its previous Fence value has completed.
            // It is therefore safe for the CPU to overwrite this slot's
            // Constant Buffer memory.
            const auto currentFrameTime =
                std::chrono::steady_clock::now();

            float frameDeltaSeconds =
                std::chrono::duration<float>(
                    currentFrameTime - previousFrameTime
                ).count();

            previousFrameTime = currentFrameTime;

            if (frameDeltaSeconds > maximumFrameDeltaSeconds)
            {
                frameDeltaSeconds = maximumFrameDeltaSeconds;
            }

            const bool rendererHasFocus =
                GetForegroundWindow() == window;

            if (rendererHasFocus &&
                (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0)
            {
                POINT currentMousePosition{};

                if (GetCursorPos(&currentMousePosition) == FALSE)
                {
                    const DWORD error = GetLastError();
                    dx12::ThrowIfFailed(
                        HRESULT_FROM_WIN32(error),
                        "GetCursorPos"
                    );
                }

                if (hasPreviousMousePosition)
                {
                    const LONG mouseDeltaX =
                        currentMousePosition.x -
                        previousMousePosition.x;

                    const LONG mouseDeltaY =
                        currentMousePosition.y -
                        previousMousePosition.y;

                    cameraState.yawRadians +=
                        static_cast<float>(mouseDeltaX) *
                        mouseRadiansPerPixel;

                    cameraState.pitchRadians -=
                        static_cast<float>(mouseDeltaY) *
                        mouseRadiansPerPixel;

                    if (cameraState.pitchRadians >
                        maximumPitchRadians)
                    {
                        cameraState.pitchRadians =
                            maximumPitchRadians;
                    }
                    else if (cameraState.pitchRadians <
                        -maximumPitchRadians)
                    {
                        cameraState.pitchRadians =
                            -maximumPitchRadians;
                    }

                    cameraState.yawRadians =
                        DirectX::XMScalarModAngle(
                            cameraState.yawRadians
                        );
                }

                previousMousePosition = currentMousePosition;
                hasPreviousMousePosition = true;
            }
            else
            {
                hasPreviousMousePosition = false;
            }

            const DirectX::XMMATRIX cameraRotation =
                DirectX::XMMatrixRotationRollPitchYaw(
                    cameraState.pitchRadians,
                    cameraState.yawRadians,
                    0.0F
                );

            const DirectX::XMVECTOR worldUp =
                DirectX::XMVectorSet(
                    0.0F,
                    1.0F,
                    0.0F,
                    0.0F
                );

            const DirectX::XMVECTOR cameraForward =
                DirectX::XMVector3Normalize(
                    DirectX::XMVector3TransformNormal(
                        DirectX::XMVectorSet(
                            0.0F,
                            0.0F,
                            1.0F,
                            0.0F
                        ),
                        cameraRotation
                    )
                );

            const DirectX::XMVECTOR cameraRight =
                DirectX::XMVector3Normalize(
                    DirectX::XMVector3TransformNormal(
                        DirectX::XMVectorSet(
                            1.0F,
                            0.0F,
                            0.0F,
                            0.0F
                        ),
                        cameraRotation
                    )
                );

            DirectX::XMVECTOR cameraMovement =
                DirectX::XMVectorZero();

            if (rendererHasFocus)
            {
                if ((GetAsyncKeyState('W') & 0x8000) != 0)
                {
                    cameraMovement =
                        DirectX::XMVectorAdd(
                            cameraMovement,
                            cameraForward
                        );
                }

                if ((GetAsyncKeyState('S') & 0x8000) != 0)
                {
                    cameraMovement =
                        DirectX::XMVectorSubtract(
                            cameraMovement,
                            cameraForward
                        );
                }

                if ((GetAsyncKeyState('D') & 0x8000) != 0)
                {
                    cameraMovement =
                        DirectX::XMVectorAdd(
                            cameraMovement,
                            cameraRight
                        );
                }

                if ((GetAsyncKeyState('A') & 0x8000) != 0)
                {
                    cameraMovement =
                        DirectX::XMVectorSubtract(
                            cameraMovement,
                            cameraRight
                        );
                }

                if ((GetAsyncKeyState('E') & 0x8000) != 0)
                {
                    cameraMovement =
                        DirectX::XMVectorAdd(
                            cameraMovement,
                            worldUp
                        );
                }

                if ((GetAsyncKeyState('Q') & 0x8000) != 0)
                {
                    cameraMovement =
                        DirectX::XMVectorSubtract(
                            cameraMovement,
                            worldUp
                        );
                }
            }

            if (DirectX::XMVectorGetX(
                    DirectX::XMVector3LengthSq(cameraMovement)
                ) > 0.0F)
            {
                cameraMovement =
                    DirectX::XMVectorScale(
                        DirectX::XMVector3Normalize(
                            cameraMovement
                        ),
                        cameraMovementUnitsPerSecond *
                        frameDeltaSeconds
                    );

                DirectX::XMVECTOR cameraPosition =
                    DirectX::XMLoadFloat3(
                        &cameraState.position
                    );

                cameraPosition =
                    DirectX::XMVectorAdd(
                        cameraPosition,
                        cameraMovement
                    );

                DirectX::XMStoreFloat3(
                    &cameraState.position,
                    cameraPosition
                );
            }

            triangleRotationRadians =
                DirectX::XMScalarModAngle(
                    triangleRotationRadians +
                    rotationRadiansPerSecond *
                    frameDeltaSeconds
                );

            // The glTF node transform is already converted from glTF's
            // right-handed convention to this renderer's left-handed,
            // row-vector matrix convention. Apply the existing visible
            // rotation after the imported node transform.
            const DirectX::XMMATRIX modelMatrix =
                DirectX::XMLoadFloat4x4(
                    &staticMesh.nodeTransform
                ) *
                DirectX::XMMatrixRotationZ(
                    triangleRotationRadians
                );

            DirectX::XMVECTOR modelDeterminant{};
            const DirectX::XMMATRIX inverseModelMatrix =
                DirectX::XMMatrixInverse(
                    &modelDeterminant,
                    modelMatrix
                );
            const float modelDeterminantValue =
                DirectX::XMVectorGetX(modelDeterminant);
            if (!std::isfinite(modelDeterminantValue) ||
                std::abs(modelDeterminantValue) < 1.0e-8F)
            {
                throw std::runtime_error(
                    "The Model Matrix is singular; a Normal Matrix cannot "
                    "be computed."
                );
            }
            const float modelHandedness =
                modelDeterminantValue < 0.0F ? -1.0F : 1.0F;
            const DirectX::XMMATRIX normalMatrix =
                DirectX::XMMatrixTranspose(inverseModelMatrix);

            const DirectX::XMVECTOR cameraPosition =
                DirectX::XMLoadFloat3(
                    &cameraState.position
                );

            const DirectX::XMMATRIX viewMatrix =
                DirectX::XMMatrixLookToLH(
                    cameraPosition,
                    cameraForward,
                    worldUp
                );

            const float aspectRatio =
                static_cast<float>(swapChainWidth) /
                static_cast<float>(swapChainHeight);

            const DirectX::XMMATRIX projectionMatrix =
                DirectX::XMMatrixPerspectiveFovLH(
                    DirectX::XMConvertToRadians(60.0F),
                    aspectRatio,
                    0.1F,
                    100.0F
                );

            TransformConstants transformConstants{};

            DirectX::XMStoreFloat4x4(
                &transformConstants.model,
                modelMatrix
            );

            DirectX::XMStoreFloat4x4(
                &transformConstants.view,
                viewMatrix
            );

            DirectX::XMStoreFloat4x4(
                &transformConstants.projection,
                projectionMatrix
            );

            DirectX::XMStoreFloat4x4(
                &transformConstants.normalMatrix,
                normalMatrix
            );

            const PbrConstants pbrConstants{
                .baseColorFactor = staticMaterial.baseColorFactor,
                .cameraWorldPosition = cameraState.position,
                .metallicFactor = staticMaterial.metallicFactor,
                .directionalLightDirection = directionalLightDirection,
                .roughnessFactor = staticMaterial.roughnessFactor,
                .directionalLightColor = directionalLightColor,
                .normalScale = staticMaterial.normalScale,
                .directionalLightIntensity = directionalLightIntensity,
                .ambientIntensity = ambientIntensity,
                .modelHandedness = modelHandedness,
                .padding = 0.0F
            };

            std::memcpy(
                mappedTransformConstantData[
                    currentBackBufferIndex
                ],
                &transformConstants,
                sizeof(transformConstants)
            );

            // The GPU has finished the previous commands stored in this
            // allocator, so its command memory can now be reused.
            dx12::ThrowIfFailed(
                commandAllocators[currentBackBufferIndex]->Reset(),
                "ID3D12CommandAllocator::Reset"
            );

            // Reset changes the closed command list back into recording state
            // and associates it with the current frame's allocator.
            dx12::ThrowIfFailed(
                CommandList->Reset(
                    commandAllocators[currentBackBufferIndex].Get(),
                    nullptr
                ),
                "ID3D12GraphicsCommandList::Reset"
            );

            // Select the RTV that belongs to the current swap-chain buffer.
            D3D12_CPU_DESCRIPTOR_HANDLE currentBackBufferRtv =
                rtvHeapStart;

            currentBackBufferRtv.ptr +=
                static_cast<SIZE_T>(currentBackBufferIndex) *
                static_cast<SIZE_T>(rtvDescriptorSize);

            // The swap-chain buffer begins the frame in PRESENT state.
            D3D12_RESOURCE_BARRIER toRenderTargetBarrier{};
            toRenderTargetBarrier.Type =
                D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toRenderTargetBarrier.Flags =
                D3D12_RESOURCE_BARRIER_FLAG_NONE;
            toRenderTargetBarrier.Transition.pResource =
                backBuffers[currentBackBufferIndex].Get();
            toRenderTargetBarrier.Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            toRenderTargetBarrier.Transition.StateBefore =
                D3D12_RESOURCE_STATE_PRESENT;
            toRenderTargetBarrier.Transition.StateAfter =
                D3D12_RESOURCE_STATE_RENDER_TARGET;

            CommandList->ResourceBarrier(
                1,
                &toRenderTargetBarrier
            );
            
            //Draw
            D3D12_VIEWPORT viewport{};

            viewport.TopLeftX = 0.0f;
            viewport.TopLeftY = 0.0f;
            viewport.Width =
                static_cast<FLOAT>(swapChainWidth);
            viewport.Height =
                static_cast<FLOAT>(swapChainHeight);
            viewport.MinDepth = 0.0f;
            viewport.MaxDepth = 1.0f;

            D3D12_RECT scissorRect{};

            scissorRect.left = 0;
            scissorRect.top = 0;
            scissorRect.right =
                static_cast<LONG>(swapChainWidth);
            scissorRect.bottom =
                static_cast<LONG>(swapChainHeight);

            CommandList->RSSetViewports(
                1,
                &viewport
            );

            CommandList->RSSetScissorRects(
                1,
                &scissorRect
            );

            CommandList->OMSetRenderTargets(
                1,
                &currentBackBufferRtv,
                FALSE,
                &depthStencilView
            );

            constexpr FLOAT clearColor[4] = {
                0.1F,
                0.2F,
                0.4F,
                1.0F
            };

            CommandList->ClearRenderTargetView(
                currentBackBufferRtv,
                clearColor,
                0,
                nullptr
            );

            // Clear 1.0 represents the farthest depth in the current normal-Z
            // convention. Stencil is not present in DXGI_FORMAT_D32_FLOAT.
            CommandList->ClearDepthStencilView(
                depthStencilView,
                D3D12_CLEAR_FLAG_DEPTH,
                1.0f,
                0,
                0,
                nullptr
            );

            // Descriptor tables contain GPU descriptor handles, so their heap
            // must be shader-visible and bound before setting root parameter 1.
            ID3D12DescriptorHeap* const shaderVisibleDescriptorHeaps[] = {
                materialSrvHeap.Get()
            };

            CommandList->SetDescriptorHeaps(
                1,
                shaderVisibleDescriptorHeaps
            );

            CommandList->SetGraphicsRootSignature(
                rootSignature.Get()
            );

            CommandList->SetPipelineState(
                trianglePipelineState.Get()
            );

            // Root parameter 0 is the CBV at HLSL register b0.
            // Select the Constant Buffer owned by the current frame slot.
            CommandList->SetGraphicsRootConstantBufferView(
                0,
                transformConstantBuffers[
                    currentBackBufferIndex
                ]->GetGPUVirtualAddress()
            );

            // Root parameter 1 is the three-SRV table mapped to t0-t2.
            CommandList->SetGraphicsRootDescriptorTable(
                1,
                materialSrvHeap->GetGPUDescriptorHandleForHeapStart()
            );

            // Root parameter 2 copies twenty 32-bit PBR values into b1.
            CommandList->SetGraphicsRoot32BitConstants(
                2,
                20,
                &pbrConstants,
                0
            );

            CommandList->IASetPrimitiveTopology(
                D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST
            );

            CommandList->IASetVertexBuffers(
                0,
                1,
                &triangleVertexBufferView
            );

            // The index view supplies vertex numbers; the vertex view above
            // supplies POSITION, NORMAL, TEXCOORD, and TANGENT data for them.
            CommandList->IASetIndexBuffer(
                &triangleIndexBufferView
            );

            CommandList->DrawIndexedInstanced(
                static_cast<UINT>(staticMesh.indices.size()),
                1,
                0,
                0,
                0
            );

            // Present requires the swap-chain buffer to be back in PRESENT state.
            D3D12_RESOURCE_BARRIER toPresentBarrier{};
            toPresentBarrier.Type =
                D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toPresentBarrier.Flags =
                D3D12_RESOURCE_BARRIER_FLAG_NONE;
            toPresentBarrier.Transition.pResource =
                backBuffers[currentBackBufferIndex].Get();
            toPresentBarrier.Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            toPresentBarrier.Transition.StateBefore =
                D3D12_RESOURCE_STATE_RENDER_TARGET;
            toPresentBarrier.Transition.StateAfter =
                D3D12_RESOURCE_STATE_PRESENT;

            CommandList->ResourceBarrier(
                1,
                &toPresentBarrier
            );

            dx12::ThrowIfFailed(
                CommandList->Close(),
                "ID3D12GraphicsCommandList::Close"
            );

            ID3D12CommandList* const commandLists[] = {
                CommandList.Get()
            };

            // ExecuteCommandLists returns void. Incorrect command recording is
            // primarily detected through the Direct3D 12 Debug Layer.
            CommandQueue->ExecuteCommandLists(
                1,
                commandLists
            );

            dx12::ThrowIfFailed(
                _IDXGISwapChain3->Present(
                    1,
                    0
                ),
                "IDXGISwapChain3::Present"
            );

            // UINT64_MAX is reserved here as the device-removal result
            // from GetCompletedValue, so do not submit it as a normal value.
            if (nextFenceValue == UINT64_MAX)
            {
                throw std::runtime_error(
                    "Fence value space has been exhausted."
                );
            }

            const UINT64 submittedFenceValue =
                nextFenceValue;

            dx12::ThrowIfFailed(
                CommandQueue->Signal(
                    fence.Get(),
                    submittedFenceValue
                ),
                "ID3D12CommandQueue::Signal"
            );

            // Record which GPU milestone must complete before this exact
            // back buffer and allocator pair can be reused.
            frameFenceValues[currentBackBufferIndex] =
                submittedFenceValue;

            ++nextFenceValue;
        }
      
        const UINT64 lastSubmittedFenceValue =
            nextFenceValue - 1;

        if (lastSubmittedFenceValue != 0)
        {
            const UINT64 completedValue =
                fence->GetCompletedValue();

            if (completedValue == UINT64_MAX)
            {
                throw std::runtime_error(
                    "ID3D12Fence::GetCompletedValue indicates "
                    "device removal during shutdown."
                );
            }

            if (completedValue < lastSubmittedFenceValue)
            {
                dx12::ThrowIfFailed(
                    fence->SetEventOnCompletion(
                        lastSubmittedFenceValue,
                        fenceEvent.get()
                    ),
                    "Shutdown ID3D12Fence::SetEventOnCompletion"
                );

                const DWORD waitResult =
                    WaitForSingleObject(
                        fenceEvent.get(),
                        INFINITE
                    );

                if (waitResult == WAIT_FAILED)
                {
                    const DWORD error = GetLastError();

                    dx12::ThrowIfFailed(
                        HRESULT_FROM_WIN32(error),
                        "Shutdown WaitForSingleObject"
                    );
                }

                if (waitResult != WAIT_OBJECT_0)
                {
                    throw std::runtime_error(
                        "Shutdown WaitForSingleObject returned "
                        "an unexpected result."
                    );
                }
            }
        }

#if defined(_DEBUG)
        Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
        dx12::ThrowIfFailed(
            device.As(&infoQueue),
            "Query ID3D12InfoQueue from ID3D12Device");
        const UINT64 storedMessageCount =
            infoQueue->GetNumStoredMessages();

        std::cout << "Direct3D 12 Debug Layer stored messages: "
                  << storedMessageCount << '\n';

        for (UINT64 messageIndex = 0;
            messageIndex < storedMessageCount;
            ++messageIndex)
        {
            SIZE_T messageByteCount = 0;
            dx12::ThrowIfFailed(
                infoQueue->GetMessage(
                    messageIndex,
                    nullptr,
                    &messageByteCount
                ),
                "ID3D12InfoQueue::GetMessage size query"
            );

            const SIZE_T alignedElementCount =
                (messageByteCount + sizeof(std::max_align_t) - 1) /
                sizeof(std::max_align_t);

            std::vector<std::max_align_t> messageStorage(
                alignedElementCount
            );

            auto* debugMessage =
                reinterpret_cast<D3D12_MESSAGE*>(
                    messageStorage.data()
                );

            dx12::ThrowIfFailed(
                infoQueue->GetMessage(
                    messageIndex,
                    debugMessage,
                    &messageByteCount
                ),
                "ID3D12InfoQueue::GetMessage"
            );

            std::cout
                << "  ["
                << messageIndex
                << "] severity="
                << static_cast<int>(debugMessage->Severity)
                << ", id="
                << static_cast<int>(debugMessage->ID)
                << ": "
                << debugMessage->pDescription
                << '\n';
        }
#endif

        return static_cast<int>(message.wParam);
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
