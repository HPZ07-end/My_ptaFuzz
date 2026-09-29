#include "circuit/circuit.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#include "analysis/solver_options.hpp"
#include "analysis/transient_analysis.hpp"
#include "circuit/node_map.hpp"
#include "devices/device.hpp"
#include "solver/mna.hpp"

namespace {
constexpr double kTimeRelativeTolerance =
    64.0 * std::numeric_limits<double>::epsilon();
constexpr double kStrictLteMaximumGrowth = 1.5;

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

const char* transientFailureReason(TransientStepAction action){
    switch(action){
        case TransientStepAction::FailInvalidEstimate:
            return "transient error estimate is invalid";
        case TransientStepAction::FailMinimumStep:
            return "transient step cannot be reduced above the minimum step";
        case TransientStepAction::FailRejectLimit:
            return "transient step rejection limit was reached";
        default:
            return "transient step controller failed";
    }
}

bool timeReached(double time, double target){
    const double scale = std::max(
        std::abs(time),
        std::abs(target)
    );
    return time >= target - kTimeRelativeTolerance * scale;
}

bool sameTime(double lhs, double rhs){
    const double scale = std::max(std::abs(lhs), std::abs(rhs));
    return std::abs(lhs - rhs) <= kTimeRelativeTolerance * scale;
}

bool advanceOutputTime(double& nextOutputTime,
                       double currentTime,
                       double outputInterval){
    do {
        const double previousOutputTime = nextOutputTime;
        nextOutputTime += outputInterval;
        if(nextOutputTime <= previousOutputTime){
            return false;
        }
    } while(timeReached(currentTime, nextOutputTime));
    return true;
}
}

bool Circuit::solveTransient(const TransientAnalysisConfig& config){
    return solveTransient(config, {});
}

