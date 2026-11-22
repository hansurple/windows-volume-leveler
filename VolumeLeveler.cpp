#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <limits>
#include <new>

// Binary interface used by VST 2.4 hosts. No SDK is required to build the DLL.
struct Effect;
using Dispatcher = intptr_t (__cdecl *)(Effect*, int32_t, int32_t, intptr_t, void*, float);
using Process = void (__cdecl *)(Effect*, float**, float**, int32_t);
using SetParameter = void (__cdecl *)(Effect*, int32_t, float);
using GetParameter = float (__cdecl *)(Effect*, int32_t);
using ProcessDouble = void (__cdecl *)(Effect*, double**, double**, int32_t);

#pragma pack(push, 8)
struct Effect {
    int32_t magic;
    Dispatcher dispatch;
    Process process;
    SetParameter setParameter;
    GetParameter getParameter;
    int32_t programs, parameters, inputs, outputs, flags;
    intptr_t reserved1, reserved2;
    int32_t delay, realQualities, offQualities;
    float ioRatio;
    void* object;
    void* user;
    int32_t uniqueId, version;
    Process processReplacing;
    ProcessDouble processDouble;
    char reserved[56];
};
#pragma pack(pop)
static_assert(sizeof(Effect) == 192 && offsetof(Effect, processReplacing) == 120,
              "The playback plugin targets x64 VST hosts.");

namespace {
constexpr int Channels = 8;
constexpr int Capacity = 2049;
constexpr int EnabledBit = 128;
constexpr int QuietestTarget = 32;
constexpr int LoudestTarget = 16;
constexpr int DefaultTarget = 24;
constexpr float PeakCeiling = 0.707945784f; // -3 dBFS sample peak
constexpr int EffectMagic = 0x56737450;
constexpr int CanReplace = 1 << 4;

enum Opcode {
    Close = 1, GetProgramName = 5, SetSampleRate = 10, MainsChanged = 12,
    GetCategory = 35, GetEffectName = 45, GetVendor = 47, GetProduct = 48,
    GetVendorVersion = 49, GetVstVersion = 58
};

HMODULE moduleHandle;

class SharedSettings {
public:
    ~SharedSettings() {
        if (value) UnmapViewOfFile(const_cast<const LONG*>(value));
    }

    void Open() {
        if (value) return;
        wchar_t path[1024];
        const DWORD length = GetModuleFileNameW(moduleHandle, path, 1024);
        if (!length || length >= 1024) return;
        wchar_t* filename = std::wcsrchr(path, L'\\');
        if (!filename || (filename - path) + 24 >= 1024) return;
        std::wcscpy(filename + 1, L"VolumeLeveler.settings");
        const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER size{};
        if (GetFileSizeEx(file, &size) && size.QuadPart == sizeof(LONG)) {
            const HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
            if (mapping) {
                value = static_cast<const volatile LONG*>(
                    MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(LONG)));
                CloseHandle(mapping);
            }
        }
        CloseHandle(file);
    }

    LONG Read() const { return value ? *value : DefaultTarget; }

private:
    const volatile LONG* value = nullptr;
};

class AutomaticGain {
public:
    void Prepare(float rate) {
        framesPerBlock = static_cast<int>(std::lround(rate * BlockSeconds));
        framesToBypass = static_cast<int>(std::lround(rate * 0.650));
        detectLouder = Blend(BlockSeconds, 0.160);
        detectQuieter = Blend(BlockSeconds, 0.550);
        reduceGain = Blend(1.0 / rate, 0.100);
        raiseGain = Blend(1.0 / rate, 0.450);
        bypassBlend = Blend(1.0 / rate, 0.040);
        enabled = false;
        bypassFrames = 0;
        appliedGain = 1.0;
        ResetDetector();
    }

    void SetControls(bool active, int target) {
        targetDb = -target;
        if (active == enabled) return;
        enabled = active;
        bypassFrames = active ? 0 : framesToBypass;
        ResetDetector();
    }

    double ProcessFrame(const float (&samples)[Channels]) {
        if (enabled) {
            for (int channel = 0; channel < Channels; ++channel) {
                channelPower[channel] += static_cast<double>(samples[channel]) * samples[channel];
            }
            if (++blockFrames == framesPerBlock) UpdateTarget();
            const double blend = goalGain < appliedGain ? reduceGain : raiseGain;
            appliedGain += (goalGain - appliedGain) * blend;
        } else if (bypassFrames > 0) {
            appliedGain += (1.0 - appliedGain) * bypassBlend;
            if (--bypassFrames == 0) appliedGain = 1.0;
        }
        return appliedGain;
    }

