#include "circuit/circuit.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <string>

#include "analysis/solver_options.hpp"
#include "solver/mna.hpp"

namespace {
constexpr double kSourceScaleDone = 1.0 - 1.0e-12;

using SteadyClock = std::chrono::steady_clock;

double elapsedWallSeconds(SteadyClock::time_point start){
    return std::chrono::duration<double>(SteadyClock::now() - start).count();
}
}

bool Circuit::solve(){
    return solveOperatingPoint();
}

bool Circuit::solveOperatingPoint(){
    return solveOperatingPoint({});
}

bool Circuit::solveOperatingPoint(
    const OperatingPointSolverOptions& options
){
    operatingPointStats_ = {};
    operatingPointStats_.attempted = true;
    operatingPointStats_.maxIterations = options.newton.maximumIterations;
    operatingPointStats_.tolerance = options.newton.tolerance;
    operatingPointStats_.minSourceStep = options.sourceStepping.minimumStep;

    const std::clock_t startClock = std::clock();
    const SteadyClock::time_point startWall = SteadyClock::now();
    const auto finish = [this, startClock, startWall](
        bool converged,
        const std::string& failureReason = std::string{}
    ) {
        operatingPointStats_.converged = converged;
        operatingPointStats_.cpuSeconds =
            double(std::clock() - startClock) / CLOCKS_PER_SEC;
        operatingPointStats_.wallSeconds = elapsedWallSeconds(startWall);
        if(converged){
            operatingPointStats_.failureReason.clear();
        }else if(!failureReason.empty()){
            operatingPointStats_.failureReason = failureReason;
        }
        return converged;
    };

    if(!options.valid()){
        return finish(false, "operating-point solver configuration is invalid");
    }

    const AssembleCallback assemble = [this] {
        assembleOperatingPointSystem();
    };
    if(hasOperatingPointInitialGuess_){
        mna_->setSolution(operatingPointInitialGuess_);
    }
    const Eigen::VectorXd initialSolution = mna_->solution();

    if(!hasNonlinearDevices()){
        setOperatingPointSourceScale(1.0);

        NewtonSolveDiagnostics linearStats;
        const bool linearSolved = solveLinearSystem(assemble, linearStats);
        operatingPointStats_.directNewton = linearStats;
        addNewtonStats(linearStats);

        operatingPointStats_.sourceScale = linearSolved ? 1.0 : 0.0;
        operatingPointStats_.finalMethod = "linear";
        return finish(linearSolved, linearStats.failureReason);
    }

    saveNonlinearIterationStates();
    setOperatingPointSourceScale(1.0);

    NewtonSolveDiagnostics directStats;
    const bool directConverged = solveNewtonSystem(
        assemble,
        directStats,
        options.newton
    );
    operatingPointStats_.directNewton = directStats;
    addNewtonStats(directStats);

    if(directConverged){
        operatingPointStats_.sourceScale = 1.0;
        operatingPointStats_.finalMethod = "direct Newton-Raphson";
        return finish(true);
    }

    restoreNonlinearIterationStates();
    mna_->setSolution(initialSolution);
    setOperatingPointSourceScale(0.0);

    if(!options.sourceStepping.enabled){
        return finish(
            false,
            "direct Newton-Raphson failed (" + directStats.failureReason +
                ") and source stepping is disabled"
        );
    }
    if(!solveOperatingPointWithSourceStepping(assemble, options)){
        return finish(
            false,
            operatingPointStats_.failureReason.empty()
                ? "source stepping failed"
                : operatingPointStats_.failureReason
        );
    }

    setOperatingPointSourceScale(1.0);
    operatingPointStats_.finalMethod = "source stepping";
    return finish(true);
}

bool Circuit::solveOperatingPointWithSourceStepping(
    const AssembleCallback& assemble,
    const OperatingPointSolverOptions& options
){
    Eigen::VectorXd acceptedSolution = mna_->solution();
    double acceptedScale = 0.0;
    double sourceStep = options.sourceStepping.initialStep;

    while(acceptedScale < kSourceScaleDone){
        const double trialScale = std::min(1.0, acceptedScale + sourceStep);
        const double actualStep = trialScale - acceptedScale;

        SourceSteppingAttemptDiagnostics attempt;
        attempt.attempt =
            static_cast<int>(operatingPointStats_.sourceAttempts.size()) + 1;
        attempt.acceptedScaleBefore = acceptedScale;
        attempt.stepSize = actualStep;
        attempt.targetScale = trialScale;

        mna_->setSolution(acceptedSolution);
        saveNonlinearIterationStates();
        setOperatingPointSourceScale(trialScale);

        NewtonSolveDiagnostics trialStats;
        const bool converged = solveNewtonSystem(
            assemble,
            trialStats,
            options.newton
        );
        attempt.newton = trialStats;
        addNewtonStats(trialStats);

        if(converged){
            attempt.accepted = true;
            attempt.status = "accepted";
            operatingPointStats_.sourceAttempts.push_back(std::move(attempt));
            acceptedScale = trialScale;
            acceptedSolution = mna_->solution();
            operatingPointStats_.sourceScale = acceptedScale;
            ++operatingPointStats_.sourceSteps;
            sourceStep = std::min(
                options.sourceStepping.maximumStep,
                sourceStep * options.sourceStepping.growthFactor
            );
            continue;
        }

        restoreNonlinearIterationStates();
        mna_->setSolution(acceptedSolution);
        setOperatingPointSourceScale(acceptedScale);
        ++operatingPointStats_.failedSourceSteps;
        attempt.status = "rejected";
        operatingPointStats_.sourceAttempts.push_back(std::move(attempt));

        sourceStep *= options.sourceStepping.failureScale;
        if(sourceStep < options.sourceStepping.minimumStep){
            operatingPointStats_.failureReason =
                "source stepping failed because the next source step is "
                "below the configured minimum";
            return false;
        }
    }

    mna_->setSolution(acceptedSolution);
    return true;
}
