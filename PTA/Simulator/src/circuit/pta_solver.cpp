#include "circuit/circuit.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <memory>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>

#include "analysis/pta_analysis.hpp"
#include "analysis/transient_analysis.hpp"
#include "circuit/node_map.hpp"
#include "devices/device.hpp"
#include "devices/pseudo_device.hpp"
#include "solver/mna.hpp"

namespace {
using SteadyClock = std::chrono::steady_clock;

double elapsedWallSeconds(SteadyClock::time_point start){
    return std::chrono::duration<double>(SteadyClock::now() - start).count();
}

void updateStepRange(double step, double& minimum, double& maximum){
    if(!std::isfinite(step) || step <= 0.0){
        return;
    }
    if(minimum == 0.0 || step < minimum){
        minimum = step;
    }
    maximum = std::max(maximum, step);
}

NewtonSolveDiagnostics combineTrustRegionRecoveryDiagnostics(
    const NewtonSolveDiagnostics& trustRegion,
    const NewtonSolveDiagnostics& recovery
){
    // The recovery starts from the same pseudo-time checkpoint, but both
    // local solves consumed work and must remain visible in the trace.
    NewtonSolveDiagnostics combined = recovery;
    combined.iterations += trustRegion.iterations;
    combined.maximumIterations += trustRegion.maximumIterations;
    combined.dampedSteps += trustRegion.dampedSteps;
    combined.backtrackingSteps += trustRegion.backtrackingSteps;
    combined.lineSearchEvaluations += trustRegion.lineSearchEvaluations;
    combined.nonMonotoneStepFallbacks +=
        trustRegion.nonMonotoneStepFallbacks;
    combined.cpuSeconds += trustRegion.cpuSeconds;
    combined.wallSeconds += trustRegion.wallSeconds;

    combined.usedTrustRegion = trustRegion.usedTrustRegion;
    combined.trustRegionRetriesExhausted =
        trustRegion.trustRegionRetriesExhausted;
    combined.usedTrustRegionExhaustionRecovery = true;
    combined.trustRegionExhaustionRecoveryIterations = recovery.iterations;
    combined.trustRegionExhaustionReason = trustRegion.failureReason;
    combined.trustRegionTrials = trustRegion.trustRegionTrials;
    combined.trustRegionRejectedSteps = trustRegion.trustRegionRejectedSteps;
    combined.trustRegionRadiusReductions =
        trustRegion.trustRegionRadiusReductions;
    combined.trustRegionRadiusExpansions =
        trustRegion.trustRegionRadiusExpansions;
    combined.initialTrustRegionRadius = trustRegion.initialTrustRegionRadius;
    combined.finalTrustRegionRadius = trustRegion.finalTrustRegionRadius;
    combined.hasTrustRegionRatio = trustRegion.hasTrustRegionRatio;
    combined.lastTrustRegionRatio = trustRegion.lastTrustRegionRatio;
    return combined;
}
}

