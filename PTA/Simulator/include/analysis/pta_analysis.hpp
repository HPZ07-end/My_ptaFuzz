#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

#include <Eigen/Core>

#include "analysis/solver_diagnostics.hpp"
#include "analysis/solver_options.hpp"

enum class PtaMode{
    Disabled,   // normal NR
    Force,      // pta method is required explicitly
    Fallback    // pta called when NR fails
};

struct PtaAnalysisConfig{
    PtaMode mode = PtaMode::Disabled;
    // PTA combines local trust-region Newton steps with pseudo-time
    // continuation.  The trust region controls the nonlinear solve at each
    // trial pseudo-time point; derivative/DC-residual checks still decide
    // whether that converged trial is a steady state or an accepted advance.
    // Use a finite initial normalized radius: an automatic radius can inherit
    // an artificially large branch-current update from the first PTA stamp.
    // The non-monotone bounds remain unlimited because PTA's outer recovery
    // owns pseudo-time reduction and capacitance growth after a failed solve.
    NewtonSolverOptions newtonOptions = [] {
        NewtonSolverOptions options;
        options.maximumConsecutiveNonMonotoneSteps =
            std::numeric_limits<int>::max();
        options.maximumNonMonotoneResidualGrowth =
            std::numeric_limits<double>::max();
        options.trustRegionInitialRadius = 1.0e8;
        return options;
    }();

    // time control
    double initialStep = 1.0e-9;
    double minimumStep = 1.0e-15;
    double maximumStep = 1.0e3;
    int maximumSteps = 10000;

    // stability
    // Threshold for the dimensionless h*dx/dt convergence metric.
    double derivativeTolerance = 1.0;
    double derivativeRelativeTolerance = 1.0e-4;
    double derivativeVoltageAbsoluteTolerance = 1.0e-6;
    double derivativeCurrentAbsoluteTolerance = 1.0e-9;
    // Threshold for the dimensionless normalized DC residual metric.
    double dcResidualTolerance = 1.0;
    double dcResidualRelativeTolerance = 1.0e-4;
    double dcVoltageAbsoluteTolerance = 1.0e-6;
    double dcCurrentAbsoluteTolerance = 1.0e-9;

    // initial value & boundaries
    double initialNodeCapacitance = 1.0e-12;
    double minimumNodeCapacitance = 1.0e-18;
    double maximumNodeCapacitance = 1.0e-3;
    double currentSourceCapacitance = 1.0e-12;
    double voltageSourceInductance = 1.0e-9;

    // .pstran compatibility controls.  A positive compoundTimeConstant
    // selects CEPTA-style RVC/GVL pseudo-elements.  The 1-ohm/1-siemens
    // initial values match the resval=1 decks distributed with the benchmark
    // family; the public control card does not expose separate R0/G0 values.
    double compoundTimeConstant = 0.0;
    double compoundInitialResistance = 1.0;
    double compoundInitialConductance = 1.0;
    double sourceRampTime = 0.0;
    // Polarity-normalized MOS VGS seed used by the first Newton voltage
    // limiter step.  It does not impose a circuit-node initial condition.
    std::optional<double> initialMosVgs;
    std::optional<double> initialBjtVbe;

    // Adaptive rules
    double failedStepScale = 0.5;         // used when NR failed
    double successfulStepScale = 2.0;
    double capacitanceGrowScale = 2.0;
    double smallOscillationScale = 0.9;
    double mediumOscillationScale = 0.7;
    double heavyOscillationScale = 0.5;

    double mediumOscillationRatio = 0.5;
    double heavyOscillationRatio = 1.0;

    bool includeMosBulk = false;
    bool includeDiodes = false;

