#include "HResult.h"

#include <d3d12.h>
#include <wrl/client.h>
#include <Windows.h>
#include <stdexcept>
#include <dxgi.h>
#include <dxgi1_4.h>

#include <cstdlib>
#include <exception>
#include <iostream>

#include <array>

namespace
{
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
        case WM_CLOSE:
        if (DestroyWindow(window) == FALSE)
        {
            const DWORD error = GetLastError();
            std::string errorString = dx12::FormatHResult(HRESULT_FROM_WIN32(error));
            std::cout << "DestroyWindow failed:" << error << errorString << "\n";
            PostQuitMessage(EXIT_FAILURE);
        }
            return 0;
        
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProcW(window, message, wParam, lParam);
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
            nullptr);  
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
        
        constexpr UINT swapChainBufferCount = 2;

        DXGI_SWAP_CHAIN_DESC1 chainDesc1{};
        chainDesc1.Width = Width;
        chainDesc1.Height = Height;
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

        UINT rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE currentRtvHandle =
            desHeap->GetCPUDescriptorHandleForHeapStart();
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
                currentRtvHandle
            );

            currentRtvHandle.ptr +=
                static_cast<SIZE_T>(rtvDescriptorSize);
        }
        std::cout << "RTV Create Success" << "\n";


        //Command Allocator
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> CommandAllocator;
        dx12::ThrowIfFailed(
            device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(CommandAllocator.GetAddressOf())
            ),"CreateCommandAllocator"
        );

        dx12::ThrowIfFailed(
            CommandAllocator->SetName(L"Main Graphics Command Allocator"),
            "ID3D12CommandAllocator::SetName"
        );
        std::cout << "ID3D12CommandAllocator Create Success" << "\n";

        //Command List
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> CommandList;
        dx12::ThrowIfFailed(
            device->CreateCommandList(
                0,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                CommandAllocator.Get(),
                nullptr,
                IID_PPV_ARGS(CommandList.GetAddressOf())
            ),"CreateCommandList"
        );

        dx12::ThrowIfFailed(
            CommandList->SetName(L"Main Graphics Command List"),
            "ID3D12GraphicsCommandList::SetName"
        );
        std::cout << "ID3D12GraphicsCommandList Create Success" << "\n";

        dx12::ThrowIfFailed(
            CommandList->Close(),
            "ID3D12GraphicsCommandList::Close"
        );
        std::cout << "ID3D12GraphicsCommandList Close Success" << "\n";

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
        
        constexpr UINT64 fenceValue = 1;

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

        ID3D12CommandList* const commandLists[] = {
            CommandList.Get()
        };

        CommandQueue->ExecuteCommandLists(
            1,
            commandLists
        );

        dx12::ThrowIfFailed(
            CommandQueue->Signal(
                fence.Get(),
                fenceValue
            ),
            "ID3D12CommandQueue::Signal" //signal: let fence inner value = fenceValue
        );

        const UINT64 completedValue = fence->GetCompletedValue();

        if (completedValue == UINT64_MAX)
        {
            throw std::runtime_error(
                "ID3D12Fence::GetCompletedValue indicates device removal."
            );
        }

        if (completedValue < fenceValue)
        {
            dx12::ThrowIfFailed(
                fence->SetEventOnCompletion(
                    fenceValue,
                    fenceEvent.get()
                ),
                "ID3D12Fence::SetEventOnCompletion" //reach fenceValue then emit this event
            );

            const DWORD waitResult = WaitForSingleObject(
                fenceEvent.get(),
                INFINITE
            ); //block this

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
                    "WaitForSingleObject returned an unexpected result."
                );
            }
        }

        std::cout << "GPU command submission completed successfully.\n";

        //ShowWindow             
        ShowWindow(window, SW_SHOW);
        if(UpdateWindow(window) == FALSE)
        {
            const DWORD error = GetLastError();
            const HRESULT result = HRESULT_FROM_WIN32(error);
            dx12::ThrowIfFailed(result, "UpdateWindow");            
        }
        
        //msg
        MSG message{};
        int result;
        while ((result = GetMessageW(&message, nullptr, 0, 0)) != 0)
        {   
            if(result == -1)
            {
                //report failure
                const DWORD error = GetLastError();
                const HRESULT errorResult = HRESULT_FROM_WIN32(error);
                dx12::ThrowIfFailed(errorResult, "window");
                break;
            }
        TranslateMessage(&message);
        DispatchMessageW(&message);           
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
