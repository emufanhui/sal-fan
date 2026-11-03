#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

#include <windows.h>

namespace dolby {

// Wide output through std::wcout stops at the first non-ASCII character under
// the default C locale: the conversion fails, the stream sets badbit, and
// every later write is silently dropped. Endpoint names are localised, so
// anything that prints them has to go through std::cout as UTF-8 instead.
inline std::string Utf8(const std::wstring_view value) {
    if (value.empty()) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                          static_cast<int>(value.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
        throw std::runtime_error("Could not encode endpoint name as UTF-8");
    }
    std::string result(static_cast<std::size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, value.data(),
                            static_cast<int>(value.size()),
                            result.data(), bytes, nullptr, nullptr) != bytes) {
        throw std::runtime_error("Could not encode endpoint name as UTF-8");
    }
    return result;
}

} // namespace dolby
