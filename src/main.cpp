#include "HResult.h"

#include <d3d12.h>
#include <wrl/client.h>
#include <Windows.h>
#include <stdexcept>
#include <dxgi.h>
#include <dxgi1_4.h>
#include <DirectXMath.h>
#include <cstddef>

#include <cstdlib>
#include <exception>
#include <iostream>

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

struct Vertex
{
    float position[3];
    float color[4];
};

static_assert(
    sizeof(Vertex) == sizeof(float) * 7,
    "Vertex must contain exactly seven floats."
);

struct TransformConstants final
{
    DirectX::XMFLOAT4X4 transform;
};

static_assert(
    sizeof(TransformConstants) == sizeof(float) * 16,
    "TransformConstants must contain exactly one 4x4 float matrix."
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

        // Root parameter 0 supplies one Constant Buffer View to vertex-shader
        // register b0. It is a root descriptor, so this milestone does not
        // require a shader-visible CBV/SRV/UAV descriptor heap.
        D3D12_ROOT_PARAMETER transformRootParameter{};

        transformRootParameter.ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_CBV;

        transformRootParameter.Descriptor.ShaderRegister = 0;
        transformRootParameter.Descriptor.RegisterSpace = 0;

        transformRootParameter.ShaderVisibility =
            D3D12_SHADER_VISIBILITY_VERTEX;

        D3D12_ROOT_SIGNATURE_DESC rootSignatureDescription{};

        rootSignatureDescription.NumParameters = 1;
        rootSignatureDescription.pParameters =
            &transformRootParameter;

        rootSignatureDescription.NumStaticSamplers = 0;
        rootSignatureDescription.pStaticSamplers = nullptr;

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

        //Vertex Buffer
        constexpr std::array<Vertex, 3> triangleVertices{{
            {
                {0.0f, 0.6f, 0.0f},
                {1.0f, 0.0f, 0.0f, 1.0f}
            },
            {
                {0.6f, -0.6f, 0.0f},
                {0.0f, 1.0f, 0.0f, 1.0f}
            },
            {
                {-0.6f, -0.6f, 0.0f},
                {0.0f, 0.0f, 1.0f, 1.0f}
            }
        }};

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

        constexpr UINT vertexBufferSize =
            static_cast<UINT>(sizeof(triangleVertices));

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

        Microsoft::WRL::ComPtr<ID3D12Resource>
            triangleVertexBuffer;

        Microsoft::WRL::ComPtr<ID3D12Resource>
            triangleVertexUploadBuffer;

        dx12::ThrowIfFailed(
            device->CreateCommittedResource(
                &vertexDefaultHeapProperties,
                D3D12_HEAP_FLAG_NONE,
                &vertexBufferDescription,
                D3D12_RESOURCE_STATE_COPY_DEST,
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
                L"Triangle Vertex Buffer"
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
                L"Triangle Vertex Upload Buffer"
            ),
            "ID3D12Resource::SetName "
            "for triangle vertex upload buffer"
        );

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
            triangleVertices.data(),
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
        
            //explain buffer
        D3D12_VERTEX_BUFFER_VIEW
        triangleVertexBufferView{};

        triangleVertexBufferView.BufferLocation =
            triangleVertexBuffer->GetGPUVirtualAddress();

        triangleVertexBufferView.SizeInBytes =
            vertexBufferSize;

        triangleVertexBufferView.StrideInBytes =
            static_cast<UINT>(sizeof(Vertex));

        std::cout
            << "Triangle default and upload vertex buffers created: "
            << triangleVertices.size()
            << " vertices, "
            << vertexBufferSize
            << " bytes; GPU upload pending.\n";

        constexpr std::array<
            D3D12_INPUT_ELEMENT_DESC,
            2
        > triangleInputElements{{
            {
                "POSITION",
                0,
                DXGI_FORMAT_R32G32B32_FLOAT,
                0,
                0,
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
                0
            },
            {
                "COLOR",
                0,
                DXGI_FORMAT_R32G32B32A32_FLOAT,
                0,
                12,
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
            D3D12_CULL_MODE_NONE;

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

        depthStencilDescription.DepthEnable = FALSE;

        depthStencilDescription.DepthWriteMask =
            D3D12_DEPTH_WRITE_MASK_ZERO;

        depthStencilDescription.DepthFunc =
            D3D12_COMPARISON_FUNC_ALWAYS;

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
            DXGI_FORMAT_UNKNOWN;

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

        dx12::ThrowIfFailed(
            CommandList->Close(),
            "Close vertex upload command list"
        );

        std::cout
            << "Vertex-buffer copy and state transition recorded.\n";

        

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

        // Submit the one-time vertex upload before rendering begins. The
        // upload buffer cannot be released merely because ExecuteCommandLists
        // returned; the GPU consumes it asynchronously. Signal and wait for a
        // dedicated initialization milestone before releasing that resource.
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

        triangleVertexUploadBuffer.Reset();

        std::cout
            << "Triangle vertex upload completed at Fence value "
            << vertexUploadFenceValue
            << "; the temporary upload buffer was released.\n";

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

        // Teaching-only animation state. This is deliberately frame-rate
        // dependent so the current milestone does not introduce a timer system.
        float triangleRotationRadians = 0.0F;
        constexpr float rotationPerRenderedFrame = 0.01F;

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
            triangleRotationRadians +=
                rotationPerRenderedFrame;

            if (triangleRotationRadians >= DirectX::XM_2PI)
            {
                triangleRotationRadians -= DirectX::XM_2PI;
            }

            const DirectX::XMMATRIX transformMatrix =
                DirectX::XMMatrixRotationZ(
                    triangleRotationRadians
                );

            TransformConstants transformConstants{};

            DirectX::XMStoreFloat4x4(
                &transformConstants.transform,
                transformMatrix
            );

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
                nullptr
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

            CommandList->IASetPrimitiveTopology(
                D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST
            );

            CommandList->IASetVertexBuffers(
                0,
                1,
                &triangleVertexBufferView
            );

            CommandList->DrawInstanced(
                3,
                1,
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
        std::cout << "Direct3D 12 Debug Layer stored messages: "
                  << infoQueue->GetNumStoredMessages() << '\n';
#endif

        return static_cast<int>(message.wParam);
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