    bool ProtectPeak() const { return enabled || bypassFrames > 0; }

private:
    static constexpr double BlockSeconds = 0.025;
    static constexpr double QuietGate = -50.0;
    static constexpr double FullBoostLevel = -42.0;

    static double Blend(double interval, double timeConstant) {
        return 1.0 - std::exp(-interval / timeConstant);
    }

    void ResetDetector() {
        std::fill_n(channelPower, Channels, 0.0);
        blockFrames = quietBlocks = 0;
        hasLevel = false;
        goalGain = 1.0;
    }

    void UpdateTarget() {
        // The strongest channel anchors the level and one gain preserves the stereo balance.
        const double meanPower = *std::max_element(channelPower, channelPower + Channels) / blockFrames;
        const double measured = std::sqrt(std::max(meanPower, 1e-12));
        std::fill_n(channelPower, Channels, 0.0);
        blockFrames = 0;

        if (measured < std::pow(10.0, QuietGate / 20.0)) {
            quietBlocks = std::min(quietBlocks + 1, 60);
            // Drop boost during silence, but carry attenuation across pauses in speech.
            if (quietBlocks >= 40 || (quietBlocks >= 6 && goalGain > 1.0)) goalGain = 1.0;
            if (quietBlocks == 60) hasLevel = false;
            return;
        }

        quietBlocks = 0;
        if (hasLevel) {
            const double blend = measured > trackedAmplitude ? detectLouder : detectQuieter;
            trackedAmplitude += (measured - trackedAmplitude) * blend;
        } else {
            trackedAmplitude = measured;
            hasLevel = true;
        }

        // A small dead band preserves natural variations around the target.
        const double trackedDb = 20.0 * std::log10(trackedAmplitude);
        const double difference = targetDb - trackedDb;
        double gainDb = std::copysign(std::max(0.0, std::abs(difference) - 2.0), difference);
        gainDb = std::clamp(gainDb, -24.0, 15.0);
        if (gainDb > 0.0) {
            gainDb *= std::clamp((trackedDb - QuietGate) / (FullBoostLevel - QuietGate), 0.0, 1.0);
        }
        goalGain = std::pow(10.0, gainDb / 20.0);
    }

    double channelPower[Channels]{};
    int framesPerBlock = 0, blockFrames = 0, quietBlocks = 0;
    int framesToBypass = 0, bypassFrames = 0;
    double targetDb = -DefaultTarget, trackedAmplitude = 0;
    double detectLouder = 0, detectQuieter = 0, reduceGain = 0, raiseGain = 0, bypassBlend = 0;
    double goalGain = 1, appliedGain = 1;
    bool hasLevel = false, enabled = false;
};

class Limiter {
public:
    void Prepare(float rate) {
        if (!std::isfinite(rate) || rate < 8000 || rate > 384000) rate = 48000;
        leveler.Prepare(rate);
        lookahead = static_cast<int>(std::lround(rate * 0.005));
        window = lookahead + 1;
        release = 1.0 - std::exp(-1.0 / (rate * 0.080));
        std::memset(audio, 0, sizeof(audio));
        std::fill_n(targetHistory, window, 1.0);
        targetSum = window;
        currentGain = 1.0f;
        peakHead = peakCount = writeFrame = 0;
        frameIndex = 0;
    }

    int Delay() const { return lookahead; }

    void ProcessAudio(float** input, float** output, int frames, bool enabled, int targetLevel) {
        leveler.SetControls(enabled, targetLevel);
        for (int frame = 0; frame < frames; ++frame) {
            const int readFrame = (writeFrame + 1) % window;
            const bool protectPeak = leveler.ProtectPeak();
            float samples[Channels];
            for (int channel = 0; channel < Channels; ++channel) {
                const float sample = input[channel][frame];
                samples[channel] = std::isfinite(sample) ? sample : 0.0f;
            }
            const double gain = leveler.ProcessFrame(samples);
            float peak = 0;
            float delayedPeak = 0;
            for (int channel = 0; channel < Channels; ++channel) {
                constexpr double MaxSample = std::numeric_limits<float>::max();
                audio[writeFrame][channel] = static_cast<float>(
                    std::clamp(samples[channel] * gain, -MaxSample, MaxSample));
                peak = std::max(peak, std::fabs(audio[writeFrame][channel]));
                delayedPeak = std::max(delayedPeak, std::fabs(audio[readFrame][channel]));
            }

            const float windowPeak = UpdateWindowPeak(peak);
            const float target = protectPeak && windowPeak > PeakCeiling ? PeakCeiling / windowPeak : 1.0f;
            // Every target in this average covers the sample leaving the delay line.
            // Averaging the targets gives a gradual attack while bounding that sample.
            targetSum += target - targetHistory[writeFrame];
            targetHistory[writeFrame] = target;
            const double attack = targetSum / window;
            currentGain = std::min(attack, currentGain + (1.0 - currentGain) * release);
            if (currentGain > 0.999999 && attack >= 0.9999999) currentGain = 1.0;
            // Enabling peak protection also bounds audio already in the delay line.
            if (protectPeak && delayedPeak > PeakCeiling) {
                currentGain = std::min(currentGain, static_cast<double>(PeakCeiling) / delayedPeak);
            }
            for (int channel = 0; channel < Channels; ++channel) {
                output[channel][frame] = static_cast<float>(audio[readFrame][channel] * currentGain);
            }
            writeFrame = readFrame;
            ++frameIndex;
        }
    }

private:
    struct Peak { uint64_t frame; float magnitude; };