    void validate() const {
        switch(mode){
            case PtaMode::Disabled:
            case PtaMode::Force:
            case PtaMode::Fallback:
                break;
            default:
                throw std::invalid_argument("PTA mode is invalid");
        }

        if(!newtonOptions.valid()){
            throw std::invalid_argument(
                "PTA Newton solver configuration is invalid"
            );
        }

        const auto isPositiveFinite = [](double value) {
            return std::isfinite(value) && value > 0.0;
        };

        if(!isPositiveFinite(initialStep) ||
           !isPositiveFinite(minimumStep) ||
           !isPositiveFinite(maximumStep) ||
           minimumStep > initialStep ||
           initialStep > maximumStep){
            throw std::invalid_argument(
                "PTA time steps must satisfy 0 < minimum <= initial <= maximum"
            );
        }

        if(maximumSteps <= 0){
            throw std::invalid_argument(
                "PTA maximum step count must be positive"
            );
        }

        if(!isPositiveFinite(derivativeTolerance) ||
           !std::isfinite(derivativeRelativeTolerance) ||
           derivativeRelativeTolerance < 0.0 ||
           !isPositiveFinite(derivativeVoltageAbsoluteTolerance) ||
           !isPositiveFinite(derivativeCurrentAbsoluteTolerance)){
            throw std::invalid_argument(
                "PTA derivative convergence tolerances are invalid"
            );
        }

        if(!isPositiveFinite(dcResidualTolerance) ||
           !std::isfinite(dcResidualRelativeTolerance) ||
           dcResidualRelativeTolerance < 0.0 ||
           !isPositiveFinite(dcVoltageAbsoluteTolerance) ||
           !isPositiveFinite(dcCurrentAbsoluteTolerance)){
            throw std::invalid_argument(
                "PTA DC residual tolerances are invalid"
            );
        }

        if(!isPositiveFinite(initialNodeCapacitance) ||
           !isPositiveFinite(minimumNodeCapacitance) ||
           !isPositiveFinite(maximumNodeCapacitance) ||
           minimumNodeCapacitance > initialNodeCapacitance ||
           initialNodeCapacitance > maximumNodeCapacitance){
            throw std::invalid_argument(
                "PTA node capacitances must satisfy 0 < minimum <= initial <= maximum"
            );
        }

        if(!isPositiveFinite(currentSourceCapacitance) ||
           !isPositiveFinite(voltageSourceInductance)){
            throw std::invalid_argument(
                "PTA source pseudo-element values must be positive and finite"
            );
        }

        if(!std::isfinite(compoundTimeConstant) ||
           compoundTimeConstant < 0.0 ||
           !isPositiveFinite(compoundInitialResistance) ||
           !isPositiveFinite(compoundInitialConductance) ||
           !std::isfinite(sourceRampTime) || sourceRampTime < 0.0 ||
           (initialMosVgs && !std::isfinite(*initialMosVgs)) ||
           (initialBjtVbe && !std::isfinite(*initialBjtVbe))){
            throw std::invalid_argument(
                "PTA compound-element, source-ramp, and MOS/BJT initial-value controls are invalid"
            );
        }

        if(!std::isfinite(failedStepScale) ||
           failedStepScale <= 0.0 || failedStepScale >= 1.0){
            throw std::invalid_argument(
                "PTA failed-step scale must be finite and in (0, 1)"
            );
        }

        if(!std::isfinite(successfulStepScale) ||
           successfulStepScale <= 1.0){
            throw std::invalid_argument(
                "PTA successful-step scale must be finite and greater than 1"
            );
        }

        if(!std::isfinite(capacitanceGrowScale) ||
           capacitanceGrowScale <= 1.0){
            throw std::invalid_argument(
                "PTA capacitance growth scale must be finite and greater than 1"
            );
        }

        if(!std::isfinite(smallOscillationScale) ||
           !std::isfinite(mediumOscillationScale) ||
           !std::isfinite(heavyOscillationScale) ||
           heavyOscillationScale <= 0.0 ||
           heavyOscillationScale >= mediumOscillationScale ||
           mediumOscillationScale >= smallOscillationScale ||
           smallOscillationScale >= 1.0){
            throw std::invalid_argument(
                "PTA oscillation scales must satisfy 0 < heavy < medium < small < 1"
            );
        }

        if(!std::isfinite(mediumOscillationRatio) ||
           !std::isfinite(heavyOscillationRatio) ||
           mediumOscillationRatio <= 0.0 ||
           mediumOscillationRatio >= heavyOscillationRatio){
            throw std::invalid_argument(
                "PTA oscillation ratios must satisfy 0 < medium < heavy"
            );
        }
    }
};

// DPTA source ramp.  The cosine profile has zero slope at both endpoints and
// tauramp=0 deliberately means that ramping is disabled.
inline double ptaSourceRampScale(double time, double rampTime) noexcept {
    if(!std::isfinite(time) || time < 0.0 ||
       !std::isfinite(rampTime) || rampTime < 0.0){
        return std::numeric_limits<double>::quiet_NaN();
    }
    if(rampTime == 0.0 || time >= rampTime){
        return 1.0;
    }

    constexpr double pi = 3.141592653589793238462643383279502884;
    return 0.5 * (1.0 - std::cos(pi * time / rampTime));
}

struct PtaDerivativeEstimate {
    bool valid = false;
    double normalizedDerivative = 0.0;
};

struct PtaResidualEstimate {
    bool valid = false;
    double normalizedResidual = 0.0;
};

struct PtaDiagnostics {
    bool attempted = false;
    bool converged = false;
    bool hasConvergenceMetrics = false;
    int iterations = 0;
    int capacitanceGrowths = 0;
    int capacitanceReductions = 0;
    int minimumStepRecoveries = 0;
    double normalizedDerivative = 0.0;
    double normalizedDcResidual = 0.0;
    // The complete per-attempt trace is preserved for experiments and is
    // emitted by --pta-diagnostics as well as the solver report.
    std::vector<PtaStepAttemptDiagnostics> attempts;
};

