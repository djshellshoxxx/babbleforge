#pragma once
// V2 reserved interfaces with V1 null / identity implementations
// (docs/V2_EXTENSION_POINTS.md §1, §1.1, §6). Header-only. Control / non-RT threads only.
// V1 never computes or displays SPL / dBA estimates: every level query returns nullopt.
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bf {

struct MaskStatistics;  // engine/MaskEngine.h

using Seconds = double;
using OutputOrZoneId = std::uint32_t;

struct CalibrationConfidence {
    enum Class { None, C_Generic, B_Uncalibrated, A_Calibrated } cls = None;
    std::string note;
};

// {gainDb, delayMs, polarity, eqKernel?}: identity by default.
struct OutputCalibration {
    double gainDb = 0.0;
    double delayMs = 0.0;
    bool polarityInvert = false;
    std::optional<std::vector<float>> eqKernel;
};

struct AmbientEstimate {
    double levelDbfs = -200.0;  // digital-referred (no SPL in V1)
    std::array<double, 26> thirdOctDb{};
};

struct LevelControllerLimits {
    double maxOffsetDb = 0.0;
    double maxSlewDbPerHour = 0.0;
};

using BandLevels = std::vector<double>;
struct SiiResult { double sii = 0.0; };
struct StiResult { double sti = 0.0; };
struct ImpulseResponse { std::vector<float> samples; double fs = 48000.0; };

class ILevelCalibration {
public:
    virtual ~ILevelCalibration() = default;
    virtual CalibrationConfidence confidence() const = 0;
    virtual std::optional<float> estimatedSplDbA(OutputOrZoneId, float digitalRmsDbfs) const = 0;
    virtual std::optional<float> digitalForTargetSplDbA(OutputOrZoneId, float targetDbA) const = 0;
};

class DigitalOnlyLevel final : public ILevelCalibration {
public:
    CalibrationConfidence confidence() const override { return {CalibrationConfidence::None, "uncalibrated: digital units only"}; }
    std::optional<float> estimatedSplDbA(OutputOrZoneId, float) const override { return std::nullopt; }
    std::optional<float> digitalForTargetSplDbA(OutputOrZoneId, float) const override { return std::nullopt; }
};

class ISpeakerCalibration {
public:
    virtual ~ISpeakerCalibration() = default;
    virtual OutputCalibration forOutput(std::uint8_t outputIndex) const = 0;
    virtual std::uint64_t revision() const = 0;  // bump -> Control re-publishes via RCU
};

class IdentitySpeakerCalibration final : public ISpeakerCalibration {
public:
    OutputCalibration forOutput(std::uint8_t) const override { return {}; }
    std::uint64_t revision() const override { return 0; }
};

class IAdaptiveLevelController {  // control thread, <= 1 Hz
public:
    virtual ~IAdaptiveLevelController() = default;
    // Additional gain offset (dB) added to Strength; V1 returns 0.
    virtual float updateGainOffsetDb(const AmbientEstimate*, const MaskStatistics&, Seconds dt) = 0;
    virtual LevelControllerLimits limits() const = 0;
};

class ConstantLevel final : public IAdaptiveLevelController {
public:
    float updateGainOffsetDb(const AmbientEstimate*, const MaskStatistics&, Seconds) override { return 0.0f; }
    LevelControllerLimits limits() const override { return {}; }
};

class IIntelligibilityEstimator {
public:
    virtual ~IIntelligibilityEstimator() = default;
    virtual std::optional<SiiResult> estimateSii(const BandLevels& speech, const BandLevels& masker,
                                                 const BandLevels* hearingThreshold) const = 0;
    virtual std::optional<StiResult> estimateSti(const ImpulseResponse&, const BandLevels& noise) const = 0;
};

class NullIntelligibilityEstimator final : public IIntelligibilityEstimator {
public:
    std::optional<SiiResult> estimateSii(const BandLevels&, const BandLevels&, const BandLevels*) const override { return std::nullopt; }
    std::optional<StiResult> estimateSti(const ImpulseResponse&, const BandLevels&) const override { return std::nullopt; }
};

// Measurement input: V1 opens no input device.
class IMeasurementInput {
public:
    virtual ~IMeasurementInput() = default;
    virtual bool available() const = 0;
    virtual std::string deviceName() const = 0;
};

class NullMeasurementInput final : public IMeasurementInput {
public:
    bool available() const override { return false; }
    std::string deviceName() const override { return {}; }
};

class IAmbientMonitor {
public:
    virtual ~IAmbientMonitor() = default;
    virtual std::optional<AmbientEstimate> current() const = 0;
};

class NullAmbientMonitor final : public IAmbientMonitor {
public:
    std::optional<AmbientEstimate> current() const override { return std::nullopt; }
};

}  // namespace bf