    float UpdateWindowPeak(float peak) {
        if (peakCount && frameIndex - peaks[peakHead].frame >= static_cast<unsigned>(window)) {
            peakHead = (peakHead + 1) % Capacity;
            --peakCount;
        }
        while (peakCount && peaks[(peakHead + peakCount - 1) % Capacity].magnitude <= peak) {
            --peakCount;
        }
        peaks[(peakHead + peakCount) % Capacity] = {frameIndex, peak};
        ++peakCount;
        return peaks[peakHead].magnitude;
    }

    float audio[Capacity][Channels]{};
    double targetHistory[Capacity]{};
    Peak peaks[Capacity]{};
    int lookahead = 0, window = 0, writeFrame = 0, peakHead = 0, peakCount = 0;
    uint64_t frameIndex = 0;
    double targetSum = 0;
    double currentGain = 1, release = 0;
    AutomaticGain leveler;
};

struct Plugin {
    Effect effect{};
    SharedSettings settings;
    Limiter limiter;
    float sampleRate = 48000;
};

intptr_t __cdecl Dispatch(Effect* effect, int32_t opcode, int32_t, intptr_t value,
                          void* data, float option) {
    auto* plugin = static_cast<Plugin*>(effect->object);
    switch (opcode) {
    case Close: delete plugin; return 1;
    case SetSampleRate:
        plugin->sampleRate = option;
        plugin->limiter.Prepare(option);
        effect->delay = plugin->limiter.Delay();
        return 1;
    case MainsChanged:
        if (value) {
            plugin->settings.Open();
            plugin->limiter.Prepare(plugin->sampleRate);
        }
        return 1;
    case GetCategory: return 1;
    case GetProgramName: if (data) std::strcpy(static_cast<char*>(data), "Default"); return 1;
    case GetEffectName:
    case GetProduct: if (data) std::strcpy(static_cast<char*>(data), "Volume Leveler"); return 1;
    case GetVendor: if (data) std::strcpy(static_cast<char*>(data), "Volume Leveler"); return 1;
    case GetVendorVersion: return 2000;
    case GetVstVersion: return 2400;
    default: return 0;
    }
}

void __cdecl ProcessAudio(Effect* effect, float** input, float** output, int32_t frames) {
    auto* plugin = static_cast<Plugin*>(effect->object);
    const LONG settings = plugin->settings.Read();
    const int target = settings & 127;
    const bool enabled = settings == (EnabledBit | target) &&
                         target >= LoudestTarget && target <= QuietestTarget;
    plugin->limiter.ProcessAudio(input, output, frames, enabled, target);
}
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) moduleHandle = instance;
    return TRUE;
}

extern "C" __declspec(dllexport) Effect* __cdecl VSTPluginMain(Dispatcher host) {
    if (!host || host(nullptr, 1, 0, 0, nullptr, 0) == 0) return nullptr;
    auto* plugin = new (std::nothrow) Plugin;
    if (!plugin) return nullptr;
    plugin->settings.Open();
    plugin->limiter.Prepare(plugin->sampleRate);
    Effect& effect = plugin->effect;
    effect.magic = EffectMagic;
    effect.dispatch = Dispatch;
    effect.process = ProcessAudio;
    effect.processReplacing = ProcessAudio;
    effect.programs = 1;
    effect.inputs = effect.outputs = Channels;
    effect.flags = CanReplace;
    effect.delay = plugin->limiter.Delay();
    effect.object = plugin;
    effect.uniqueId = 0x564C7632;
    effect.version = 2000;
    return &effect;
}
