#include "omniphony_pcm_bridge.h"

#include "multi_endpoint_renderer.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace dolby {

namespace {

[[noreturn]] void ThrowWin32Error(const char* operation) {
    const DWORD error = GetLastError();

    throw std::runtime_error(
        std::string(operation) +
        " failed, Win32 error=" +
        std::to_string(error));
}

std::int16_t FloatToInt16(float sample) {
    if (!std::isfinite(sample)) {
        sample = 0.0f;
    }

    const double value = std::clamp(
        static_cast<double>(sample),
        -1.0,
        1.0);

    return static_cast<std::int16_t>(
        std::lround(value * 32767.0));
}

} // namespace

struct OmniphonyPcmBridge::Impl {
    HANDLE pipe = INVALID_HANDLE_VALUE;

    std::wstring pipeName;

    std::vector<std::uint8_t> pendingBytes;

    bool connected = false;
    bool clientClosed = false;

    explicit Impl(const std::wstring& name)
        : pipeName(name) {
    }

    ~Impl() {
        Close();
    }

    void Close() noexcept {
        if (pipe != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(pipe);
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            pipe = INVALID_HANDLE_VALUE;
        }

        connected = false;
    }

    void Create() {
        if (pipe != INVALID_HANDLE_VALUE) {
            return;
        }

        pipe = CreateNamedPipeW(
            pipeName.c_str(),
            PIPE_ACCESS_INBOUND,
            PIPE_TYPE_BYTE |
                PIPE_READMODE_BYTE |
                PIPE_WAIT,
            1,
            1024 * 1024,
            1024 * 1024,
            0,
            nullptr);

        if (pipe == INVALID_HANDLE_VALUE) {
            ThrowWin32Error("CreateNamedPipeW");
        }
    }

    void Connect() {
        Create();

        const BOOL result = ConnectNamedPipe(pipe, nullptr);

        if (!result) {
            const DWORD error = GetLastError();

            if (error != ERROR_PIPE_CONNECTED) {
                ThrowWin32Error("ConnectNamedPipe");
            }
        }

        connected = true;
        clientClosed = false;
    }

    bool ReadAvailable(InterleavedPcmQueue& queue) {
        if (!connected || pipe == INVALID_HANDLE_VALUE) {
            return false;
        }

        DWORD available = 0;

        if (!PeekNamedPipe(
                pipe,
                nullptr,
                0,
                nullptr,
                &available,
                nullptr)) {

            const DWORD error = GetLastError();

            if (error == ERROR_BROKEN_PIPE ||
                error == ERROR_PIPE_NOT_CONNECTED) {
                clientClosed = true;
                connected = false;
                return false;
            }

            ThrowWin32Error("PeekNamedPipe");
        }

        if (available == 0) {
            return false;
        }

        constexpr DWORD kReadChunk = 64 * 1024;

        std::vector<std::uint8_t> chunk(
            std::min<DWORD>(available, kReadChunk));

        DWORD bytesRead = 0;

        if (!ReadFile(
                pipe,
                chunk.data(),
                static_cast<DWORD>(chunk.size()),
                &bytesRead,
                nullptr)) {

            const DWORD error = GetLastError();

            if (error == ERROR_BROKEN_PIPE ||
                error == ERROR_PIPE_NOT_CONNECTED) {
                clientClosed = true;
                connected = false;
                return false;
            }

            ThrowWin32Error("ReadFile");
        }

        if (bytesRead == 0) {
            return false;
        }

        pendingBytes.insert(
            pendingBytes.end(),
            chunk.begin(),
            chunk.begin() + bytesRead);

        const std::size_t completeBytes =
            pendingBytes.size() -
            (pendingBytes.size() % kFrameBytes);

        if (completeBytes == 0) {
            return true;
        }

        const std::size_t frameCount =
            completeBytes / kFrameBytes;

        const auto* input =
            reinterpret_cast<const float*>(pendingBytes.data());

        // The queue owns the float -> int16 conversion now, so the crossover,
        // when armed, sees the block before it is quantised.
        queue.AppendInterleaved(input, frameCount);

        pendingBytes.erase(
            pendingBytes.begin(),
            pendingBytes.begin() +
                static_cast<std::ptrdiff_t>(completeBytes));

        (void)frameCount;

        return true;
    }
};

OmniphonyPcmBridge::OmniphonyPcmBridge(
    const std::wstring& pipeName)
    : impl_(new Impl(pipeName)) {
}

OmniphonyPcmBridge::~OmniphonyPcmBridge() {
    delete impl_;
    impl_ = nullptr;
}

void OmniphonyPcmBridge::WaitForConnection() {
    if (!impl_) {
        throw std::runtime_error(
            "Omniphony PCM bridge is not initialized");
    }

    impl_->Connect();
}

bool OmniphonyPcmBridge::ReadAvailable(
    InterleavedPcmQueue& queue) {

    if (!impl_) {
        throw std::runtime_error(
            "Omniphony PCM bridge is not initialized");
    }

    return impl_->ReadAvailable(queue);
}

bool OmniphonyPcmBridge::IsConnected() const noexcept {
    return impl_ != nullptr && impl_->connected;
}

bool OmniphonyPcmBridge::IsClientClosed() const noexcept {
    return impl_ != nullptr && impl_->clientClosed;
}

} // namespace dolby