bool Circuit::solveTransient(
    const TransientAnalysisConfig& config,
    const OperatingPointSolverOptions& operatingPointOptions
){
    transientStats_ = {};
    transientStats_.attempted = true;
    transientStats_.usedInitialConditions = config.useInitialConditions;
    transientStats_.finalMethod =
        "adaptive Backward Euler / variable-step BDF2 with strict LTE";
    transientStats_.maxIterations =
        config.solverOptions.newtonOptions.maximumIterations;
    transientStats_.tolerance = config.solverOptions.newtonOptions.tolerance;
    transientSamples_.clear();

    const std::clock_t startClock = std::clock();
    const SteadyClock::time_point startWall = SteadyClock::now();
    TransientIntegrator integrator;
    double time = 0.0;

    const auto finishTransient = [this, startClock, startWall, &time](
        bool converged,
        const std::string& failureReason = std::string{}
    ) {
        transientStats_.converged = converged;
        transientStats_.finalTime = time;
        transientStats_.cpuSeconds =
            double(std::clock() - startClock) / CLOCKS_PER_SEC;
        transientStats_.wallSeconds = elapsedWallSeconds(startWall);
        if(converged){
            transientStats_.failureReason.clear();
        }else if(!failureReason.empty()){
            transientStats_.failureReason = failureReason;
        }
        return converged;
    };

    const double maximumIntegrationStep = config.maximumStep
        ? *config.maximumStep
        : config.outputInterval;

    if(!config.valid() || !operatingPointOptions.valid()){
        return finishTransient(
            false,
            "transient or operating-point initialization configuration is invalid"
        );
    }

    const bool hasMos3UicChargeHistory = config.useInitialConditions &&
        std::any_of(
            devices_.begin(), devices_.end(),
            [](const std::unique_ptr<Device>& device){
                return device->requiresUicChargeHistoryProtection();
            }
        );

    Eigen::VectorXd initialSolution;

    setOperatingPointSourceScale(1.0);
    if(config.useInitialConditions){
        initialSolution = hasUicInitialSolution_
            ? uicInitialSolution_
            : Eigen::VectorXd::Zero(mna_->size());
        mna_->setSolution(initialSolution);
    } else {
        if(!solveOperatingPoint(operatingPointOptions)){
            transientStats_.initializationCpuSeconds =
                double(std::clock() - startClock) / CLOCKS_PER_SEC;
            transientStats_.initializationWallSeconds =
                elapsedWallSeconds(startWall);
            return finishTransient(
                false,
                "transient operating-point initialization failed: " +
                    operatingPointStats_.failureReason
            );
        }
        initialSolution = mna_->solution();
    }

    integrator.initialize(time, initialSolution);
    for(auto& device: devices_){
        device->initializeTransientHistory(initialSolution);
    }

    transientStats_.initializationCpuSeconds =
        double(std::clock() - startClock) / CLOCKS_PER_SEC;
    transientStats_.initializationWallSeconds = elapsedWallSeconds(startWall);

    double nextOutputTime = config.outputStartTime;
    if(timeReached(time, nextOutputTime)){
        recordTransientSample(time);
        if(!advanceOutputTime(
            nextOutputTime,
            time,
            config.outputInterval
        )){
            return finishTransient(
                false,
                "transient output time cannot be advanced"
            );
        }
    }

    double proposedStep = maximumIntegrationStep;

    const auto failTransient = [&finishTransient](const std::string& reason) {
        return finishTransient(false, reason);
    };

    while(!timeReached(time, config.stopTime)){
        double hardStepLimit = std::min(
            {
                maximumIntegrationStep,
                nextOutputTime - time,
                config.stopTime - time
            }
        );

        const bool hasStrictBdf2History = !hasMos3UicChargeHistory &&
            integrator.oldestSolution() != nullptr;
        if(hasStrictBdf2History){
            const double bdf2StepLimit = std::nextafter(
                integrator.previousStep() * integrator.maximumBdf2StepRatio(),
                0.0
            );
            hardStepLimit = std::min(hardStepLimit, bdf2StepLimit);
        }

        if(!std::isfinite(hardStepLimit) || hardStepLimit <= 0.0){
            return failTransient(
                "transient hard step limit is non-finite or non-positive"
            );
        }

        double candidateStep = std::min(proposedStep, hardStepLimit);
        if(!std::isfinite(candidateStep) ||
           candidateStep <= 0.0 ||
           time + candidateStep <= time){
            return failTransient(
                "transient candidate step is invalid or cannot advance time"
            );
        }

        int rejectedAttempts = 0;

        const bool useBdf2 = hasStrictBdf2History &&
            integrator.hasStrictBdf2LteHistory(time + candidateStep);

        while(true){
            const double nextTime = time + candidateStep;
            if(!std::isfinite(nextTime) || nextTime <= time){
                return failTransient(
                    "transient candidate time is invalid or did not advance"
                );
            }

            const Eigen::VectorXd acceptedSolution = mna_->solution();
            saveNonlinearIterationStates();

            const auto runTransientAttempt =
                [&](const TransientIntegrator& trialIntegrator,
                    double trialTime) {
                    TransientStepAttempt trial = tryTransientStep(
                        trialIntegrator,
                        trialTime,
                        config.solverOptions
                    );
                    ++transientStats_.attemptedSteps;
                    const double attemptedStep =
                        trialTime - trialIntegrator.acceptedTime();
                    updateStepRange(
                        attemptedStep,
                        transientStats_.minimumAttemptedStep,
                        transientStats_.maximumAttemptedStep
                    );
                    if(trial.integrationOrder == 1){
                        ++transientStats_.backwardEulerAttempts;
                    }else if(trial.integrationOrder == 2){
                        ++transientStats_.bdf2Attempts;
                    }
                    addTransientStats(trial.newtonStats);
                    return trial;
                };

            TransientStepAttempt attempt;

            if(!useBdf2){
                const double halfStep = 0.5 * candidateStep;
                const double halfTime = time + halfStep;

                if(!std::isfinite(halfStep) ||
                   halfStep < config.solverOptions.minimumStep ||
                   !std::isfinite(halfTime) ||
                   halfTime <= time ||
                   nextTime <= halfTime){
                    restoreTransientCheckpoint(acceptedSolution);
                    return failTransient(
                        "Backward Euler step-doubling requires a valid half-step "
                        "at or above the transient minimum step"
                    );
                }

                TransientIntegrator coarseIntegrator(
                    integrator.maximumBdf2StepRatio()
                );
                // Startup/error-control step doubling is deliberately BE.
                // Do not let the outer integrator's two-point BDF2 history
                // leak into the coarse trial before strict LTE history exists.
                coarseIntegrator.restartFrom(time, acceptedSolution);
                TransientStepAttempt coarse = runTransientAttempt(
                    coarseIntegrator,
                    nextTime
                );
                attempt = coarse;

                if(coarse.converged && coarse.integrationOrder == 1){
                    // The coarse solve may change nonlinear limiting state;
                    // start the fine pair from the same accepted checkpoint.
                    restoreTransientCheckpoint(acceptedSolution);

                    TransientIntegrator fineIntegrator(
                        integrator.maximumBdf2StepRatio()
                    );
                    fineIntegrator.restartFrom(time, acceptedSolution);

                    TransientStepAttempt firstHalf = runTransientAttempt(
                        fineIntegrator,
                        halfTime
                    );
                    attempt = firstHalf;

                    if(firstHalf.converged &&
                       firstHalf.integrationOrder == 1){
                        advanceTransientHistory(
                            acceptedSolution, firstHalf.solution
                        );
                        firstHalf.transientHistoryAdvanced = true;
                        // restartFrom deliberately clears history: the
                        // second half is BE rather than an internal BDF2 step.
                        fineIntegrator.restartFrom(
                            halfTime,
                            firstHalf.solution
                        );

                        TransientStepAttempt secondHalf =
                            runTransientAttempt(
                                fineIntegrator,
                                nextTime
                            );

                        if(secondHalf.converged &&
                           secondHalf.integrationOrder == 1){
                            advanceTransientHistory(
                                firstHalf.solution, secondHalf.solution
                            );
                            secondHalf.transientHistoryAdvanced = true;
                            const TransientErrorEstimate estimate =
                                estimateTransientSolutionDifference(
                                    acceptedSolution,
                                    secondHalf.solution,
                                    coarse.solution,
                                    nodeMap_->nodeCount(),
                                    config.solverOptions,
                                    !hasMos3UicChargeHistory
                                );

                            // Commit the fine endpoint, never the coarse
                            // whole-step solution or a Richardson extrapolate.
                            secondHalf.errorEstimateValid = estimate.valid;
                            secondHalf.normalizedError =
                                estimate.normalizedError;
                            secondHalf.suggestedStepScale =
                                estimate.suggestedScale;
                        }
                        attempt = std::move(secondHalf);
                    }
                }
            } else {
                attempt = runTransientAttempt(integrator, nextTime);
            }

            transientStats_.lastAttemptedStep = candidateStep;
            transientStats_.lastAttemptedTime = nextTime;
            transientStats_.lastIntegrationOrder = attempt.integrationOrder;
            transientStats_.lastNewton = attempt.newtonStats;
            if(attempt.errorEstimateValid &&
               std::isfinite(attempt.normalizedError)){
                transientStats_.hasNormalizedError = true;
                transientStats_.lastNormalizedError =
                    attempt.normalizedError;
                transientStats_.maximumNormalizedError = std::max(
                    transientStats_.maximumNormalizedError,
                    attempt.normalizedError
                );
            }

            TransientStepControlInput controlInput;
            controlInput.converged = attempt.converged;
            controlInput.requiresBdf2 = useBdf2;
            controlInput.integrationOrder = attempt.integrationOrder;
            controlInput.errorEstimateValid = attempt.errorEstimateValid;
            controlInput.normalizedError = attempt.normalizedError;
            controlInput.suggestedStepScale = attempt.suggestedStepScale;
            controlInput.currentTime = time;
            controlInput.attemptedStep = candidateStep;
            controlInput.hardStepLimit = hardStepLimit;
            controlInput.rejectedAttempts = rejectedAttempts;

            const TransientStepControlDecision decision =
                decideTransientStep(controlInput, config.solverOptions);

            if(decision.action == TransientStepAction::Accept){
                mna_->setSolution(attempt.solution);
                if(!attempt.transientHistoryAdvanced){
                    advanceTransientHistory(
                        integrator.currentSolution(), attempt.solution
                    );
                }
                integrator.accept(nextTime, std::move(attempt.solution));

                time = nextTime;
                if(timeReached(time, config.stopTime)){
                    time = config.stopTime;
                }
                ++transientStats_.timeSteps;
                updateStepRange(
                    candidateStep,
                    transientStats_.minimumAcceptedStep,
                    transientStats_.maximumAcceptedStep
                );

                proposedStep = std::min(
                    maximumIntegrationStep,
                    decision.nextStep
                );

                if(!std::isfinite(proposedStep) || proposedStep <= 0.0){
                    return failTransient(
                        "transient step controller proposed an invalid next step"
                    );
                }

                if(timeReached(time, nextOutputTime)){
                    recordTransientSample(time);
                    if(!timeReached(time, config.stopTime) &&
                       !advanceOutputTime(
                           nextOutputTime,
                           time,
                           config.outputInterval
                       )){
                        return failTransient(
                            "transient output time cannot be advanced"
                        );
                    }
                }

                break;
            }

            ++transientStats_.rejectedSteps;
            if(!attempt.converged){
                ++transientStats_.convergenceRejectedSteps;
            } else if(decision.action ==
                      TransientStepAction::FailInvalidEstimate){
                ++transientStats_.invalidEstimateFailures;
            } else if(attempt.errorEstimateValid &&
                      attempt.normalizedError > 1.0){
                ++transientStats_.errorRejectedSteps;
            }

            restoreTransientCheckpoint(acceptedSolution);

            if(decision.action == TransientStepAction::RetryConvergence ||
               decision.action == TransientStepAction::RetryError){
                candidateStep = decision.nextStep;
                ++rejectedAttempts;
                continue;
            }

            return failTransient(transientFailureReason(decision.action));
        }
    }

    if(transientSamples_.empty() ||
       !sameTime(transientSamples_.back().time, time)){
        recordTransientSample(time);
    }

    return finishTransient(true);
}

