#include "crossover_stage.h"

#include <Windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>

namespace dolby {
namespace {

constexpr wchar_t kDllName[] = L"cdsp_cpp.dll";

template <typename Function>
Function Resolve(const HMODULE module, const char* name) {
    return reinterpret_cast<Function>(reinterpret_cast<void*>(GetProcAddress(module, name)));
}

std::wstring Text(const std::filesystem::path& path) {
    return path.wstring();
}

std::wstring Number(std::size_t value) {
    return std::to_wstring(value);
}

// The top-level "samplerate" value, or 0 when the config does not carry one.
std::size_t ReadSampleRate(const std::string& json) {
    const std::string key = "\"samplerate\"";
    std::size_t at = json.find(key);
    if (at == std::string::npos) return 0;
    at = json.find(':', at + key.size());
    if (at == std::string::npos) return 0;
    ++at;
    while (at < json.size() && (json[at] == ' ' || json[at] == '\t')) ++at;
    std::size_t value = 0;
    bool digits = false;
    while (at < json.size() && json[at] >= '0' && json[at] <= '9') {
        value = value * 10 + static_cast<std::size_t>(json[at] - '0');
        digits = true;
        ++at;
    }
    return digits ? value : 0;
}

// cdsp designs every biquad and every millisecond delay for the samplerate in
// the JSON, and it does not resample. Rewriting that one scalar is what lets a
// single config follow the stream across 44.1, 48 and 96 kHz instead of asking
// for one JSON per rate. Nothing else in the document is touched, so filter
// names, paths and FIR file references survive verbatim.
std::string WithSampleRate(const std::string& json, const std::size_t rate) {
    const std::string key = "\"samplerate\"";
    std::size_t at = json.find(key);
    if (at == std::string::npos) return json;
    at = json.find(':', at + key.size());
    if (at == std::string::npos) return json;
    std::size_t first = at + 1;
    while (first < json.size() && (json[first] == ' ' || json[first] == '\t')) ++first;
    std::size_t last = first;
    while (last < json.size() && json[last] >= '0' && json[last] <= '9') ++last;
    if (last == first) return json;
    std::string patched = json;
    patched.replace(first, last - first, std::to_string(rate));
    return patched;
}

std::filesystem::path KnownFolder(const KNOWNFOLDERID& id) {
    wchar_t* raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw)) || raw == nullptr) {
        return {};
    }
    std::filesystem::path result(raw);
    CoTaskMemFree(raw);
    return result;
}

} // namespace

std::filesystem::path ResolveEngineDirectory() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    return std::filesystem::path(buffer).parent_path();
}

std::filesystem::path ResolveCrossoverConfigPath() {
    std::filesystem::path override = {};
    const wchar_t* raw = _wgetenv(L"SAL_CDSP_CONFIG");
    if (raw != nullptr && raw[0] != L'\0') {
        override = raw;
        std::error_code ignored;
        if (std::filesystem::exists(override, ignored)) return override;
        // An override that is not there is a decision, not a fallback: stay
        // bypassed rather than quietly loading some other file.
        return {};
    }

    const std::filesystem::path besideEngine = ResolveEngineDirectory() / L"dsp.json";
    std::error_code ignored;
    if (std::filesystem::exists(besideEngine, ignored)) return besideEngine;

    const std::filesystem::path programData = KnownFolder(FOLDERID_ProgramData);
    if (!programData.empty()) {
        const std::filesystem::path machineWide = programData / L"SpatialAudioLab" / L"dsp.json";
        if (std::filesystem::exists(machineWide, ignored)) return machineWide;
    }
    return {};
}

CrossoverStage::~CrossoverStage() {
    Reset();
    if (module_ != nullptr) FreeLibrary(static_cast<HMODULE>(module_));
    module_ = nullptr;
}

bool CrossoverStage::Load(const std::filesystem::path& directory,
                          const std::filesystem::path& configPath, const std::size_t sampleRate) {
    configPath_ = configPath;
    if (module_ == nullptr && !OpenLibrary(directory)) return false;
    return ApplyConfig(sampleRate);
}

bool CrossoverStage::OpenLibrary(const std::filesystem::path& directory) {
    const std::filesystem::path path = directory / kDllName;
    std::error_code ignored;
    if (!std::filesystem::exists(path, ignored)) {
        diagnostic_ = L"crossover: " + Text(path) + L" is not there, running straight through";
        return false;
    }
    const HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (module == nullptr) {
        diagnostic_ = L"crossover: LoadLibrary failed for " + Text(path) + L" (error " +
                      Number(GetLastError()) + L")";
        return false;
    }

    init_ = Resolve<InitFn>(module, "cdsp_init");
    process_ = Resolve<ProcessFn>(module, "cdsp_process");
    channelsIn_ = Resolve<CountFn>(module, "cdsp_channels_in");
    channelsOut_ = Resolve<CountFn>(module, "cdsp_channels_out");
    chunkSize_ = Resolve<CountFn>(module, "cdsp_chunksize");
    sampleRate_ = Resolve<CountFn>(module, "cdsp_samplerate");
    reset_ = Resolve<ResetFn>(module, "cdsp_reset");
    if (init_ == nullptr || process_ == nullptr || channelsIn_ == nullptr ||
        channelsOut_ == nullptr || chunkSize_ == nullptr) {
        diagnostic_ = L"crossover: " + Text(path) + L" does not export the expected ABI";
        FreeLibrary(module);
        return false;
    }
    module_ = module;
    return true;
}