bool Circuit::solveAdaptivePta(const PtaAnalysisConfig& config){
    const bool continuesFailedOrdinarySolve =
        operatingPointStats_.attempted &&
        !operatingPointStats_.converged &&
        !operatingPointStats_.ptaAttempted;
    if(!activatePta(config)){
        operatingPointStats_.attempted = true;
        operatingPointStats_.ptaAttempted = true;
        operatingPointStats_.converged = false;
        operatingPointStats_.failureReason =
            "PTA matrix activation failed";
        return false;
    }
    if(!continuesFailedOrdinarySolve){
        operatingPointStats_ = {};
        if(hasOperatingPointInitialGuess_){
            mna_->setSolution(operatingPointInitialGuess_);
        }
    }
    operatingPointStats_.attempted = true;
    operatingPointStats_.ptaAttempted = true;
    operatingPointStats_.maxIterations = config.newtonOptions.maximumIterations;
    operatingPointStats_.tolerance = config.newtonOptions.tolerance;
    operatingPointStats_.ptaAttempts.clear();

    const double priorCpuSeconds = operatingPointStats_.cpuSeconds;
    const double priorWallSeconds = operatingPointStats_.wallSeconds;
    const std::clock_t startClock = std::clock();
    const SteadyClock::time_point startWall = SteadyClock::now();
    const auto finish = [
        this,
        startClock,
        startWall,
        priorCpuSeconds,
        priorWallSeconds
    ](
        bool converged,
        const std::string& failureReason = std::string{}
    ) {
        operatingPointStats_.ptaCpuSeconds =
            double(std::clock() - startClock) / CLOCKS_PER_SEC;
        operatingPointStats_.ptaWallSeconds =
            elapsedWallSeconds(startWall);
        operatingPointStats_.cpuSeconds =
            priorCpuSeconds + operatingPointStats_.ptaCpuSeconds;
        operatingPointStats_.wallSeconds =
            priorWallSeconds + operatingPointStats_.ptaWallSeconds;
        operatingPointStats_.converged = converged;
        if(converged){
            operatingPointStats_.finalMethod =
                "pseudo-transient analysis (PTA)";
            operatingPointStats_.failureReason.clear();
        }else if(!failureReason.empty()){
            operatingPointStats_.failureReason = failureReason;
        }
        return converged;
    };

    TransientIntegrator integrator;
    double time = 0.0;
    double step = config.initialStep;

    initializePtaStates(config, time);

    integrator.initialize(time, mna_->solution());

    Eigen::VectorXd derivative(mna_->size());
    Eigen::VectorXd matrixProduct(mna_->size());
    Eigen::VectorXd residual(mna_->size());

    for(int stepCount = 0;
        stepCount < config.maximumSteps;
        ++stepCount)
    {
        double candidateStep = std::min(step, config.maximumStep);
        if(integrator.olderSolution() != nullptr){
            const double bdf2StepLimit = std::nextafter(
                integrator.previousStep() *
                    integrator.maximumBdf2StepRatio(),
                0.0
            );
            candidateStep = std::min(candidateStep, bdf2StepLimit);
        }

        const double nextTime = time + candidateStep;
        PtaStepAttemptDiagnostics attempt;
        attempt.attempt = stepCount + 1;
        attempt.startTime = time;
        attempt.targetTime = nextTime;
        attempt.timeStep = candidateStep;
        operatingPointStats_.ptaFinalStep = candidateStep;
        updateStepRange(
            candidateStep,
            operatingPointStats_.ptaMinimumAttemptedStep,
            operatingPointStats_.ptaMaximumAttemptedStep
        );

        if(!std::isfinite(candidateStep) ||
           candidateStep < config.minimumStep ||
           !std::isfinite(nextTime) ||
           nextTime <= time){
            attempt.status = "failed";
            attempt.failureReason =
                "PTA time step fell below the minimum or cannot advance pseudo-time";
            operatingPointStats_.ptaAttempts.push_back(std::move(attempt));
            return finish(
                false,
                "PTA time step fell below the minimum or cannot advance pseudo-time"
            );
        }

        const Eigen::VectorXd acceptedSolution =
            integrator.currentSolution();

        const double sourceScale = ptaSourceRampScale(
            nextTime,
            config.sourceRampTime
        );
        attempt.sourceScale = sourceScale;
        operatingPointStats_.ptaFinalSourceScale = sourceScale;
        if(!std::isfinite(sourceScale)){
            attempt.status = "failed";
            attempt.failureReason = "PTA source-ramp scale is non-finite";
            operatingPointStats_.ptaAttempts.push_back(std::move(attempt));
            return finish(false, "PTA source-ramp scale is non-finite");
        }
        setOperatingPointSourceScale(sourceScale);

        const Eigen::VectorXd trialInitialSolution = integrator.predict(nextTime);
        mna_->setSolution(trialInitialSolution);
        saveNonlinearIterationStates();

        const TransientStampContext ctx =
            integrator.makeContext(nextTime);
        attempt.integrationOrder = ctx.derivative.order;

        const AssembleCallback assemble = [this, &ctx] {
            assemblePtaSystem(ctx);
        };

        NewtonSolveDiagnostics stats;
        bool converged = hasNonlinearDevices()
            ? solveNewtonSystem(assemble, stats, config.newtonOptions)
            : solveLinearSystem(assemble, stats);

        const bool hasPtaLimiterSeed = config.initialBjtVbe.has_value() ||
            config.initialMosVgs.has_value();
        if(hasPtaLimiterSeed && hasNonlinearDevices() && !converged &&
           config.newtonOptions.trustRegionEnabled &&
           stats.trustRegionRetriesExhausted)
        {
            // A rejected trust-region model does not mean that this
            // seeded pseudo-time point is unusable.  Restore the checkpoint
            // before the first local Newton solve, then give PTA's existing
            // non-monotone continuation policy one independent attempt.
            restoreNonlinearIterationStates();
            mna_->setSolution(trialInitialSolution);

            NewtonSolverOptions recoveryOptions = config.newtonOptions;
            recoveryOptions.trustRegionEnabled = false;
            NewtonSolveDiagnostics recoveryStats;
            converged = solveNewtonSystem(
                assemble,
                recoveryStats,
                recoveryOptions
            );
            stats = combineTrustRegionRecoveryDiagnostics(
                stats,
                recoveryStats
            );
        }
        attempt.newton = stats;
        addNewtonStats(stats);
        operatingPointStats_.ptaIterations += stats.iterations;
        operatingPointStats_.ptaDampedSteps += stats.dampedSteps;

        if(!converged){
            ++operatingPointStats_.ptaRejectedSteps;

            // Attribute a minimum-step recovery to one KCL node.  The
            // pseudo-system residual is intentionally used here: it tells us
            // which artificial mass term failed to stabilize the trial.
            assemblePtaSystem(ctx);
            if(!mna_->evaluateResidual(matrixProduct, residual)){
                const std::string failureReason =
                    "PTA pseudo-system residual contains a non-finite value";
                attempt.status = "failed";
                attempt.failureReason = failureReason;
                operatingPointStats_.ptaAttempts.push_back(std::move(attempt));
                return finish(false, failureReason);
            }
            restoreTransientCheckpoint(acceptedSolution);

            const double reducedStep = std::max(
                config.minimumStep,
                candidateStep * config.failedStepScale
            );

            if(reducedStep < candidateStep){
                attempt.retriedWithSmallerStep = true;
                attempt.retryTimeStep = reducedStep;
                attempt.status = "retry with smaller pseudo-time step";
                attempt.failureReason = stats.failureReason;
                operatingPointStats_.ptaAttempts.push_back(std::move(attempt));
                step = reducedStep;
                continue;
            }

            const auto growth = growPtaNodeCapacitanceForResidual(
                residual,
                config
            );
            if(!growth){
                attempt.status = "failed";
                attempt.failureReason =
                    "PTA Newton solve failed at the minimum step and no "
                    "residual-selected node capacitance can be increased";
                operatingPointStats_.ptaAttempts.push_back(std::move(attempt));
                return finish(
                    false,
                    "PTA Newton solve failed at the minimum step and no "
                    "residual-selected node capacitance can be increased"
                );
            }

            ++operatingPointStats_.ptaMinimumStepRecoveries;
            ++operatingPointStats_.ptaCapacitanceGrowths;
            attempt.restartedAfterCapacitanceGrowth = true;
            attempt.capacitanceGrowths = 1;
            PtaNodeCapacitanceGrowth recordedGrowth = *growth;
            const auto& nodeNames = nodeMap_->nodeNameByIdx();
            if(recordedGrowth.nodeIndex >= 0 &&
               static_cast<std::size_t>(recordedGrowth.nodeIndex) <
                   nodeNames.size()){
                recordedGrowth.nodeName = nodeNames[
                    static_cast<std::size_t>(recordedGrowth.nodeIndex)
                ];
            }
            attempt.capacitanceGrowthNodes.push_back(std::move(recordedGrowth));
            attempt.retryTimeStep = reducedStep;
            attempt.status =
                "restart after residual-selected PTA capacitance growth";
            attempt.failureReason =
                "Newton solve failed at the minimum PTA step";
            operatingPointStats_.ptaAttempts.push_back(std::move(attempt));
            integrator.initialize(time, acceptedSolution);
            step = reducedStep;
            continue;
        }

        Eigen::VectorXd currentSolution = mna_->solution();

        // BDF coefficients sum to zero.  Evaluate the derivative from state
        // differences so a settled solution does not lose precision through
        // cancellation among terms of order |x| / h.
        derivative = currentSolution - ctx.previousSolution;
        derivative *= ctx.derivative.alpha0;
        if(ctx.olderSolution != nullptr){
            derivative += ctx.derivative.alpha2 *
                (*ctx.olderSolution - ctx.previousSolution);
        }
        if(!derivative.allFinite()){
            attempt.status = "failed";
            attempt.failureReason = "PTA derivative contains a non-finite value";
            operatingPointStats_.ptaAttempts.push_back(std::move(attempt));
            return finish(
                false,
                "PTA derivative contains a non-finite value"
            );
        }
        const PtaDerivativeEstimate derivativeEstimate =
            estimatePtaNormalizedDerivative(
                derivative,
                currentSolution,
                ctx.previousSolution,
                nodeMap_->nodeCount(),
                ctx.timeStep,
                config
        );
        if(!derivativeEstimate.valid){
            attempt.status = "failed";
            attempt.failureReason = "PTA normalized derivative estimate is invalid";
            operatingPointStats_.ptaAttempts.push_back(std::move(attempt));
            return finish(
                false,
                "PTA normalized derivative estimate is invalid"
            );
        }

        // Test the original DC residual F(x), excluding artificial PTA terms
        // and always using the nominal independent-source values.  The ramp
        // is a convergence aid and must not redefine the target DC system.
        setOperatingPointSourceScale(1.0);
        assembleOperatingPointSystem();
        if(!mna_->evaluateResidual(matrixProduct, residual)){
            attempt.status = "failed";
            attempt.failureReason = "PTA DC residual contains a non-finite value";
            operatingPointStats_.ptaAttempts.push_back(std::move(attempt));
            return finish(
                false,
                "PTA DC residual contains a non-finite value"
            );
        }
        const PtaResidualEstimate dcResidualEstimate =
            estimatePtaNormalizedResidual(
                residual,
                matrixProduct,
                mna_->rhs(),
                nodeMap_->nodeCount(),
                config
        );
        if(!dcResidualEstimate.valid){
            attempt.status = "failed";
            attempt.failureReason = "PTA normalized DC residual estimate is invalid";
            operatingPointStats_.ptaAttempts.push_back(std::move(attempt));
            return finish(
                false,
                "PTA normalized DC residual estimate is invalid"
            );
        }

        attempt.hasConvergenceMetrics = true;
        attempt.normalizedDerivative =
            derivativeEstimate.normalizedDerivative;
        attempt.normalizedDcResidual =
            dcResidualEstimate.normalizedResidual;
        operatingPointStats_.hasPtaConvergenceMetrics = true;
        operatingPointStats_.ptaNormalizedDerivative =
            derivativeEstimate.normalizedDerivative;
        operatingPointStats_.ptaNormalizedDcResidual =
            dcResidualEstimate.normalizedResidual;

        if(derivativeEstimate.normalizedDerivative <
               config.derivativeTolerance &&
           dcResidualEstimate.normalizedResidual <
               config.dcResidualTolerance){
            attempt.accepted = true;
            attempt.reachedSteadyState = true;
            attempt.status = "steady state converged";
            ++operatingPointStats_.ptaAcceptedSteps;
            operatingPointStats_.ptaFinalTime = nextTime;
            operatingPointStats_.sourceScale = 1.0;
            operatingPointStats_.ptaAttempts.push_back(std::move(attempt));
            return finish(true);
        }

        const PtaCapacitanceReductionSummary reductions =
            updatePtaNodeCapacitancesAfterAcceptedStep(
                currentSolution,
                acceptedSolution,
                config
            );
        attempt.capacitanceReductions = reductions.total;
        attempt.smallOscillationCapacitanceReductions =
            reductions.smallOscillation;
        attempt.mediumOscillationCapacitanceReductions =
            reductions.mediumOscillation;
        attempt.heavyOscillationCapacitanceReductions =
            reductions.heavyOscillation;
        attempt.accepted = true;
        attempt.status = "accepted";
        ++operatingPointStats_.ptaAcceptedSteps;
        operatingPointStats_.ptaFinalTime = nextTime;
        operatingPointStats_.ptaAttempts.push_back(std::move(attempt));

        integrator.accept(nextTime, std::move(currentSolution));
        time = nextTime;
        step = std::min(
            config.maximumStep,
            candidateStep * config.successfulStepScale
        );
    }

    return finish(
        false,
        "PTA maximum pseudo-time step count was reached before convergence"
    );
}

