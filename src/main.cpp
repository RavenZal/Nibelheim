#include "HResult.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdlib>
#include <exception>
#include <iostream>

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

#if defined(_DEBUG)
        Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
        dx12::ThrowIfFailed(
            device.As(&infoQueue),
            "Query ID3D12InfoQueue from ID3D12Device");
        std::cout << "Direct3D 12 Debug Layer stored messages: "
                  << infoQueue->GetNumStoredMessages() << '\n';
#endif

        return EXIT_SUCCESS;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