bool CrossoverStage::ApplyConfig(const std::size_t sampleRate) {
    active_.store(false, std::memory_order_release);
    frames_ = 0;
    channels_ = 0;
    if (module_ == nullptr) return false;

    std::error_code ignored;
    if (configPath_.empty() || !std::filesystem::exists(configPath_, ignored)) {
        diagnostic_ = L"crossover: no config at " + Text(configPath_) +
                      L", running straight through";
        return false;
    }
    std::ifstream stream(configPath_, std::ios::binary);
    if (!stream) {
        diagnostic_ = L"crossover: cannot read " + Text(configPath_) +
                      L", running straight through";
        return false;
    }
    const std::string json((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());
    const std::size_t configuredRate = ReadSampleRate(json);
    const std::string effective = sampleRate != 0 ? WithSampleRate(json, sampleRate) : json;
    const int result = init_(effective.c_str());
    if (result != 0) {
        diagnostic_ = L"crossover: rejected " + Text(configPath_) + L" (error " +
                      Number(static_cast<std::size_t>(result)) + L"), running straight through";
        return false;
    }

    dspIn_ = static_cast<std::size_t>(std::max(0, channelsIn_()));
    dspOut_ = static_cast<std::size_t>(std::max(0, channelsOut_()));
    chunk_ = static_cast<std::size_t>(std::max(0, chunkSize_()));
    rate_ = sampleRate_ != nullptr ? static_cast<std::size_t>(std::max(0, sampleRate_())) : 0;
    if (sampleRate != 0 && rate_ != sampleRate) {
        // It kept its own rate, so every crossover sits at the wrong frequency.
        diagnostic_ = L"crossover: reported " + Number(rate_) + L" Hz while the stream runs at " +
                      Number(sampleRate) + L" Hz; bypassing";
        return false;
    }
    // Saying the config was retuned matters: a crossover designed for 48 kHz now
    // runs at 96 kHz, and the log is where somebody notices that.
    if (configuredRate != 0 && sampleRate != 0 && configuredRate != sampleRate) {
        diagnostic_ = L"crossover: " + Number(dspIn_) + L" in / " + Number(dspOut_) + L" out, " +
                      Number(chunk_) + L" frames per block, " + Number(rate_) +
                      L" Hz (config said " + Number(configuredRate) + L" Hz)";
    } else {
        diagnostic_ = L"crossover: " + Number(dspIn_) + L" in / " + Number(dspOut_) + L" out, " +
                      Number(chunk_) + L" frames per block, " + Number(rate_) + L" Hz";
    }
    return true;
}

bool CrossoverStage::Reload(const std::size_t sampleRate) {
    if (module_ == nullptr) return false;
    return ApplyConfig(sampleRate);
}

bool CrossoverStage::Configure(const std::size_t frames, const std::size_t channels,
                               const std::size_t sampleRate) {
    if (module_ == nullptr || chunk_ == 0) {
        active_.store(false, std::memory_order_release);
        return false;
    }
    if (sampleRate != 0 && rate_ != sampleRate) {
        diagnostic_ = L"crossover: " + Number(rate_) + L" Hz does not match the stream's " +
                      Number(sampleRate) + L" Hz; bypassing";
        active_.store(false, std::memory_order_release);
        return false;
    }
    // Both of these are hard constraints of cdsp itself: a block of a different
    // length, or a channel count its paths do not describe, is rejected by
    // cdsp_process anyway - doing it here keeps the audio path on one branch.
    //
    // SAL's producers hand over a variable number of frames, so the queue feeds
    // this stage in exact chunk-sized batches; `frames` here is the batch size,
    // not whatever one capture callback happened to carry.
    if (frames != chunk_) {
        diagnostic_ = L"crossover: block of " + Number(frames) +
                      L" frames does not match the configured chunk of " + Number(chunk_) +
                      L"; bypassing";
        active_.store(false, std::memory_order_release);
        return false;
    }
    if (dspIn_ != channels || dspOut_ != channels) {
        diagnostic_ = L"crossover: " + Number(dspIn_) + L"->" + Number(dspOut_) +
                      L" channels cannot feed a " + Number(channels) +
                      L"-channel stream; bypassing";
        active_.store(false, std::memory_order_release);
        return false;
    }
    frames_ = frames;
    channels_ = channels;
    diagnostic_ = L"crossover: active, " + Number(channels) + L" channels, " + Number(chunk_) +
                  L" frames per block, " + Number(rate_) + L" Hz";
    active_.store(true, std::memory_order_release);
    return true;
}

bool CrossoverStage::Process(const float* in, float* out, const std::size_t frames) noexcept {
    if (!active_.load(std::memory_order_acquire)) return false;
    if (in == nullptr || out == nullptr || frames != frames_) return false;
    return process_(in, out, static_cast<unsigned int>(frames)) == 0;
}

void CrossoverStage::Reset() noexcept {
    active_.store(false, std::memory_order_release);
    if (module_ != nullptr && reset_ != nullptr) reset_();
    frames_ = 0;
    channels_ = 0;
    chunk_ = 0;
    dspIn_ = 0;
    dspOut_ = 0;
}

} // namespace dolby