void Circuit::initializePtaStates(const PtaAnalysisConfig& config,
                                  double time){
    const double sourceScale = ptaSourceRampScale(
        time,
        config.sourceRampTime
    );
    setOperatingPointSourceScale(sourceScale);

    if(config.initialBjtVbe){
        for(auto& device: devices_){
            if(device->getType() == DeviceType::BJT){
                device->initializePtaState(*config.initialBjtVbe);
            }
        }
    }

    if(config.initialMosVgs){
        for(auto& device: devices_){
            if(device->getType() == DeviceType::MOSFET){
                device->initializePtaMosVgsState(*config.initialMosVgs);
            }
        }
    }
}

void Circuit::collectPendingPtaPlacements(const PtaAnalysisConfig& config){
    pendingPtaPlacements_.clear();

    const auto addNodeCaps =
        [this](const Device* owner, std::initializer_list<int> terminals){
            const auto& nodes = owner->getNodeIds();

            for(int terminal: terminals){
                if(terminal < static_cast<int>(nodes.size()) &&
                    nodes[terminal] >= 0){
                        pendingPtaPlacements_.push_back({
                            PtaPlacementKind::TransistorNodeCap,
                            owner,
                            terminal
                        });
                }
            }
    };

    for(auto& device: devices_){
        const Device* owner = device.get();

        switch(owner->getType()){
            case DeviceType::VoltageSource:{
                pendingPtaPlacements_.push_back({
                    PtaPlacementKind::VoltageSourceSeriesInductor,
                    owner,
                    -1
                });
                break;
            }
            case DeviceType::CurrentSource:{
                pendingPtaPlacements_.push_back({
                    PtaPlacementKind::CurrentSourceParallelCap,
                    owner,
                    -1
                });
                break;
            }
            case DeviceType::BJT:{
                addNodeCaps(owner, {0, 1, 2});
                break;
            }
            case DeviceType::MOSFET:{
                addNodeCaps(owner, {0, 1, 2});
                if(config.includeMosBulk){
                    addNodeCaps(owner, {3});
                }
                break;
            }
            case DeviceType::Diode:{
                if(config.includeDiodes){
                    addNodeCaps(owner, {0, 1});
                }
                break;
            }
            default:
                break;
        }
    }
}

