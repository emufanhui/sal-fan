#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dolby {

class InterleavedPcmQueue;

class OmniphonyPcmBridge {
public:
    static constexpr std::uint32_t kSampleRate = 48'000;
    static constexpr std::size_t kChannels = 12;
    static constexpr std::size_t kBytesPerSample = sizeof(float);
    static constexpr std::size_t kFrameBytes =
        kChannels * kBytesPerSample;

    explicit OmniphonyPcmBridge(
        const std::wstring& pipeName =
            LR"(\\.\pipe\SpatialAudioLab.Pcm714)");

    ~OmniphonyPcmBridge();

    OmniphonyPcmBridge(const OmniphonyPcmBridge&) = delete;
    OmniphonyPcmBridge& operator=(const OmniphonyPcmBridge&) = delete;

    // 创建 pipe 并等待 mpv 连接。
    void WaitForConnection();

    // 非阻塞地读取当前已经到达的 PCM。
    //
    // 返回：
    //   true  = 本次至少收到了一些 PCM
    //   false = 当前没有数据
    //
    // 完整 frame 不足 kFrameBytes 时会保留到下一次调用，
    // 因此不会因为 ReadFile 的分块方式破坏 PCM frame。
    bool ReadAvailable(InterleavedPcmQueue& queue);

    bool IsConnected() const noexcept;
    bool IsClientClosed() const noexcept;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace dolby