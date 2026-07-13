#pragma once

#include <windows.h>

#include <array>
#include <iomanip>
#include <source_location>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace dx12
{
inline std::string FormatHResult(const HRESULT result)
{
    std::array<char, 1024> messageBuffer{};
    const DWORD messageLength = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        static_cast<DWORD>(result),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        messageBuffer.data(),
        static_cast<DWORD>(messageBuffer.size()),
        nullptr);

    std::string message;
    if (messageLength != 0)
    {
        message.assign(messageBuffer.data(), messageLength);

        while (!message.empty() &&
               (message.back() == '\r' || message.back() == '\n'))
        {
            message.pop_back();
        }
    }
    else
    {
        message = "No system error message is available.";
    }

    return message;
}

inline void ThrowIfFailed(
    const HRESULT result,
    const std::string_view operation,
    const std::source_location location = std::source_location::current())
{
    if (SUCCEEDED(result))
    {
        return;
    }

    std::ostringstream error;
    error << operation << " failed with HRESULT 0x" << std::uppercase
          << std::hex << std::setw(8) << std::setfill('0')
          << static_cast<unsigned long>(result) << std::dec << " ("
          << FormatHResult(result) << ") at " << location.file_name() << ':'
          << location.line();

    throw std::runtime_error(error.str());
}
} // namespace dx12