void Circuit::materializePseudoDevices(const PtaAnalysisConfig& config){
    pseudoDevices_.clear();
    ptaNodeCaps_.clear();
    std::unordered_set<int> cappedNodes;
    std::set<std::pair<int, int>> cappedSourcePairs;
    std::unordered_set<int> inductedBranches;

    const auto addPseudo = [this](std::unique_ptr<PseudoDevice> device){
        pseudoDevices_.push_back(std::move(device));
    };

    const bool useCompoundElements = config.compoundTimeConstant > 0.0;

    for(const PendingPtaPlacement& placement: pendingPtaPlacements_){
        const Device* owner = placement.owner;
        if(owner == nullptr){
            continue;
        }

        const auto nodes = owner->getNodeIds();

        switch (placement.kind){
            case PtaPlacementKind::TransistorNodeCap:{
                const int terminal = placement.terminal;
                if(terminal < 0 || terminal >= static_cast<int>(nodes.size())){
                    continue;
                }

                const int node = nodes[terminal];
                if(node >= 0 && cappedNodes.insert(node).second){
                    const int branch = useCompoundElements
                        ? allocateUnknown()
                        : -1;
                    auto capacitor = std::make_unique<PseudoCapacitor>(
                        node,
                        -1,
                        config.initialNodeCapacitance,
                        useCompoundElements
                            ? config.compoundInitialResistance
                            : 0.0,
                        config.compoundTimeConstant,
                        branch
                    );
                    PseudoCapacitor* rawCapacitor = capacitor.get();

                    pseudoDevices_.push_back(std::move(capacitor));
                    ptaNodeCaps_.push_back({
                        node,
                        rawCapacitor,
                        config.initialNodeCapacitance,
                        0.0,
                        false
                    });
                }
                break;
            }

            case PtaPlacementKind::CurrentSourceParallelCap: {
                if(nodes.size() < 2){
                    continue;
                }

                const int p = nodes[0];
                const int n = nodes[1];
                const auto endpoints = std::minmax(p, n);
                const std::pair<int, int> key{
                    endpoints.first,
                    endpoints.second
                };

                if(p != n && cappedSourcePairs.insert(key).second){
                    const int branch = useCompoundElements
                        ? allocateUnknown()
                        : -1;
                    addPseudo(std::make_unique<PseudoCapacitor>(
                        p,
                        n,
                        config.currentSourceCapacitance,
                        useCompoundElements
                            ? config.compoundInitialResistance
                            : 0.0,
                        config.compoundTimeConstant,
                        branch
                    ));
                }
                break;
            }

            case PtaPlacementKind::VoltageSourceSeriesInductor:{
                const int branch = owner->branchUnknown();
                if(branch >= 0 && inductedBranches.insert(branch).second){
                    addPseudo(std::make_unique<PseudoInductor>(
                        branch,
                        config.voltageSourceInductance,
                        useCompoundElements
                            ? config.compoundInitialConductance
                            : 0.0,
                        config.compoundTimeConstant,
                        nodes.size() >= 1 ? nodes[0] : -1,
                        nodes.size() >= 2 ? nodes[1] : -1,
                        owner->nominalSourceValue(),
                        config.sourceRampTime
                    ));
                }
                break;
            }
        }
    }
}

