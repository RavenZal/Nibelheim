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
        dx12::ThrowIfFailed(
        CommandList->Close(),
        "Initial ID3D12GraphicsCommandList::Close"
        );

        std::cout << "ID3D12GraphicsCommandList Create and Close initially Success" << "\n";

        

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
