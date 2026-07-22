#include "WicImageLoader.h"

#include "HResult.h"

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace
{

class ScopedComInitialization final
{
public:
    ScopedComInitialization()
    {
        const HRESULT result = CoInitializeEx(
            nullptr,
            COINIT_MULTITHREADED
        );
        dx12::ThrowIfFailed(result, "CoInitializeEx for WIC image decode");
        initialized_ = true;
    }

    ~ScopedComInitialization() noexcept
    {
        if (initialized_)
        {
            CoUninitialize();
        }
    }

    ScopedComInitialization(const ScopedComInitialization&) = delete;
    ScopedComInitialization& operator=(const ScopedComInitialization&) = delete;

private:
    bool initialized_ = false;
};

} // namespace

namespace dx12
{

DecodedImage DecodeWicImage(const MaterialTextureSource& source)
{
    const bool hasEmbeddedBytes = !source.embeddedImageBytes.empty();
    const bool hasExternalPath = !source.imagePath.empty();
    if (hasEmbeddedBytes == hasExternalPath)
    {
        throw std::runtime_error(
            "A glTF image source must contain exactly one of embedded bytes "
            "or an external path."
        );
    }

    ScopedComInitialization comInitialization;

    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    HRESULT factoryResult = CoCreateInstance(
        CLSID_WICImagingFactory2,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(factory.GetAddressOf())
    );
    if (factoryResult == REGDB_E_CLASSNOTREG)
    {
        factoryResult = CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(factory.ReleaseAndGetAddressOf())
        );
    }
    ThrowIfFailed(factoryResult, "CoCreateInstance for WIC imaging factory");

    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    Microsoft::WRL::ComPtr<IWICStream> memoryStream;
    if (hasEmbeddedBytes)
    {
        if (source.embeddedImageBytes.size() >
            std::numeric_limits<DWORD>::max())
        {
            throw std::runtime_error(
                "Embedded glTF image exceeds the WIC memory-stream limit."
            );
        }

        ThrowIfFailed(
            factory->CreateStream(memoryStream.GetAddressOf()),
            "IWICImagingFactory::CreateStream"
        );
        ThrowIfFailed(
            memoryStream->InitializeFromMemory(
                const_cast<BYTE*>(source.embeddedImageBytes.data()),
                static_cast<DWORD>(source.embeddedImageBytes.size())
            ),
            "IWICStream::InitializeFromMemory"
        );
        ThrowIfFailed(
            factory->CreateDecoderFromStream(
                memoryStream.Get(),
                nullptr,
                WICDecodeMetadataCacheOnLoad,
                decoder.GetAddressOf()
            ),
            "IWICImagingFactory::CreateDecoderFromStream"
        );
    }
    else
    {
        ThrowIfFailed(
            factory->CreateDecoderFromFilename(
                source.imagePath.c_str(),
                nullptr,
                GENERIC_READ,
                WICDecodeMetadataCacheOnLoad,
                decoder.GetAddressOf()
            ),
            "IWICImagingFactory::CreateDecoderFromFilename"
        );
    }

    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    ThrowIfFailed(
        decoder->GetFrame(0, frame.GetAddressOf()),
        "IWICBitmapDecoder::GetFrame"
    );

    UINT width = 0;
    UINT height = 0;
    ThrowIfFailed(
        frame->GetSize(&width, &height),
        "IWICBitmapFrameDecode::GetSize"
    );
    if (width == 0 || height == 0)
    {
        throw std::runtime_error("WIC decoded an image with zero dimensions.");
    }
    if (width > std::numeric_limits<UINT>::max() / 4U)
    {
        throw std::runtime_error("Decoded image row stride exceeds UINT.");
    }

    const UINT rowStride = width * 4U;
    if (height > std::numeric_limits<UINT>::max() / rowStride)
    {
        throw std::runtime_error("Decoded image byte size exceeds UINT.");
    }
    const UINT byteSize = rowStride * height;

    WICPixelFormatGUID sourceFormat{};
    ThrowIfFailed(
        frame->GetPixelFormat(&sourceFormat),
        "IWICBitmapFrameDecode::GetPixelFormat"
    );

    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    ThrowIfFailed(
        factory->CreateFormatConverter(converter.GetAddressOf()),
        "IWICImagingFactory::CreateFormatConverter"
    );

    BOOL canConvert = FALSE;
    ThrowIfFailed(
        converter->CanConvert(
            sourceFormat,
            GUID_WICPixelFormat32bppRGBA,
            &canConvert
        ),
        "IWICFormatConverter::CanConvert"
    );
    if (canConvert == FALSE)
    {
        throw std::runtime_error(
            "WIC cannot convert the glTF image to 32bpp RGBA."
        );
    }

    ThrowIfFailed(
        converter->Initialize(
            frame.Get(),
            GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom
        ),
        "IWICFormatConverter::Initialize"
    );

    DecodedImage image;
    image.width = width;
    image.height = height;
    image.rowStride = rowStride;
    image.rgba8Pixels.resize(byteSize);
    ThrowIfFailed(
        converter->CopyPixels(
            nullptr,
            rowStride,
            byteSize,
            image.rgba8Pixels.data()
        ),
        "IWICFormatConverter::CopyPixels"
    );

    return image;
}

} // namespace dx12