std::optional<PtaNodeCapacitanceGrowth>
Circuit::growPtaNodeCapacitanceForResidual(
    const Eigen::VectorXd& residual,
    const PtaAnalysisConfig& config
){
    PtaNodeCapState* selected = nullptr;
    double selectedResidual = -1.0;

    for(PtaNodeCapState& state: ptaNodeCaps_){
        if(state.node < 0 || state.node >= residual.size() ||
           state.capacitor == nullptr ||
           !std::isfinite(state.capacitance) || state.capacitance <= 0.0){
            continue;
        }

        const double residualMagnitude = std::abs(residual[state.node]);
        const double nextCapacitance = std::min(
            state.capacitance * config.capacitanceGrowScale,
            config.maximumNodeCapacitance
        );
        if(!std::isfinite(residualMagnitude) ||
           !std::isfinite(nextCapacitance) ||
           nextCapacitance <= state.capacitance){
            continue;
        }

        if(selected == nullptr || residualMagnitude > selectedResidual ||
           (residualMagnitude == selectedResidual && state.node < selected->node)){
            selected = &state;
            selectedResidual = residualMagnitude;
        }
    }

    if(selected == nullptr){
        return std::nullopt;
    }

    const double capacitanceBefore = selected->capacitance;
    const double capacitanceAfter = std::min(
        capacitanceBefore * config.capacitanceGrowScale,
        config.maximumNodeCapacitance
    );
    selected->capacitor->setValue(capacitanceAfter);
    selected->capacitance = capacitanceAfter;
    selected->previousDelta = 0.0;
    selected->hasPreviousDelta = false;

    return PtaNodeCapacitanceGrowth{
        selected->node,
        {},
        selectedResidual,
        capacitanceBefore,
        capacitanceAfter
    };
}

