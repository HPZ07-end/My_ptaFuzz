#include "circuit/circuit.hpp"

#include <chrono>
#include <cmath>
#include <ctime>
#include <limits>
#include <string>

#include "analysis/solver_options.hpp"
#include "analysis/transient_analysis.hpp"
#include "circuit/node_map.hpp"
#include "devices/device.hpp"
#include "devices/pseudo_device.hpp"
#include "solver/newton_convergence.hpp"
#include "solver/mna.hpp"
#include "solver/newton_step.hpp"

namespace {
using SteadyClock = std::chrono::steady_clock;

double elapsedWallSeconds(SteadyClock::time_point start){
    return std::chrono::duration<double>(SteadyClock::now() - start).count();
}
}

bool Circuit::solveLinearSystem(const AssembleCallback& assemble,
                                NewtonSolveDiagnostics& stats){
    stats = {};
    stats.attempted = true;
    stats.maximumIterations = 1;
    const std::clock_t startClock = std::clock();
    const SteadyClock::time_point startWall = SteadyClock::now();
    const auto finish = [&stats, startClock, startWall](
        bool converged,
        const std::string& failureReason = std::string{}
    ) {
        stats.converged = converged;
        stats.cpuSeconds =
            double(std::clock() - startClock) / CLOCKS_PER_SEC;
        stats.wallSeconds = elapsedWallSeconds(startWall);
        stats.failureReason = converged ? std::string{} : failureReason;
        return converged;
    };

    assemble();
    stats.iterations = 1;

    if(!mna_->solve()){
        return finish(
            false,
            "sparse linear system factorization or solve failed"
        );
    }
    if(!mna_->solution().allFinite()){
        return finish(false, "sparse linear solution contains a non-finite value");
    }

    stats.finalDelta = 0.0;
    return finish(true);
}