inline PtaDerivativeEstimate estimatePtaNormalizedDerivative(
    const Eigen::VectorXd& derivative,
    const Eigen::VectorXd& currentSolution,
    const Eigen::VectorXd& previousSolution,
    int voltageUnknownCount,
    double timeStep,
    const PtaAnalysisConfig& config
){
    PtaDerivativeEstimate result;

    const Eigen::Index size = derivative.size();
    if(size == 0 ||
       currentSolution.size() != size ||
       previousSolution.size() != size ||
       voltageUnknownCount < 0 ||
       voltageUnknownCount > size ||
       !std::isfinite(timeStep) || timeStep <= 0.0 ||
       !std::isfinite(config.derivativeRelativeTolerance) ||
       config.derivativeRelativeTolerance < 0.0 ||
       !std::isfinite(config.derivativeVoltageAbsoluteTolerance) ||
       config.derivativeVoltageAbsoluteTolerance <= 0.0 ||
       !std::isfinite(config.derivativeCurrentAbsoluteTolerance) ||
       config.derivativeCurrentAbsoluteTolerance <= 0.0)
    {
        return result;
    }

    double normalizedDerivative = 0.0;

    for(Eigen::Index i = 0; i < size; ++i){
        const double derivativeValue = derivative[i];
        const double current = currentSolution[i];
        const double previous = previousSolution[i];
        if(!std::isfinite(derivativeValue) ||
           !std::isfinite(current) ||
           !std::isfinite(previous))
        {
            return result;
        }

        const double absoluteTolerance =
            i < voltageUnknownCount
            ? config.derivativeVoltageAbsoluteTolerance
            : config.derivativeCurrentAbsoluteTolerance;
        const double scale = std::max(std::abs(current), std::abs(previous));
        const double weight = absoluteTolerance +
            config.derivativeRelativeTolerance * scale;
        const double scaledDerivative =
            timeStep * std::abs(derivativeValue);

        if(!std::isfinite(weight) || weight <= 0.0 ||
           !std::isfinite(scaledDerivative))
        {
            return result;
        }

        const double componentDerivative = scaledDerivative / weight;
        if(!std::isfinite(componentDerivative)){
            return result;
        }

        normalizedDerivative = std::max(
            normalizedDerivative,
            componentDerivative
        );
    }

    result.valid = true;
    result.normalizedDerivative = normalizedDerivative;
    return result;
}

inline PtaResidualEstimate estimatePtaNormalizedResidual(
    const Eigen::VectorXd& residual,
    const Eigen::VectorXd& matrixProduct,
    const Eigen::VectorXd& rhs,
    int voltageUnknownCount,
    const PtaAnalysisConfig& config
){
    PtaResidualEstimate result;

    const Eigen::Index size = residual.size();
    if(size == 0 ||
       matrixProduct.size() != size ||
       rhs.size() != size ||
       voltageUnknownCount < 0 ||
       voltageUnknownCount > size ||
       !std::isfinite(config.dcResidualRelativeTolerance) ||
       config.dcResidualRelativeTolerance < 0.0 ||
       !std::isfinite(config.dcVoltageAbsoluteTolerance) ||
       config.dcVoltageAbsoluteTolerance <= 0.0 ||
       !std::isfinite(config.dcCurrentAbsoluteTolerance) ||
       config.dcCurrentAbsoluteTolerance <= 0.0)
    {
        return result;
    }

    double normalizedResidual = 0.0;

    for(Eigen::Index i = 0; i < size; ++i){
        const double residualValue = residual[i];
        const double matrixValue = matrixProduct[i];
        const double rhsValue = rhs[i];
        if(!std::isfinite(residualValue) ||
           !std::isfinite(matrixValue) ||
           !std::isfinite(rhsValue))
        {
            return result;
        }

        const double absoluteTolerance =
            i < voltageUnknownCount
            ? config.dcCurrentAbsoluteTolerance
            : config.dcVoltageAbsoluteTolerance;
        const double scale = std::max(std::abs(matrixValue), std::abs(rhsValue));
        const double weight = absoluteTolerance +
            config.dcResidualRelativeTolerance * scale;

        if(!std::isfinite(weight) || weight <= 0.0){
            return result;
        }

        const double componentResidual = std::abs(residualValue) / weight;
        if(!std::isfinite(componentResidual)){
            return result;
        }

        normalizedResidual = std::max(normalizedResidual, componentResidual);
    }

    result.valid = true;
    result.normalizedResidual = normalizedResidual;
    return result;
}