Circuit::PtaCapacitanceReductionSummary
Circuit::updatePtaNodeCapacitancesAfterAcceptedStep(
    const Eigen::VectorXd& currentSolution,
    const Eigen::VectorXd& previousSolution,
    const PtaAnalysisConfig& config
){
    PtaCapacitanceReductionSummary summary;
    for(auto& state: ptaNodeCaps_){
        if(state.node < 0 ||
           state.node >= currentSolution.size() ||
           state.node >= previousSolution.size()){
            state.previousDelta = 0.0;
            state.hasPreviousDelta = false;
            continue;
        }

        const double currentDelta =
            currentSolution[state.node] - previousSolution[state.node];

        if(!std::isfinite(currentDelta)){
            state.previousDelta = 0.0;
            state.hasPreviousDelta = false;
            continue;
        }

        const bool oscillating =
            state.hasPreviousDelta &&
            std::isfinite(state.previousDelta) &&
            ((state.previousDelta < 0.0 && currentDelta > 0.0) ||
            (state.previousDelta > 0.0 && currentDelta < 0.0));

        if(oscillating &&
           state.capacitor != nullptr &&
           std::isfinite(state.capacitance) &&
           state.capacitance > 0.0){
            const double ratio =
                std::abs(currentDelta) / std::abs(state.previousDelta);

            double capacitanceScale = config.smallOscillationScale;
            int* severityCount = &summary.smallOscillation;
            if(ratio >= config.heavyOscillationRatio){
                capacitanceScale = config.heavyOscillationScale;
                severityCount = &summary.heavyOscillation;
            }else if(ratio >= config.mediumOscillationRatio){
                capacitanceScale = config.mediumOscillationScale;
                severityCount = &summary.mediumOscillation;
            }

            const double nextCapacitance = std::max(
                config.minimumNodeCapacitance,
                state.capacitance * capacitanceScale
            );

            if(std::isfinite(nextCapacitance) &&
               nextCapacitance < state.capacitance){
                state.capacitor->setValue(nextCapacitance);
                state.capacitance = nextCapacitance;
                ++operatingPointStats_.ptaCapacitanceReductions;
                ++summary.total;
                ++(*severityCount);
            }
        }

        state.previousDelta = currentDelta;
        state.hasPreviousDelta = true;
    }
    return summary;
}