Circuit::TransientStepAttempt Circuit::tryTransientStep(
    const TransientIntegrator& integrator,
    double targetTime,
    const TransientSolverOptions& options
){
    TransientStepAttempt attempt;

    mna_->setSolution(integrator.predict(targetTime));

    const TransientStampContext ctx = integrator.makeContext(targetTime);

    attempt.integrationOrder = ctx.derivative.order;

    const AssembleCallback assemble = [this, &ctx]{
        assembleTransientSystem(ctx);
    };
    attempt.converged = hasNonlinearDevices()
        ? solveNewtonSystem(
            assemble,
            attempt.newtonStats,
            options.newtonOptions
        )
        : solveLinearSystem(assemble, attempt.newtonStats);

    if(attempt.converged){
        attempt.solution = mna_->solution();
        if(integrator.hasStrictBdf2LteHistory(targetTime)){
            const TransientErrorEstimate estimate = estimateStrictTransientLte(
                integrator,
                targetTime,
                attempt.solution,
                options
            );

            attempt.errorEstimateValid = estimate.valid;
            attempt.normalizedError = estimate.normalizedError;
            attempt.suggestedStepScale = estimate.suggestedScale;
        }
    }

    return attempt;
}

TransientErrorEstimate Circuit::estimateStrictTransientLte(
    const TransientIntegrator& integrator,
    double targetTime,
    const Eigen::VectorXd& correctedSolution,
    const TransientSolverOptions& options
){
    const TransientLteDefect defect = integrator.strictBdf2Defect(
        targetTime,
        correctedSolution
    );
    if(!defect.valid ||
       defect.derivativeDefect.size() != correctedSolution.size()){
        return {};
    }

    const TransientStampContext stampContext =
        integrator.makeContext(targetTime);
    if(stampContext.derivative.order != 2){
        return {};
    }

    // Reassemble at the converged endpoint.  The resulting matrix is the
    // DAE Jacobian used to project the BDF2 derivative defect into a state
    // error; solveFactorized deliberately leaves the accepted solution intact.
    mna_->setSolution(correctedSolution);
    assembleTransientSystem(stampContext);
    if(!mna_->factorize()){
        return {};
    }

    Eigen::VectorXd residual = Eigen::VectorXd::Zero(mna_->size());
    const TransientLteContext lteContext{
        correctedSolution,
        defect.derivativeDefect
    };
    for(const auto& device: devices_){
        device->stampTransientLteDefect(lteContext, residual);
    }
    if(!residual.allFinite()){
        return {};
    }

    Eigen::VectorXd localError;
    if(!mna_->solveFactorized(-residual, localError)){
        return {};
    }

    TransientErrorEstimate estimate = estimateTransientStateError(
        integrator.currentSolution(),
        correctedSolution,
        localError,
        nodeMap_->nodeCount(),
        options,
        3
    );
    if(estimate.valid){
        // The defect is asymptotically third order.  Limit growth while its
        // higher-order remainder is still visible on practical circuit steps.
        estimate.suggestedScale = std::min(
            estimate.suggestedScale,
            kStrictLteMaximumGrowth
        );
    }
    return estimate;
}

void Circuit::addTransientStats(const NewtonSolveDiagnostics& stats){
    transientStats_.iterations += stats.iterations;
    transientStats_.dampedSteps += stats.dampedSteps;
    transientStats_.finalDelta = stats.finalDelta;
}

void Circuit::restoreTransientCheckpoint(const Eigen::VectorXd& acceptedSolution){
    restoreNonlinearIterationStates();
    mna_->setSolution(acceptedSolution);
}

void Circuit::advanceTransientHistory(const Eigen::VectorXd& previous,
                                      const Eigen::VectorXd& accepted){
    for(auto& device: devices_){
        device->acceptTransientSolution(previous, accepted);
    }
}

void Circuit::recordTransientSample(double time){
    transientSamples_.push_back({time, mna_->solution()});
    ++transientStats_.outputPoints;
}