bool Circuit::solveNewtonSystem(const AssembleCallback& assemble,
                                NewtonSolveDiagnostics& stats,
                                const NewtonSolverOptions& options){
    stats = {};
    stats.attempted = true;
    stats.usedNewtonRaphson = true;
    stats.maximumIterations = options.maximumIterations;
    stats.tolerance = options.tolerance;

    const std::clock_t startClock = std::clock();
    const SteadyClock::time_point startWall = SteadyClock::now();
    const auto finish = [&stats, startClock, startWall](
        bool converged,
        const std::string& failureReason = std::string{}
    ) {
        stats.converged = converged;
        stats.cpuSeconds =
            double(std::clock() - startClock) / CLOCKS_PER_SEC;
        stats.wallSeconds = elapsedWallSeconds(startWall);
        stats.failureReason = converged ? std::string{} : failureReason;
        return converged;
    };

    if(!options.valid()){
        return finish(false, "Newton-Raphson configuration is invalid");
    }

    Eigen::VectorXd previous = mna_->solution();
    Eigen::VectorXd matrixProduct;
    Eigen::VectorXd residual;
    const int voltageUnknownCount = nodeMap_->nodeCount();
    int consecutiveNonMonotoneFallbacks = 0;
    bool trustRegionInitialized = false;
    double trustRegionRadius = 0.0;

    const auto normalizedResidual = [this,
                                     &matrixProduct,
                                     &residual,
                                     voltageUnknownCount,
                                     &options](double& value) {
        if(!mna_->evaluateResidual(matrixProduct, residual)){
            return false;
        }
        const NewtonResidualEstimate estimate =
            estimateNormalizedNewtonResidual(
                residual,
                matrixProduct,
                mna_->rhs(),
                voltageUnknownCount,
                options.relativeTolerance,
                options.voltageAbsoluteTolerance,
                options.currentAbsoluteTolerance
            );
        if(!estimate.valid){
            return false;
        }
        value = estimate.normalizedResidual;
        return true;
    };

    for(int iter = 0; iter < options.maximumIterations; ++iter){
        stats.iterations = iter + 1;
        assemble();

        double baselineResidual = 0.0;
        if(!normalizedResidual(baselineResidual)){
            return finish(
                false,
                "Newton-Raphson nonlinear residual contains a non-finite value"
            );
        }
        // Device limiters are stateful.  Every backtracking candidate must be
        // stamped from precisely the same previous-iterate state.
        saveNonlinearLineSearchStates();

        if(!mna_->solve()){
            return finish(
                false,
                "sparse linear system factorization or solve failed during "
                "Newton-Raphson iteration"
            );
        }

        Eigen::VectorXd& current = mna_->solution();
        const NewtonStepResult step = limitNewtonStep(
            current,
            previous,
            options.maximumSolutionStep
        );
        if(!std::isfinite(step.delta)){
            return finish(
                false,
                "Newton-Raphson update is non-finite"
            );
        }
        const Eigen::VectorXd direction = current - previous;
        const Eigen::VectorXd baselineMatrixProduct = matrixProduct;
        const Eigen::VectorXd baselineResidualVector = residual;
        const Eigen::VectorXd baselineRhs = mna_->rhs();
        double stepScale = 1.0;
        bool accepted = false;
        double candidateResidual = 0.0;

        if(options.trustRegionEnabled){
            stats.usedTrustRegion = true;
            const NewtonUpdateEstimate fullStepEstimate =
                estimateNormalizedNewtonUpdate(
                    previous + direction,
                    previous,
                    voltageUnknownCount,
                    options.relativeTolerance,
                    options.voltageAbsoluteTolerance,
                    options.currentAbsoluteTolerance
                );
            if(!fullStepEstimate.valid ||
               !mna_->multiplyMatrix(
                   direction,
                   matrixProduct
               ))
            {
                return finish(
                    false,
                    "Newton-Raphson trust-region model is non-finite"
                );
            }
            const Eigen::VectorXd linearDirectionProduct = matrixProduct;

            if(!trustRegionInitialized){
                trustRegionRadius = options.trustRegionInitialRadius > 0.0
                    ? options.trustRegionInitialRadius
                    : std::clamp(
                        fullStepEstimate.normalizedUpdate,
                        options.trustRegionMinimumRadius,
                        options.trustRegionMaximumRadius
                    );
                trustRegionInitialized = true;
                stats.initialTrustRegionRadius = trustRegionRadius;
            }

            const auto reduceTrustRegion = [&] {
                const double previousRadius = trustRegionRadius;
                trustRegionRadius = std::max(
                    options.trustRegionMinimumRadius,
                    trustRegionRadius * options.trustRegionShrinkFactor
                );
                if(trustRegionRadius < previousRadius){
                    ++stats.trustRegionRadiusReductions;
                    return true;
                }
                return false;
            };

            for(int trial = 0;
                trial <= options.maximumTrustRegionRetries;
                ++trial)
            {
                stepScale = 1.0;
                NewtonUpdateEstimate stepEstimate;
                for(int projection = 0; projection < 8; ++projection){
                    current = previous + stepScale * direction;
                    stepEstimate = estimateNormalizedNewtonUpdate(
                        current,
                        previous,
                        voltageUnknownCount,
                        options.relativeTolerance,
                        options.voltageAbsoluteTolerance,
                        options.currentAbsoluteTolerance
                    );
                    if(!stepEstimate.valid){
                        return finish(
                            false,
                            "Newton-Raphson trust-region step is non-finite"
                        );
                    }
                    if(stepEstimate.normalizedUpdate <= trustRegionRadius){
                        break;
                    }
                    stepScale *= trustRegionRadius /
                        stepEstimate.normalizedUpdate;
                }
                if(!stepEstimate.valid || stepScale <= 0.0 ||
                   !std::isfinite(stepScale))
                {
                    return finish(
                        false,
                        "Newton-Raphson trust-region step is invalid"
                    );
                }
                if(stepEstimate.normalizedUpdate > trustRegionRadius){
                    ++stats.trustRegionRejectedSteps;
                    if(!reduceTrustRegion()){
                        break;
                    }
                    continue;
                }

                const Eigen::VectorXd predictedMatrixProduct =
                    baselineMatrixProduct + stepScale * linearDirectionProduct;
                const Eigen::VectorXd predictedResidualVector =
                    baselineResidualVector + stepScale * linearDirectionProduct;
                const NewtonResidualEstimate predictedEstimate =
                    estimateNormalizedNewtonResidual(
                        predictedResidualVector,
                        predictedMatrixProduct,
                        baselineRhs,
                        voltageUnknownCount,
                        options.relativeTolerance,
                        options.voltageAbsoluteTolerance,
                        options.currentAbsoluteTolerance
                    );
                const double predictedReduction = predictedEstimate.valid
                    ? baselineResidual - predictedEstimate.normalizedResidual
                    : 0.0;
                const bool hasPredictedReduction =
                    std::isfinite(predictedReduction) &&
                    predictedReduction > 0.0;

                current = previous + stepScale * direction;
                restoreNonlinearLineSearchStates();
                assemble();
                ++stats.trustRegionTrials;
                if(!normalizedResidual(candidateResidual)){
                    return finish(
                        false,
                        "Newton-Raphson nonlinear residual contains a non-finite value"
                    );
                }

                double agreementRatio = 0.0;
                if(hasPredictedReduction){
                    // The agreement ratio must compare the linear prediction
                    // and the nonlinear residual with identical row weights.
                    // Convergence itself still uses the freshly stamped
                    // candidate normalization in candidateResidual.
                    const NewtonResidualEstimate actualAgreementEstimate =
                        estimateNormalizedNewtonResidual(
                            residual,
                            baselineMatrixProduct,
                            baselineRhs,
                            voltageUnknownCount,
                            options.relativeTolerance,
                            options.voltageAbsoluteTolerance,
                            options.currentAbsoluteTolerance
                        );
                    if(!actualAgreementEstimate.valid){
                        return finish(
                            false,
                            "Newton-Raphson trust-region residual model is invalid"
                        );
                    }
                    agreementRatio =
                        (baselineResidual -
                         actualAgreementEstimate.normalizedResidual) /
                        predictedReduction;
                    if(!std::isfinite(agreementRatio)){
                        return finish(
                            false,
                            "Newton-Raphson trust-region agreement ratio is non-finite"
                        );
                    }
                    stats.hasTrustRegionRatio = true;
                    stats.lastTrustRegionRatio = agreementRatio;
                }
                if(candidateResidual < options.normalizedResidualTolerance ||
                   (hasPredictedReduction &&
                    agreementRatio >= options.trustRegionAcceptanceRatio))
                {
                    accepted = true;
                    if(hasPredictedReduction &&
                       agreementRatio < options.trustRegionShrinkThreshold)
                    {
                        reduceTrustRegion();
                    }else if(hasPredictedReduction &&
                             agreementRatio > options.trustRegionGrowThreshold &&
                             stepEstimate.normalizedUpdate >=
                                 options.trustRegionBoundaryFraction *
                                     trustRegionRadius)
                    {
                        const double previousRadius = trustRegionRadius;
                        trustRegionRadius = std::min(
                            options.trustRegionMaximumRadius,
                            trustRegionRadius * options.trustRegionGrowFactor
                        );
                        if(trustRegionRadius > previousRadius){
                            ++stats.trustRegionRadiusExpansions;
                        }
                    }
                    break;
                }

                ++stats.trustRegionRejectedSteps;
                if(!reduceTrustRegion()){
                    break;
                }
            }

            stats.finalTrustRegionRadius = trustRegionRadius;
            if(!accepted){
                current = previous;
                restoreNonlinearLineSearchStates();
                stats.trustRegionRetriesExhausted = true;
                return finish(
                    false,
                    "Newton-Raphson trust-region retries exhausted"
                );
            }
            consecutiveNonMonotoneFallbacks = 0;
        }else{
            for(int backtrack = 0;
                backtrack <= options.maximumBacktracks;
                ++backtrack)
            {
                current = previous + stepScale * direction;
                restoreNonlinearLineSearchStates();
                assemble();
                ++stats.lineSearchEvaluations;
                if(!normalizedResidual(candidateResidual)){
                    return finish(
                        false,
                        "Newton-Raphson nonlinear residual contains a non-finite value"
                    );
                }

                const double requiredResidual = baselineResidual *
                    (1.0 - options.sufficientDecrease * stepScale);
                if(candidateResidual < options.normalizedResidualTolerance ||
                   candidateResidual <= requiredResidual){
                    accepted = true;
                    break;
                }

                if(backtrack == options.maximumBacktracks){
                    break;
                }
                stepScale *= options.backtrackScale;
                ++stats.backtrackingSteps;
            }

            if(!accepted){
                if(consecutiveNonMonotoneFallbacks >=
                       options.maximumConsecutiveNonMonotoneSteps)
                {
                    current = previous;
                    restoreNonlinearLineSearchStates();
                    return finish(
                        false,
                        "Newton-Raphson residual line search exhausted its "
                        "backtracking limit and the controlled non-monotone "
                        "step limit"
                    );
                }

                // This retained path is selected when trustRegionEnabled is
                // disabled.  PTA's outer pseudo-time continuation still
                // independently checks derivative and DC residual before
                // accepting a time step.
                current = previous + direction;
                stepScale = 1.0;
                restoreNonlinearLineSearchStates();
                assemble();
                ++stats.lineSearchEvaluations;
                if(!normalizedResidual(candidateResidual)){
                    return finish(
                        false,
                        "Newton-Raphson nonlinear residual contains a non-finite value"
                    );
                }
                const bool hasResidualGrowthBound =
                    options.maximumNonMonotoneResidualGrowth <
                        std::numeric_limits<double>::max();
                const double maximumResidual = !hasResidualGrowthBound ||
                        baselineResidual > std::numeric_limits<double>::max() /
                            options.maximumNonMonotoneResidualGrowth
                    ? std::numeric_limits<double>::max()
                    : baselineResidual *
                        options.maximumNonMonotoneResidualGrowth;
                if(hasResidualGrowthBound && candidateResidual > maximumResidual){
                    current = previous;
                    restoreNonlinearLineSearchStates();
                    return finish(
                        false,
                        "Newton-Raphson residual line search exhausted its "
                        "backtracking limit and the controlled non-monotone "
                        "residual-growth bound"
                    );
                }
                accepted = true;
                ++stats.nonMonotoneStepFallbacks;
                ++consecutiveNonMonotoneFallbacks;
            }else{
                consecutiveNonMonotoneFallbacks = 0;
            }
        }

        if(step.limited || stepScale < 1.0){
            ++stats.dampedSteps;
        }
        stats.finalStepScale = stepScale;
        const NewtonUpdateEstimate updateEstimate =
            estimateNormalizedNewtonUpdate(
                current,
                previous,
                voltageUnknownCount,
                options.relativeTolerance,
                options.voltageAbsoluteTolerance,
                options.currentAbsoluteTolerance
            );
        if(!updateEstimate.valid){
            return finish(
                false,
                "Newton-Raphson normalized update estimate is invalid"
            );
        }
        stats.finalDelta = (current - previous).cwiseAbs().maxCoeff();
        stats.hasNormalizedUpdate = true;
        stats.normalizedUpdate = updateEstimate.normalizedUpdate;
        stats.hasNormalizedResidual = true;
        stats.normalizedResidual = candidateResidual;
        if(updateEstimate.normalizedUpdate <
               options.normalizedUpdateTolerance &&
           candidateResidual < options.normalizedResidualTolerance){
            return finish(true);
        }
        previous = current;
    }

    return finish(
        false,
        "Newton-Raphson maximum iteration count was reached"
    );
}

