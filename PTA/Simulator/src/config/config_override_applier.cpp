#include "config/overrides.hpp"

// Merge typed external overrides while preserving netlist-owned values.

#include <algorithm>
#include <stdexcept>

#include "analysis/pta_analysis.hpp"
#include "analysis/solver_options.hpp"
#include "analysis/transient_analysis.hpp"

namespace simulator::config {
namespace {

void applyNewtonOverrides(
    const NewtonOverrides& overrides,
    NewtonSolverOptions& options
){
    if(overrides.maximumIterations){
        options.maximumIterations = *overrides.maximumIterations;
    }
    if(overrides.tolerance){
        options.tolerance = *overrides.tolerance;
        // Keep the pre-normalization configuration key useful: when the
        // dedicated physical tolerances are absent, its value becomes the
        // absolute tolerance for both unknown dimensions.
        if(!overrides.voltageAbsoluteTolerance){
            options.voltageAbsoluteTolerance = *overrides.tolerance;
        }
        if(!overrides.currentAbsoluteTolerance){
            options.currentAbsoluteTolerance = *overrides.tolerance;
        }
    }
    if(overrides.relativeTolerance){
        options.relativeTolerance = *overrides.relativeTolerance;
    }
    if(overrides.voltageAbsoluteTolerance){
        options.voltageAbsoluteTolerance =
            *overrides.voltageAbsoluteTolerance;
    }
    if(overrides.currentAbsoluteTolerance){
        options.currentAbsoluteTolerance =
            *overrides.currentAbsoluteTolerance;
    }
    if(overrides.normalizedUpdateTolerance){
        options.normalizedUpdateTolerance =
            *overrides.normalizedUpdateTolerance;
    }
    if(overrides.normalizedResidualTolerance){
        options.normalizedResidualTolerance =
            *overrides.normalizedResidualTolerance;
    }
    if(overrides.maximumBacktracks){
        options.maximumBacktracks = *overrides.maximumBacktracks;
    }
    if(overrides.backtrackScale){
        options.backtrackScale = *overrides.backtrackScale;
    }
    if(overrides.sufficientDecrease){
        options.sufficientDecrease = *overrides.sufficientDecrease;
    }
    if(overrides.maximumSolutionStep){
        options.maximumSolutionStep = *overrides.maximumSolutionStep;
    }
    if(overrides.maximumConsecutiveNonMonotoneSteps){
        options.maximumConsecutiveNonMonotoneSteps =
            *overrides.maximumConsecutiveNonMonotoneSteps;
    }
    if(overrides.maximumNonMonotoneResidualGrowth){
        options.maximumNonMonotoneResidualGrowth =
            *overrides.maximumNonMonotoneResidualGrowth;
    }
    if(overrides.trustRegionEnabled){
        options.trustRegionEnabled = *overrides.trustRegionEnabled;
    }
    if(overrides.trustRegionInitialRadius){
        options.trustRegionInitialRadius = *overrides.trustRegionInitialRadius;
    }
    if(overrides.trustRegionMinimumRadius){
        options.trustRegionMinimumRadius = *overrides.trustRegionMinimumRadius;
    }
    if(overrides.trustRegionMaximumRadius){
        options.trustRegionMaximumRadius = *overrides.trustRegionMaximumRadius;
    }
    if(overrides.maximumTrustRegionRetries){
        options.maximumTrustRegionRetries =
            *overrides.maximumTrustRegionRetries;
    }
    if(overrides.trustRegionAcceptanceRatio){
        options.trustRegionAcceptanceRatio =
            *overrides.trustRegionAcceptanceRatio;
    }
    if(overrides.trustRegionShrinkThreshold){
        options.trustRegionShrinkThreshold =
            *overrides.trustRegionShrinkThreshold;
    }
    if(overrides.trustRegionGrowThreshold){
        options.trustRegionGrowThreshold =
            *overrides.trustRegionGrowThreshold;
    }
    if(overrides.trustRegionShrinkFactor){
        options.trustRegionShrinkFactor =
            *overrides.trustRegionShrinkFactor;
    }
    if(overrides.trustRegionGrowFactor){
        options.trustRegionGrowFactor =
            *overrides.trustRegionGrowFactor;
    }
    if(overrides.trustRegionBoundaryFraction){
        options.trustRegionBoundaryFraction =
            *overrides.trustRegionBoundaryFraction;
    }
}

void applySourceSteppingOverrides(
    const SourceSteppingOverrides& overrides,
    SourceSteppingOptions& options
){
    if(overrides.enabled){
        options.enabled = *overrides.enabled;
    }
    if(overrides.initialStep){
        options.initialStep = *overrides.initialStep;
    }
    if(overrides.maximumStep){
        options.maximumStep = *overrides.maximumStep;
    }
    if(overrides.minimumStep){
        options.minimumStep = *overrides.minimumStep;
    }
    if(overrides.growthFactor){
        options.growthFactor = *overrides.growthFactor;
    }
    if(overrides.failureScale){
        options.failureScale = *overrides.failureScale;
    }
}

PtaMode toPtaMode(PtaModeOverride mode){
    switch(mode){
        case PtaModeOverride::Disabled:
            return PtaMode::Disabled;
        case PtaModeOverride::Force:
            return PtaMode::Force;
        case PtaModeOverride::Fallback:
            return PtaMode::Fallback;
    }

    throw std::invalid_argument("PTA override mode is invalid");
}

void applyPtaOverrides(
    const PtaOverrides& overrides,
    PtaAnalysisConfig& config,
    bool pstranForcesPtaMode,
    const PtaParameterLocks& netlistLocks
){
    if(overrides.mode && !netlistLocks.mode){
        config.mode = toPtaMode(*overrides.mode);
    }
    if(overrides.newton){
        applyNewtonOverrides(*overrides.newton, config.newtonOptions);
    }

    if(overrides.initialStep && !netlistLocks.initialStep) {
        config.initialStep = *overrides.initialStep;
    }
    if(overrides.minimumStep && !netlistLocks.minimumStep) {
        config.minimumStep = *overrides.minimumStep;
    }
    if(overrides.maximumStep && !netlistLocks.maximumStep) {
        config.maximumStep = *overrides.maximumStep;
    }
    if(overrides.maximumSteps) config.maximumSteps = *overrides.maximumSteps;

    if(overrides.derivativeTolerance && !netlistLocks.derivativeTolerance){
        config.derivativeTolerance = *overrides.derivativeTolerance;
    }
    if(overrides.derivativeRelativeTolerance){
        config.derivativeRelativeTolerance =
            *overrides.derivativeRelativeTolerance;
    }
    if(overrides.derivativeVoltageAbsoluteTolerance){
        config.derivativeVoltageAbsoluteTolerance =
            *overrides.derivativeVoltageAbsoluteTolerance;
    }
    if(overrides.derivativeCurrentAbsoluteTolerance){
        config.derivativeCurrentAbsoluteTolerance =
            *overrides.derivativeCurrentAbsoluteTolerance;
    }

    if(overrides.dcResidualTolerance && !netlistLocks.dcResidualTolerance){
        config.dcResidualTolerance = *overrides.dcResidualTolerance;
    }
    if(overrides.dcResidualRelativeTolerance){
        config.dcResidualRelativeTolerance =
            *overrides.dcResidualRelativeTolerance;
    }
    if(overrides.dcVoltageAbsoluteTolerance){
        config.dcVoltageAbsoluteTolerance =
            *overrides.dcVoltageAbsoluteTolerance;
    }
    if(overrides.dcCurrentAbsoluteTolerance){
        config.dcCurrentAbsoluteTolerance =
            *overrides.dcCurrentAbsoluteTolerance;
    }

    if(overrides.initialNodeCapacitance){
        config.initialNodeCapacitance = *overrides.initialNodeCapacitance;
    }
    if(overrides.minimumNodeCapacitance){
        config.minimumNodeCapacitance = *overrides.minimumNodeCapacitance;
    }
    if(overrides.maximumNodeCapacitance){
        config.maximumNodeCapacitance = *overrides.maximumNodeCapacitance;
    }
    if(overrides.currentSourceCapacitance){
        config.currentSourceCapacitance = *overrides.currentSourceCapacitance;
    }
    if(overrides.voltageSourceInductance){
        config.voltageSourceInductance = *overrides.voltageSourceInductance;
    }

    if(overrides.compoundTimeConstant &&
       !netlistLocks.compoundTimeConstant){
        config.compoundTimeConstant = *overrides.compoundTimeConstant;
    }
    if(overrides.compoundInitialResistance){
        config.compoundInitialResistance = *overrides.compoundInitialResistance;
    }
    if(overrides.compoundInitialConductance){
        config.compoundInitialConductance = *overrides.compoundInitialConductance;
    }
    if(overrides.sourceRampTime && !netlistLocks.sourceRampTime){
        config.sourceRampTime = *overrides.sourceRampTime;
    }
    if(overrides.initialMosVgs.specified && !netlistLocks.initialMosVgs){
        config.initialMosVgs = overrides.initialMosVgs.value;
    }
    if(overrides.initialBjtVbe.specified && !netlistLocks.initialBjtVbe){
        config.initialBjtVbe = overrides.initialBjtVbe.value;
    }

    if(overrides.failedStepScale){
        config.failedStepScale = *overrides.failedStepScale;
    }
    if(overrides.successfulStepScale){
        config.successfulStepScale = *overrides.successfulStepScale;
    }
    if(overrides.capacitanceGrowScale){
        config.capacitanceGrowScale = *overrides.capacitanceGrowScale;
    }
    if(overrides.smallOscillationScale){
        config.smallOscillationScale = *overrides.smallOscillationScale;
    }
    if(overrides.mediumOscillationScale){
        config.mediumOscillationScale = *overrides.mediumOscillationScale;
    }
    if(overrides.heavyOscillationScale){
        config.heavyOscillationScale = *overrides.heavyOscillationScale;
    }
    if(overrides.mediumOscillationRatio){
        config.mediumOscillationRatio = *overrides.mediumOscillationRatio;
    }
    if(overrides.heavyOscillationRatio){
        config.heavyOscillationRatio = *overrides.heavyOscillationRatio;
    }

    if(overrides.includeMosBulk){
        config.includeMosBulk = *overrides.includeMosBulk;
    }
    if(overrides.includeDiodes){
        config.includeDiodes = *overrides.includeDiodes;
    }

    if(pstranForcesPtaMode){
        config.mode = PtaMode::Force;
    }
}

void applyTransientSolverOverrides(
    const TransientSolverOverrides& overrides,
    TransientSolverOptions& options
){
    if(overrides.newton){
        applyNewtonOverrides(*overrides.newton, options.newtonOptions);
    }
    if(overrides.relativeTolerance){
        options.relativeTolerance = *overrides.relativeTolerance;
    }
    if(overrides.voltageAbsoluteTolerance){
        options.voltageAbsoluteTolerance =
            *overrides.voltageAbsoluteTolerance;
    }
    if(overrides.currentAbsoluteTolerance){
        options.currentAbsoluteTolerance =
            *overrides.currentAbsoluteTolerance;
    }
    if(overrides.minimumStep){
        options.minimumStep = *overrides.minimumStep;
    }
    if(overrides.safetyFactor){
        options.safetyFactor = *overrides.safetyFactor;
    }
    if(overrides.minimumScale){
        options.minimumScale = *overrides.minimumScale;
    }
    if(overrides.maximumScale){
        options.maximumScale = *overrides.maximumScale;
    }
    if(overrides.convergenceFailureScale){
        options.convergenceFailureScale = *overrides.convergenceFailureScale;
    }
    if(overrides.maximumRejects){
        options.maximumRejects = *overrides.maximumRejects;
    }
}

void applyTransientOverrides(
    const TransientOverrides& overrides,
    std::optional<TransientAnalysisConfig>& transient,
    const TransientParameterLocks& netlistLocks
){
    if(overrides.enabled && !*overrides.enabled && !netlistLocks.enabled){
        transient.reset();
        return;
    }

    if(!transient){
        transient.emplace();
    }

    TransientAnalysisConfig& config = *transient;
    if(overrides.outputInterval && !netlistLocks.outputInterval){
        config.outputInterval = *overrides.outputInterval;
    }
    if(overrides.stopTime && !netlistLocks.stopTime){
        config.stopTime = *overrides.stopTime;
    }
    if(overrides.outputStartTime && !netlistLocks.outputStartTime){
        config.outputStartTime = *overrides.outputStartTime;
    }
    if(overrides.maximumStep && !netlistLocks.maximumStep){
        config.maximumStep = *overrides.maximumStep;
    }
    if(overrides.useInitialConditions && !netlistLocks.useInitialConditions){
        config.useInitialConditions = *overrides.useInitialConditions;
    }
    if(overrides.solver){
        applyTransientSolverOverrides(*overrides.solver, config.solverOptions);
    }
}

}  // namespace

NetlistAnalysisParameterLocks parameterLocksFor(const AnalysisPlan& plan){
    NetlistAnalysisParameterLocks locks;

    if(plan.transient){
        locks.transient.enabled = true;
        locks.transient.outputInterval = true;
        locks.transient.stopTime = true;

        if(plan.transientNetlistParameters){
            const auto& presence = *plan.transientNetlistParameters;
            locks.transient.outputStartTime = presence.outputStartTime;
            locks.transient.maximumStep = presence.maximumStep;
            locks.transient.useInitialConditions =
                presence.useInitialConditions;
        }
        if(plan.delmax){
            locks.transient.maximumStep = true;
        }
    }

    if(plan.pseudoTransient){
        const auto& pstran = *plan.pseudoTransient;
        locks.pta.mode = true;
        locks.pta.initialStep = pstran.initialStepSpecified;
        locks.pta.minimumStep = pstran.minimumStepSpecified;
        locks.pta.maximumStep = pstran.maximumStepSpecified ||
            plan.delmax.has_value();
        locks.pta.derivativeTolerance = pstran.convergenceValueSpecified;
        locks.pta.dcResidualTolerance = pstran.convergenceValueSpecified;
        locks.pta.compoundTimeConstant = pstran.tauSpecified;
        locks.pta.sourceRampTime = pstran.tauRampSpecified;
        locks.pta.initialMosVgs = pstran.kvgs0Specified;
        locks.pta.initialBjtVbe = pstran.vbe0Specified;
    }

    return locks;
}

void applyConfigOverrides(
    const ConfigOverrides& overrides,
    OperatingPointSolverOptions& operatingPoint,
    PtaAnalysisConfig& pta,
    std::optional<TransientAnalysisConfig>& transient,
    bool pstranForcesPtaMode,
    const NetlistAnalysisParameterLocks& netlistLocks
){
    if(overrides.operatingPoint){
        if(overrides.operatingPoint->newton){
            applyNewtonOverrides(
                *overrides.operatingPoint->newton,
                operatingPoint.newton
            );
        }
        if(overrides.operatingPoint->sourceStepping){
            applySourceSteppingOverrides(
                *overrides.operatingPoint->sourceStepping,
                operatingPoint.sourceStepping
            );
        }
    }

    if(overrides.pta){
        applyPtaOverrides(
            *overrides.pta,
            pta,
            pstranForcesPtaMode,
            netlistLocks.pta
        );
    }

    if(overrides.transient){
        applyTransientOverrides(
            *overrides.transient,
            transient,
            netlistLocks.transient
        );
    }
}

}  // namespace simulator::config
