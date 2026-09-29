#pragma once

#include <optional>
#include <string>

#include "analysis/analysis_plan.hpp"
#include "analysis/pta_analysis.hpp"
#include "analysis/solver_options.hpp"
#include "analysis/transient_analysis.hpp"

namespace simulator::config {
struct LoadedConfig;

// Values explicitly supplied by a netlist remain authoritative over external
// configuration layers.
struct PtaParameterLocks {
    bool mode = false;
    bool initialStep = false;
    bool minimumStep = false;
    bool maximumStep = false;
    bool derivativeTolerance = false;
    bool dcResidualTolerance = false;
    bool compoundTimeConstant = false;
    bool sourceRampTime = false;
    bool initialMosVgs = false;
    bool initialBjtVbe = false;
};

struct TransientParameterLocks {
    bool enabled = false;
    bool outputInterval = false;
    bool stopTime = false;
    bool outputStartTime = false;
    bool maximumStep = false;
    bool useInitialConditions = false;
};

struct NetlistAnalysisParameterLocks {
    PtaParameterLocks pta;
    TransientParameterLocks transient;
};

enum class PtaModeOverride {
    Disabled,
    Force,
    Fallback
};

template<class T>
struct NullableOverride {
    bool specified {false};
    std::optional<T> value;
};

struct NewtonOverrides {
    std::optional<int> maximumIterations;
    std::optional<double> tolerance;
    std::optional<double> relativeTolerance;
    std::optional<double> voltageAbsoluteTolerance;
    std::optional<double> currentAbsoluteTolerance;
    std::optional<double> normalizedUpdateTolerance;
    std::optional<double> normalizedResidualTolerance;
    std::optional<int> maximumBacktracks;
    std::optional<double> backtrackScale;
    std::optional<double> sufficientDecrease;
    std::optional<double> maximumSolutionStep;
    std::optional<int> maximumConsecutiveNonMonotoneSteps;
    std::optional<double> maximumNonMonotoneResidualGrowth;
    std::optional<bool> trustRegionEnabled;
    std::optional<double> trustRegionInitialRadius;
    std::optional<double> trustRegionMinimumRadius;
    std::optional<double> trustRegionMaximumRadius;
    std::optional<int> maximumTrustRegionRetries;
    std::optional<double> trustRegionAcceptanceRatio;
    std::optional<double> trustRegionShrinkThreshold;
    std::optional<double> trustRegionGrowThreshold;
    std::optional<double> trustRegionShrinkFactor;
    std::optional<double> trustRegionGrowFactor;
    std::optional<double> trustRegionBoundaryFraction;
};

struct SourceSteppingOverrides {
    std::optional<bool> enabled;
    std::optional<double> initialStep;
    std::optional<double> maximumStep;
    std::optional<double> minimumStep;
    std::optional<double> growthFactor;
    std::optional<double> failureScale;
};

struct OperatingPointOverrides {
    std::optional<NewtonOverrides> newton;
    std::optional<SourceSteppingOverrides> sourceStepping;
};

struct PtaOverrides {
    std::optional<PtaModeOverride> mode;
    std::optional<NewtonOverrides> newton;

    std::optional<double> initialStep;
    std::optional<double> minimumStep;
    std::optional<double> maximumStep;
    std::optional<int> maximumSteps;

    std::optional<double> derivativeTolerance;
    std::optional<double> derivativeRelativeTolerance;
    std::optional<double> derivativeVoltageAbsoluteTolerance;
    std::optional<double> derivativeCurrentAbsoluteTolerance;

    std::optional<double> dcResidualTolerance;
    std::optional<double> dcResidualRelativeTolerance;
    std::optional<double> dcVoltageAbsoluteTolerance;
    std::optional<double> dcCurrentAbsoluteTolerance;

    std::optional<double> initialNodeCapacitance;
    std::optional<double> minimumNodeCapacitance;
    std::optional<double> maximumNodeCapacitance;
    std::optional<double> currentSourceCapacitance;
    std::optional<double> voltageSourceInductance;

    std::optional<double> compoundTimeConstant;
    std::optional<double> compoundInitialResistance;
    std::optional<double> compoundInitialConductance;
    std::optional<double> sourceRampTime;
    NullableOverride<double> initialMosVgs;
    NullableOverride<double> initialBjtVbe;

    std::optional<double> failedStepScale;
    std::optional<double> successfulStepScale;
    std::optional<double> capacitanceGrowScale;
    std::optional<double> smallOscillationScale;
    std::optional<double> mediumOscillationScale;
    std::optional<double> heavyOscillationScale;
    std::optional<double> mediumOscillationRatio;
    std::optional<double> heavyOscillationRatio;

    std::optional<bool> includeMosBulk;
    std::optional<bool> includeDiodes;
};

struct TransientSolverOverrides {
    std::optional<NewtonOverrides> newton;

    std::optional<double> relativeTolerance;
    std::optional<double> voltageAbsoluteTolerance;
    std::optional<double> currentAbsoluteTolerance;
    std::optional<double> minimumStep;

    std::optional<double> safetyFactor;
    std::optional<double> minimumScale;
    std::optional<double> maximumScale;
    std::optional<double> convergenceFailureScale;
    std::optional<int> maximumRejects;
};

struct TransientOverrides {
    std::optional<bool> enabled;
    std::optional<double> outputInterval;
    std::optional<double> stopTime;
    std::optional<double> outputStartTime;
    std::optional<double> maximumStep;
    std::optional<bool> useInitialConditions;
    std::optional<TransientSolverOverrides> solver;
};

struct ConfigOverrides {
    std::optional<int> schemaVersion;
    std::optional<bool> debug;
    std::optional<OperatingPointOverrides> operatingPoint;
    std::optional<PtaOverrides> pta;
    std::optional<TransientOverrides> transient;
};

/**
 * 将已加载的 json 文档严格解析为覆盖层。
 *
 * 该函数不修改求解器运行期配置或 AnalysisPlan。
 * 找不到配置文件时，返回完全为空的 ConfigOverrides。
 *
 * json schema、字段类型、未知字段和数值格式错误时 throw std::runtime_error。
 */
[[nodiscard]] ConfigOverrides parseConfigOverrides(
    const LoadedConfig& loadedConfig
);

[[nodiscard]] NetlistAnalysisParameterLocks parameterLocksFor(
    const AnalysisPlan& plan
);

void applyConfigOverrides(
    const ConfigOverrides& overrides,
    OperatingPointSolverOptions& operatingPoint,
    PtaAnalysisConfig& pta,
    std::optional<TransientAnalysisConfig>& transient,
    bool pstranForcesPtaMode,
    const NetlistAnalysisParameterLocks& netlistLocks = {}
);

bool applyOperatingPointOption(
    const std::string& assignment,
    OperatingPointSolverOptions& options,
    std::string& key,
    std::string& error
);

bool applyPtaOption(
    const std::string& assignment,
    PtaAnalysisConfig& options,
    std::string& key,
    std::string& error,
    const PtaParameterLocks& netlistLocks = {}
);

bool applyTransientOption(
    const std::string& assignment,
    std::optional<TransientAnalysisConfig>& options,
    std::string& key,
    std::string& error,
    const std::optional<TransientAnalysisConfig>& baseOptions = std::nullopt,
    std::optional<double> hardMaximumStep = std::nullopt,
    const TransientParameterLocks& netlistLocks = {}
);

}  // namespace simulator::config