void Circuit::addNewtonStats(const NewtonSolveDiagnostics& stats){
    operatingPointStats_.iterations += stats.iterations;
    operatingPointStats_.dampedSteps += stats.dampedSteps;
    operatingPointStats_.finalDelta = stats.finalDelta;
}

void Circuit::cacheOperatingPointDeviceRoles(){
    sourceSteppingDevices_.clear();
    iterationStateDevices_.clear();
    hasNonlinearDevices_ = false;

    for(auto& device: devices_){
        if(device->getType() == DeviceType::VoltageSource ||
           device->getType() == DeviceType::CurrentSource){
            sourceSteppingDevices_.push_back(device.get());
        }

        if(device->isNonlinear()){
            hasNonlinearDevices_ = true;
            iterationStateDevices_.push_back(device.get());
        }
    }
}

void Circuit::assembleOperatingPointSystem(){
    mna_->clear();
    for(auto& device: devices_){
        device->stampOperatingPoint();
    }
}

void Circuit::assembleTransientSystem(const TransientStampContext& ctx){
    mna_->clear();
    for(auto& device: devices_){
        device->stampTransient(ctx);
    }
}

void Circuit::assemblePtaSystem(const TransientStampContext& ctx){
    mna_->clear();

    for(auto& device: devices_){
        device->stampOperatingPoint();
    }

    for(auto& pseudoDevice: pseudoDevices_){
        pseudoDevice->stampPseudo(ctx);
    }
}

bool Circuit::hasNonlinearDevices() const{
    return hasNonlinearDevices_;
}

void Circuit::setOperatingPointSourceScale(double scale){
    if(scale == operatingPointSourceScale_){
        return;
    }

    operatingPointSourceScale_ = scale;
    for(auto* device: sourceSteppingDevices_){
        device->setOperatingPointSourceScale(scale);
    }
}

void Circuit::saveNonlinearIterationStates(){
    for(auto* device: iterationStateDevices_){
        device->saveIterationState();
    }
}

void Circuit::restoreNonlinearIterationStates(){
    for(auto* device: iterationStateDevices_){
        device->restoreIterationState();
    }
}

void Circuit::saveNonlinearLineSearchStates(){
    for(auto* device: iterationStateDevices_){
        device->saveLineSearchState();
    }
}

void Circuit::restoreNonlinearLineSearchStates(){
    for(auto* device: iterationStateDevices_){
        device->restoreLineSearchState();
    }
}
