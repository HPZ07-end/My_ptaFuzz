#include "config/overrides.hpp"

// Parse and apply individual command-line name=value overrides.

#include <algorithm>
#include <limits>
#include <stdexcept>

#include "netlist/spice_syntax.hpp"

namespace simulator::config {
namespace {

bool splitAssignment(
    const std::string& assignment,
    std::string& key,
    std::string& value,
    std::string& error
){
    const std::size_t equals = assignment.find('=');
    if(equals == std::string::npos || equals == 0 ||
       equals + 1 == assignment.size()){
        error = "expected name=value";
        return false;
    }

    key = to_lower_copy(assignment.substr(0, equals));
    std::replace(key.begin(), key.end(), '_', '-');
    value = assignment.substr(equals + 1);
    return true;
}

bool readDouble(
    const std::string& value,
    double& target,
    std::string& error
){
    try {
        target = parse_spice_number(value);
        return true;
    } catch(const std::runtime_error& exception) {
        error = exception.what();
        return false;
    }
}

bool readInteger(
    const std::string& value,
    int minimum,
    int& target,
    std::string& error
){
    try {
        std::size_t parsedLength = 0;
        const long long parsed = std::stoll(value, &parsedLength, 10);
        if(parsedLength != value.size() || parsed < minimum ||
           parsed > std::numeric_limits<int>::max()){
            error = minimum > 0
                ? "expected a positive integer"
                : "expected a non-negative integer";
            return false;
        }
        target = static_cast<int>(parsed);
        return true;
    } catch(const std::exception&) {
        error = minimum > 0
            ? "expected a positive integer"
            : "expected a non-negative integer";
        return false;
    }
}

bool readBoolean(
    const std::string& value,
    bool& target,
    std::string& error
){
    const std::string normalized = to_lower_copy(value);
    if(normalized == "true" || normalized == "1"){
        target = true;
        return true;
    }
    if(normalized == "false" || normalized == "0"){
        target = false;
        return true;
    }

    error = "expected true, false, 1, or 0";
    return false;
}

bool readLegacyNewtonTolerance(
    const std::string& value,
    NewtonSolverOptions& options,
    std::string& error
){
    double tolerance = 0.0;
    if(!readDouble(value, tolerance, error)){
        return false;
    }
    options.tolerance = tolerance;
    options.voltageAbsoluteTolerance = tolerance;
    options.currentAbsoluteTolerance = tolerance;
    return true;
}

bool applyTrustRegionOption(
    const std::string& key,
    const std::string& value,
    const std::string& prefix,
    NewtonSolverOptions& options,
    std::string& error,
    bool& recognized
){
    recognized = true;
    if(key == prefix + "trust-region-enabled"){
        return readBoolean(value, options.trustRegionEnabled, error);
    }
    if(key == prefix + "trust-region-initial-radius"){
        return readDouble(value, options.trustRegionInitialRadius, error);
    }
    if(key == prefix + "trust-region-minimum-radius"){
        return readDouble(value, options.trustRegionMinimumRadius, error);
    }
    if(key == prefix + "trust-region-maximum-radius"){
        return readDouble(value, options.trustRegionMaximumRadius, error);
    }
    if(key == prefix + "maximum-trust-region-retries"){
        return readInteger(value, 0, options.maximumTrustRegionRetries, error);
    }
    if(key == prefix + "trust-region-acceptance-ratio"){
        return readDouble(value, options.trustRegionAcceptanceRatio, error);
    }
    if(key == prefix + "trust-region-shrink-threshold"){
        return readDouble(value, options.trustRegionShrinkThreshold, error);
    }
    if(key == prefix + "trust-region-grow-threshold"){
        return readDouble(value, options.trustRegionGrowThreshold, error);
    }
    if(key == prefix + "trust-region-shrink-factor"){
        return readDouble(value, options.trustRegionShrinkFactor, error);
    }
    if(key == prefix + "trust-region-grow-factor"){
        return readDouble(value, options.trustRegionGrowFactor, error);
    }
    if(key == prefix + "trust-region-boundary-fraction"){
        return readDouble(value, options.trustRegionBoundaryFraction, error);
    }
    recognized = false;
    return false;
}

TransientAnalysisConfig& requireTransientConfig(
    std::optional<TransientAnalysisConfig>& options,
    const std::optional<TransientAnalysisConfig>& baseOptions
){
    if(!options){
        options = baseOptions.value_or(TransientAnalysisConfig{});
    }
    return *options;
}

bool isPtaOptionLocked(
    const std::string& key,
    const PtaParameterLocks& netlistLocks
){
    return (key == "initial-step" && netlistLocks.initialStep) ||
        (key == "minimum-step" && netlistLocks.minimumStep) ||
        (key == "maximum-step" && netlistLocks.maximumStep) ||
        (key == "derivative-tolerance" &&
         netlistLocks.derivativeTolerance) ||
        (key == "dc-residual-tolerance" &&
         netlistLocks.dcResidualTolerance) ||
        (key == "compound-time-constant" &&
         netlistLocks.compoundTimeConstant) ||
        (key == "source-ramp-time" && netlistLocks.sourceRampTime) ||
        (key == "initial-mos-vgs" && netlistLocks.initialMosVgs) ||
        (key == "initial-bjt-vbe" && netlistLocks.initialBjtVbe);
}

bool isTransientOptionLocked(
    const std::string& key,
    const TransientParameterLocks& netlistLocks
){
    return (key == "enabled" && netlistLocks.enabled) ||
        (key == "output-interval" && netlistLocks.outputInterval) ||
        (key == "stop-time" && netlistLocks.stopTime) ||
        (key == "output-start-time" && netlistLocks.outputStartTime) ||
        (key == "maximum-step" && netlistLocks.maximumStep) ||
        (key == "use-initial-conditions" &&
         netlistLocks.useInitialConditions);
}

}  // namespace

bool applyOperatingPointOption(
    const std::string& assignment,
    OperatingPointSolverOptions& options,
    std::string& key,
    std::string& error
){
    std::string value;
    if(!splitAssignment(assignment, key, value, error)){
        return false;
    }

    if(key == "newton.maximum-iterations"){
        return readInteger(value, 1, options.newton.maximumIterations, error);
    }
    if(key == "newton.tolerance"){
        return readLegacyNewtonTolerance(value, options.newton, error);
    }
    if(key == "newton.relative-tolerance"){
        return readDouble(value, options.newton.relativeTolerance, error);
    }
    if(key == "newton.voltage-absolute-tolerance"){
        return readDouble(
            value,
            options.newton.voltageAbsoluteTolerance,
            error
        );
    }
    if(key == "newton.current-absolute-tolerance"){
        return readDouble(
            value,
            options.newton.currentAbsoluteTolerance,
            error
        );
    }
    if(key == "newton.normalized-update-tolerance"){
        return readDouble(
            value,
            options.newton.normalizedUpdateTolerance,
            error
        );
    }
    if(key == "newton.normalized-residual-tolerance"){
        return readDouble(
            value,
            options.newton.normalizedResidualTolerance,
            error
        );
    }
    if(key == "newton.maximum-backtracks"){
        return readInteger(value, 0, options.newton.maximumBacktracks, error);
    }
    if(key == "newton.backtrack-scale"){
        return readDouble(value, options.newton.backtrackScale, error);
    }
    if(key == "newton.sufficient-decrease"){
        return readDouble(value, options.newton.sufficientDecrease, error);
    }
    if(key == "newton.maximum-solution-step"){
        return readDouble(value, options.newton.maximumSolutionStep, error);
    }
    if(key == "newton.maximum-consecutive-non-monotone-steps"){
        return readInteger(
            value,
            0,
            options.newton.maximumConsecutiveNonMonotoneSteps,
            error
        );
    }
    if(key == "newton.maximum-non-monotone-residual-growth"){
        return readDouble(
            value,
            options.newton.maximumNonMonotoneResidualGrowth,
            error
        );
    }
    bool trustRegionRecognized = false;
    const bool trustRegionApplied = applyTrustRegionOption(
        key,
        value,
        "newton.",
        options.newton,
        error,
        trustRegionRecognized
    );
    if(trustRegionRecognized){
        return trustRegionApplied;
    }
    if(key == "source-stepping.enabled"){
        return readBoolean(value, options.sourceStepping.enabled, error);
    }
    if(key == "source-stepping.initial-step"){
        return readDouble(value, options.sourceStepping.initialStep, error);
    }
    if(key == "source-stepping.maximum-step"){
        return readDouble(value, options.sourceStepping.maximumStep, error);
    }
    if(key == "source-stepping.minimum-step"){
        return readDouble(value, options.sourceStepping.minimumStep, error);
    }
    if(key == "source-stepping.growth-factor"){
        return readDouble(value, options.sourceStepping.growthFactor, error);
    }
    if(key == "source-stepping.failure-scale"){
        return readDouble(value, options.sourceStepping.failureScale, error);
    }

    error = "unknown operating-point option";
    return false;
}

bool applyPtaOption(
    const std::string& assignment,
    PtaAnalysisConfig& options,
    std::string& key,
    std::string& error,
    const PtaParameterLocks& netlistLocks
){
    std::string value;
    if(!splitAssignment(assignment, key, value, error)){
        return false;
    }
    if(isPtaOptionLocked(key, netlistLocks)){
        return true;
    }

    if(key == "newton.maximum-iterations"){
        return readInteger(value, 1, options.newtonOptions.maximumIterations,
                           error);
    }
    if(key == "newton.tolerance"){
        return readLegacyNewtonTolerance(value, options.newtonOptions, error);
    }
    if(key == "newton.relative-tolerance"){
        return readDouble(
            value,
            options.newtonOptions.relativeTolerance,
            error
        );
    }
    if(key == "newton.voltage-absolute-tolerance"){
        return readDouble(
            value,
            options.newtonOptions.voltageAbsoluteTolerance,
            error
        );
    }
    if(key == "newton.current-absolute-tolerance"){
        return readDouble(
            value,
            options.newtonOptions.currentAbsoluteTolerance,
            error
        );
    }
    if(key == "newton.normalized-update-tolerance"){
        return readDouble(
            value,
            options.newtonOptions.normalizedUpdateTolerance,
            error
        );
    }
    if(key == "newton.normalized-residual-tolerance"){
        return readDouble(
            value,
            options.newtonOptions.normalizedResidualTolerance,
            error
        );
    }
    if(key == "newton.maximum-backtracks"){
        return readInteger(
            value,
            0,
            options.newtonOptions.maximumBacktracks,
            error
        );
    }
    if(key == "newton.backtrack-scale"){
        return readDouble(value, options.newtonOptions.backtrackScale, error);
    }
    if(key == "newton.sufficient-decrease"){
        return readDouble(value, options.newtonOptions.sufficientDecrease, error);
    }
    if(key == "newton.maximum-solution-step"){
        return readDouble(value, options.newtonOptions.maximumSolutionStep,
                          error);
    }
    if(key == "newton.maximum-consecutive-non-monotone-steps"){
        return readInteger(
            value,
            0,
            options.newtonOptions.maximumConsecutiveNonMonotoneSteps,
            error
        );
    }
    if(key == "newton.maximum-non-monotone-residual-growth"){
        return readDouble(
            value,
            options.newtonOptions.maximumNonMonotoneResidualGrowth,
            error
        );
    }
    bool trustRegionRecognized = false;
    const bool trustRegionApplied = applyTrustRegionOption(
        key,
        value,
        "newton.",
        options.newtonOptions,
        error,
        trustRegionRecognized
    );
    if(trustRegionRecognized){
        return trustRegionApplied;
    }
    if(key == "maximum-steps"){
        return readInteger(value, 1, options.maximumSteps, error);
    }

    const auto setDouble = [&](double& target) {
        return readDouble(value, target, error);
    };
    const auto setBoolean = [&](bool& target) {
        return readBoolean(value, target, error);
    };

    if(key == "initial-step") return setDouble(options.initialStep);
    if(key == "minimum-step") return setDouble(options.minimumStep);
    if(key == "maximum-step") return setDouble(options.maximumStep);
    if(key == "derivative-tolerance") return setDouble(options.derivativeTolerance);
    if(key == "derivative-relative-tolerance") {
        return setDouble(options.derivativeRelativeTolerance);
    }
    if(key == "derivative-voltage-absolute-tolerance") {
        return setDouble(options.derivativeVoltageAbsoluteTolerance);
    }
    if(key == "derivative-current-absolute-tolerance") {
        return setDouble(options.derivativeCurrentAbsoluteTolerance);
    }
    if(key == "dc-residual-tolerance") return setDouble(options.dcResidualTolerance);
    if(key == "dc-residual-relative-tolerance") {
        return setDouble(options.dcResidualRelativeTolerance);
    }
    if(key == "dc-voltage-absolute-tolerance") {
        return setDouble(options.dcVoltageAbsoluteTolerance);
    }
    if(key == "dc-current-absolute-tolerance") {
        return setDouble(options.dcCurrentAbsoluteTolerance);
    }
    if(key == "initial-node-capacitance") {
        return setDouble(options.initialNodeCapacitance);
    }
    if(key == "minimum-node-capacitance") {
        return setDouble(options.minimumNodeCapacitance);
    }
    if(key == "maximum-node-capacitance") {
        return setDouble(options.maximumNodeCapacitance);
    }
    if(key == "current-source-capacitance") {
        return setDouble(options.currentSourceCapacitance);
    }
    if(key == "voltage-source-inductance") {
        return setDouble(options.voltageSourceInductance);
    }
    if(key == "compound-time-constant") {
        return setDouble(options.compoundTimeConstant);
    }
    if(key == "compound-initial-resistance") {
        return setDouble(options.compoundInitialResistance);
    }
    if(key == "compound-initial-conductance") {
        return setDouble(options.compoundInitialConductance);
    }
    if(key == "source-ramp-time") return setDouble(options.sourceRampTime);
    if(key == "initial-mos-vgs"){
        if(to_lower_copy(value) == "null"){
            options.initialMosVgs.reset();
            return true;
        }
        double parsed = 0.0;
        if(!readDouble(value, parsed, error)){
            return false;
        }
        options.initialMosVgs = parsed;
        return true;
    }
    if(key == "initial-bjt-vbe"){
        if(to_lower_copy(value) == "null"){
            options.initialBjtVbe.reset();
            return true;
        }
        double parsed = 0.0;
        if(!readDouble(value, parsed, error)){
            return false;
        }
        options.initialBjtVbe = parsed;
        return true;
    }
    if(key == "failed-step-scale") return setDouble(options.failedStepScale);
    if(key == "successful-step-scale") {
        return setDouble(options.successfulStepScale);
    }
    if(key == "capacitance-grow-scale") {
        return setDouble(options.capacitanceGrowScale);
    }
    if(key == "small-oscillation-scale") {
        return setDouble(options.smallOscillationScale);
    }
    if(key == "medium-oscillation-scale") {
        return setDouble(options.mediumOscillationScale);
    }
    if(key == "heavy-oscillation-scale") {
        return setDouble(options.heavyOscillationScale);
    }
    if(key == "medium-oscillation-ratio") {
        return setDouble(options.mediumOscillationRatio);
    }
    if(key == "heavy-oscillation-ratio") {
        return setDouble(options.heavyOscillationRatio);
    }
    if(key == "include-mos-bulk") return setBoolean(options.includeMosBulk);
    if(key == "include-diodes") return setBoolean(options.includeDiodes);

    error = "unknown PTA option";
    return false;
}

bool applyTransientOption(
    const std::string& assignment,
    std::optional<TransientAnalysisConfig>& options,
    std::string& key,
    std::string& error,
    const std::optional<TransientAnalysisConfig>& baseOptions,
    std::optional<double> hardMaximumStep,
    const TransientParameterLocks& netlistLocks
){
    std::string value;
    if(!splitAssignment(assignment, key, value, error)){
        return false;
    }
    if(isTransientOptionLocked(key, netlistLocks)){
        return true;
    }

    if(key == "enabled"){
        bool enabled = false;
        if(!readBoolean(value, enabled, error)){
            return false;
        }
        if(!enabled){
            options.reset();
        } else {
            requireTransientConfig(options, baseOptions);
        }
        return true;
    }

    TransientAnalysisConfig& config = requireTransientConfig(
        options,
        baseOptions
    );
    if(key == "output-interval"){
        return readDouble(value, config.outputInterval, error);
    }
    if(key == "stop-time"){
        return readDouble(value, config.stopTime, error);
    }
    if(key == "output-start-time"){
        return readDouble(value, config.outputStartTime, error);
    }
    if(key == "maximum-step"){
        double maximumStep = 0.0;
        if(!readDouble(value, maximumStep, error)){
            return false;
        }
        config.maximumStep = hardMaximumStep
            ? std::min(*hardMaximumStep, maximumStep)
            : maximumStep;
        return true;
    }
    if(key == "use-initial-conditions"){
        return readBoolean(value, config.useInitialConditions, error);
    }
    if(key == "solver.newton.maximum-iterations"){
        return readInteger(
            value,
            1,
            config.solverOptions.newtonOptions.maximumIterations,
            error
        );
    }
    if(key == "solver.newton.tolerance"){
        return readLegacyNewtonTolerance(
            value,
            config.solverOptions.newtonOptions,
            error
        );
    }
    if(key == "solver.newton.relative-tolerance"){
        return readDouble(
            value,
            config.solverOptions.newtonOptions.relativeTolerance,
            error
        );
    }
    if(key == "solver.newton.voltage-absolute-tolerance"){
        return readDouble(
            value,
            config.solverOptions.newtonOptions.voltageAbsoluteTolerance,
            error
        );
    }
    if(key == "solver.newton.current-absolute-tolerance"){
        return readDouble(
            value,
            config.solverOptions.newtonOptions.currentAbsoluteTolerance,
            error
        );
    }
    if(key == "solver.newton.normalized-update-tolerance"){
        return readDouble(
            value,
            config.solverOptions.newtonOptions.normalizedUpdateTolerance,
            error
        );
    }
    if(key == "solver.newton.normalized-residual-tolerance"){
        return readDouble(
            value,
            config.solverOptions.newtonOptions.normalizedResidualTolerance,
            error
        );
    }
    if(key == "solver.newton.maximum-backtracks"){
        return readInteger(
            value,
            0,
            config.solverOptions.newtonOptions.maximumBacktracks,
            error
        );
    }
    if(key == "solver.newton.backtrack-scale"){
        return readDouble(
            value,
            config.solverOptions.newtonOptions.backtrackScale,
            error
        );
    }
    if(key == "solver.newton.sufficient-decrease"){
        return readDouble(
            value,
            config.solverOptions.newtonOptions.sufficientDecrease,
            error
        );
    }
    if(key == "solver.newton.maximum-solution-step"){
        return readDouble(
            value,
            config.solverOptions.newtonOptions.maximumSolutionStep,
            error
        );
    }
    if(key == "solver.newton.maximum-consecutive-non-monotone-steps"){
        return readInteger(
            value,
            0,
            config.solverOptions.newtonOptions
                .maximumConsecutiveNonMonotoneSteps,
            error
        );
    }
    if(key == "solver.newton.maximum-non-monotone-residual-growth"){
        return readDouble(
            value,
            config.solverOptions.newtonOptions
                .maximumNonMonotoneResidualGrowth,
            error
        );
    }
    bool trustRegionRecognized = false;
    const bool trustRegionApplied = applyTrustRegionOption(
        key,
        value,
        "solver.newton.",
        config.solverOptions.newtonOptions,
        error,
        trustRegionRecognized
    );
    if(trustRegionRecognized){
        return trustRegionApplied;
    }
    if(key == "solver.relative-tolerance"){
        return readDouble(value, config.solverOptions.relativeTolerance, error);
    }
    if(key == "solver.voltage-absolute-tolerance"){
        return readDouble(
            value,
            config.solverOptions.voltageAbsoluteTolerance,
            error
        );
    }
    if(key == "solver.current-absolute-tolerance"){
        return readDouble(
            value,
            config.solverOptions.currentAbsoluteTolerance,
            error
        );
    }
    if(key == "solver.minimum-step"){
        return readDouble(value, config.solverOptions.minimumStep, error);
    }
    if(key == "solver.safety-factor"){
        return readDouble(value, config.solverOptions.safetyFactor, error);
    }
    if(key == "solver.minimum-scale"){
        return readDouble(value, config.solverOptions.minimumScale, error);
    }
    if(key == "solver.maximum-scale"){
        return readDouble(value, config.solverOptions.maximumScale, error);
    }
    if(key == "solver.convergence-failure-scale"){
        return readDouble(
            value,
            config.solverOptions.convergenceFailureScale,
            error
        );
    }
    if(key == "solver.maximum-rejects"){
        return readInteger(value, 0, config.solverOptions.maximumRejects, error);
    }

    error = "unknown transient option";
    return false;
}

}  // namespace simulator::config
