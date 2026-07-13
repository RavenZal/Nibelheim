#include "HResult.h"

#include <d3d12.h>
#include <wrl/client.h>
#include <Windows.h>
#include <stdexcept>

#include <cstdlib>
#include <exception>
#include <iostream>

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


        //Device
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        constexpr D3D_FEATURE_LEVEL minimumFeatureLevel =
            D3D_FEATURE_LEVEL_11_0;

        dx12::ThrowIfFailed(
            D3D12CreateDevice(
                nullptr,
                minimumFeatureLevel,
                IID_PPV_ARGS(device.GetAddressOf())),
            "D3D12CreateDevice");

        std::cout << "Direct3D 12 device created successfully "
                     "(minimum feature level 11_0).\n";

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
