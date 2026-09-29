#pragma once

#include <cassert>
#include <optional>
#include <algorithm>
#include <cmath>
#include <utility>

#include <Eigen/Core>

#include "analysis/solver_options.hpp"

struct TransientSolverOptions {
    NewtonSolverOptions newtonOptions;

    double relativeTolerance = 1.0e-4;
    double voltageAbsoluteTolerance = 1.0e-6;
    double currentAbsoluteTolerance = 1.0e-9;

    double minimumStep = 1.0e-15;

    double safetyFactor = 0.9;
    double minimumScale = 0.2;
    double maximumScale = 2.0;
    double convergenceFailureScale = 0.25;

    int maximumRejects = 10;

    bool validFor(double maximumIntegrationStep) const noexcept {
        return newtonOptions.valid() &&
            std::isfinite(maximumIntegrationStep) &&
            maximumIntegrationStep > 0.0 &&
            std::isfinite(relativeTolerance) &&
            relativeTolerance >= 0.0 &&
            std::isfinite(voltageAbsoluteTolerance) &&
            voltageAbsoluteTolerance > 0.0 &&
            std::isfinite(currentAbsoluteTolerance) &&
            currentAbsoluteTolerance > 0.0 &&
            std::isfinite(minimumStep) &&
            minimumStep > 0.0 &&
            minimumStep <= maximumIntegrationStep &&
            std::isfinite(safetyFactor) &&
            safetyFactor > 0.0 &&
            safetyFactor <= 1.0 &&
            std::isfinite(minimumScale) &&
            minimumScale > 0.0 &&
            minimumScale < 1.0 &&
            std::isfinite(maximumScale) &&
            maximumScale >= 1.0 &&
            maximumScale >= minimumScale &&
            std::isfinite(convergenceFailureScale) &&
            convergenceFailureScale > 0.0 &&
            convergenceFailureScale < 1.0 &&
            maximumRejects >= 0;
    }
};

struct TransientErrorEstimate {
    bool valid = false;
    double normalizedError = 0.0;
    double suggestedScale = 1.0;
};

// BDF2's local truncation error is a defect in the time derivative.  Circuit
// converts this defect into a state error through the converged MNA Jacobian.
struct TransientLteDefect {
    bool valid = false;
    Eigen::VectorXd derivativeDefect;
};

// Parsed .tran parameters plus transient solver configuration.
struct TransientAnalysisConfig {
    double outputInterval = 0.0;  // TSTEP
    double stopTime = 0.0;        // TSTOP
    double outputStartTime = 0.0; // TSTART
    std::optional<double> maximumStep; // TMAX
    bool useInitialConditions = false; // UIC
    TransientSolverOptions solverOptions;

    bool valid() const noexcept {
        const double maximumIntegrationStep = maximumStep
            ? *maximumStep
            : outputInterval;

        return std::isfinite(outputInterval) &&
            std::isfinite(stopTime) &&
            std::isfinite(outputStartTime) &&
            std::isfinite(maximumIntegrationStep) &&
            outputInterval > 0.0 &&
            maximumIntegrationStep > 0.0 &&
            outputStartTime >= 0.0 &&
            outputStartTime < stopTime &&
            stopTime > 0.0 &&
            solverOptions.validFor(maximumIntegrationStep);
    }
};

struct TransientDerivativeCoefficients {
    int order = 1;
    double alpha0 = 0.0;
    double alpha1 = 0.0;
    double alpha2 = 0.0;
};

struct TransientStampContext {
    double targetTime = 0.0;
    double timeStep = 0.0;
    TransientDerivativeCoefficients derivative;
    const Eigen::VectorXd& previousSolution;
    const Eigen::VectorXd* olderSolution = nullptr;
    double previousTime = 0.0;
    double olderTime = 0.0;

    double previousSolutionVal(int idx) const {
        if(idx < 0){
            return 0.0;
        }
        assert(idx < previousSolution.size());
        return previousSolution[idx];
    }

    double olderSolutionVal(int idx) const {
        if(idx < 0){
            return 0.0;
        }
        assert(olderSolution != nullptr);
        assert(idx < olderSolution->size());
        return (*olderSolution)[idx];
    }

    double historyDerivativeVal(int idx) const {
        double history = derivative.alpha1 * previousSolutionVal(idx);
        if(derivative.alpha2 != 0.0){
            history += derivative.alpha2 * olderSolutionVal(idx);
        }
        return history;
    }

    double historyDerivativeDifference(int p, int n) const {
        return historyDerivativeVal(p) - historyDerivativeVal(n);
    }
};

struct TransientLteContext {
    const Eigen::VectorXd& correctedSolution;
    const Eigen::VectorXd& derivativeDefect;
};

inline TransientErrorEstimate estimateTransientSolutionDifference(
    const Eigen::VectorXd& acceptedSolution,
    const Eigen::VectorXd& correctedSolution,
    const Eigen::VectorXd& referenceSolution,
    int voltageUnknownCount,
    const TransientSolverOptions& options,
    bool includeCurrentUnknowns = true
){
    TransientErrorEstimate result;

    const Eigen::Index size = correctedSolution.size();
    const bool optionsValid =
        std::isfinite(options.relativeTolerance) &&
        std::isfinite(options.voltageAbsoluteTolerance) &&
        std::isfinite(options.currentAbsoluteTolerance) &&
        std::isfinite(options.safetyFactor) &&
        std::isfinite(options.minimumScale) &&
        std::isfinite(options.maximumScale) &&
        options.relativeTolerance >= 0.0 &&
        options.voltageAbsoluteTolerance > 0.0 &&
        options.currentAbsoluteTolerance > 0.0 &&
        options.safetyFactor > 0.0 &&
        options.minimumScale > 0.0 &&
        options.maximumScale >= options.minimumScale;

    if(!optionsValid || size == 0 ||
       acceptedSolution.size() != size ||
       referenceSolution.size() != size ||
       voltageUnknownCount < 0 || voltageUnknownCount > size){
        return result;
    }

    double normalizedError = 0.0;

    const Eigen::Index evaluatedSize = includeCurrentUnknowns
        ? size : static_cast<Eigen::Index>(voltageUnknownCount);
    for(Eigen::Index i = 0; i < evaluatedSize; ++i){
        const double accepted = acceptedSolution[i];
        const double corrected = correctedSolution[i];
        const double reference = referenceSolution[i];

        if(!std::isfinite(accepted) ||
           !std::isfinite(corrected) ||
           !std::isfinite(reference)){
            return result;
        }

        const double absoluteTolerance =
            i < static_cast<Eigen::Index>(voltageUnknownCount)
            ? options.voltageAbsoluteTolerance
            : options.currentAbsoluteTolerance;

        const double weight = absoluteTolerance +
            options.relativeTolerance *
            std::max(std::abs(corrected), std::abs(accepted));
        const double difference = corrected - reference;

        if(!std::isfinite(weight) || weight <= 0.0 ||
           !std::isfinite(difference)){
            return result;
        }

        const double componentError = std::abs(difference) / weight;
        if(!std::isfinite(componentError)){
            return result;
        }

        normalizedError = std::max(normalizedError, componentError);
    }

    result.valid = true;
    result.normalizedError = normalizedError;

    if(normalizedError == 0.0){
        result.suggestedScale = options.maximumScale;
        return result;
    }

    result.suggestedScale = std::clamp(
        options.safetyFactor / std::sqrt(normalizedError),
        options.minimumScale,
        options.maximumScale
    );
    return result;
}

inline TransientErrorEstimate estimateTransientStateError(
    const Eigen::VectorXd& acceptedSolution,
    const Eigen::VectorXd& correctedSolution,
    const Eigen::VectorXd& localError,
    int voltageUnknownCount,
    const TransientSolverOptions& options,
    int localErrorOrder
){
    TransientErrorEstimate result;

    const Eigen::Index size = correctedSolution.size();
    const bool optionsValid =
        std::isfinite(options.relativeTolerance) &&
        std::isfinite(options.voltageAbsoluteTolerance) &&
        std::isfinite(options.currentAbsoluteTolerance) &&
        std::isfinite(options.safetyFactor) &&
        std::isfinite(options.minimumScale) &&
        std::isfinite(options.maximumScale) &&
        options.relativeTolerance >= 0.0 &&
        options.voltageAbsoluteTolerance > 0.0 &&
        options.currentAbsoluteTolerance > 0.0 &&
        options.safetyFactor > 0.0 &&
        options.minimumScale > 0.0 &&
        options.maximumScale >= options.minimumScale &&
        localErrorOrder > 0;

    if(!optionsValid || size == 0 ||
       acceptedSolution.size() != size ||
       localError.size() != size ||
       voltageUnknownCount < 0 || voltageUnknownCount > size){
        return result;
    }

    double normalizedError = 0.0;
    for(Eigen::Index i = 0; i < size; ++i){
        const double accepted = acceptedSolution[i];
        const double corrected = correctedSolution[i];
        const double error = localError[i];
        if(!std::isfinite(accepted) ||
           !std::isfinite(corrected) ||
           !std::isfinite(error)){
            return result;
        }

        const double absoluteTolerance =
            i < static_cast<Eigen::Index>(voltageUnknownCount)
            ? options.voltageAbsoluteTolerance
            : options.currentAbsoluteTolerance;
        const double weight = absoluteTolerance +
            options.relativeTolerance *
            std::max(std::abs(corrected), std::abs(accepted));
        if(!std::isfinite(weight) || weight <= 0.0){
            return result;
        }

        const double componentError = std::abs(error) / weight;
        if(!std::isfinite(componentError)){
            return result;
        }
        normalizedError = std::max(normalizedError, componentError);
    }

    result.valid = true;
    result.normalizedError = normalizedError;
    if(normalizedError == 0.0){
        result.suggestedScale = options.maximumScale;
        return result;
    }

    const double exponent = -1.0 / static_cast<double>(localErrorOrder);
    result.suggestedScale = std::clamp(
        options.safetyFactor * std::pow(normalizedError, exponent),
        options.minimumScale,
        options.maximumScale
    );
    return result;
}

enum class TransientStepAction {
    Accept,
    RetryConvergence,
    RetryError,
    FailInvalidEstimate,
    FailMinimumStep,
    FailRejectLimit
};

struct TransientStepControlInput {
    bool converged = false;
    bool requiresBdf2 = false;
    int integrationOrder = 1;
    bool errorEstimateValid = false;
    double normalizedError = 0.0;
    double suggestedStepScale = 1.0;

    double currentTime = 0.0;
    double attemptedStep = 0.0;
    double hardStepLimit = 0.0;
    int rejectedAttempts = 0;
};

struct TransientStepControlDecision {
    TransientStepAction action = TransientStepAction::FailMinimumStep;
    double nextStep = 0.0;
};

inline TransientStepControlDecision decideTransientStep(
    const TransientStepControlInput& input,
    const TransientSolverOptions& options
){
    const bool stepInputValid =
        std::isfinite(input.currentTime) &&
        std::isfinite(input.attemptedStep) &&
        std::isfinite(input.hardStepLimit) &&
        input.attemptedStep > 0.0 &&
        input.attemptedStep <= input.hardStepLimit &&
        input.rejectedAttempts >= 0;

    if(!stepInputValid){
        return {TransientStepAction::FailMinimumStep, 0.0};
    }

    const auto retry = [&](TransientStepAction action, double scale) {
        if(input.rejectedAttempts >= options.maximumRejects){
            return TransientStepControlDecision{
                TransientStepAction::FailRejectLimit,
                0.0
            };
        }

        if(!std::isfinite(scale) || scale <= 0.0 || scale >= 1.0){
            return TransientStepControlDecision{
                TransientStepAction::FailMinimumStep,
                0.0
            };
        }

        const double reducedStep = std::max(
            options.minimumStep,
            input.attemptedStep * scale
        );
        const double nextTime = input.currentTime + reducedStep;

        if(!std::isfinite(reducedStep) ||
           !std::isfinite(nextTime) ||
           reducedStep >= input.attemptedStep ||
           reducedStep > input.hardStepLimit ||
           nextTime <= input.currentTime){
            return TransientStepControlDecision{
                TransientStepAction::FailMinimumStep,
                0.0
            };
        }

        return TransientStepControlDecision{action, reducedStep};
    };

    if(!input.converged){
        return retry(
            TransientStepAction::RetryConvergence,
            options.convergenceFailureScale
        );
    }

    if(input.requiresBdf2 && input.integrationOrder != 2){
        return {TransientStepAction::FailInvalidEstimate, 0.0};
    }

    if(!input.errorEstimateValid ||
       !std::isfinite(input.normalizedError) ||
       input.normalizedError < 0.0 ||
       !std::isfinite(input.suggestedStepScale) ||
       input.suggestedStepScale <= 0.0){
        return {TransientStepAction::FailInvalidEstimate, 0.0};
    }

    if(input.normalizedError > 1.0){
        return retry(
            TransientStepAction::RetryError,
            input.suggestedStepScale
        );
    }

    const double recommendedStep =
        input.attemptedStep * input.suggestedStepScale;

    if(!std::isfinite(recommendedStep) || recommendedStep <= 0.0){
        return {TransientStepAction::FailInvalidEstimate, 0.0};
    }

    return {
        TransientStepAction::Accept,
        recommendedStep
    };
}

class TransientIntegrator {
public:
    explicit TransientIntegrator(
        double maximumBdf2StepRatio = 2.0
    ): maximumBdf2StepRatio_(maximumBdf2StepRatio) {}

    void initialize(
        double initialTime,
        const Eigen::VectorXd& initialSolution
    ){
        acceptedTime_ = initialTime;
        previousStep_ = 0.0;
        solutionN_ = initialSolution;
        initialized_ = true;
        hasOlderSolution_ = false;
        hasOldestSolution_ = false;
        previousPreviousStep_ = 0.0;
    }

    void restartFrom(
        double time,
        const Eigen::VectorXd& solution
    ){
        initialize(time, solution);
    }

    bool canUseBdf2(double targetTime) const {
        if(!initialized_ || !hasOlderSolution_ || previousStep_ <= 0.0){
            return false;
        }

        const double dt = targetTime - acceptedTime_;
        if(dt <= 0.0){
            return false;
        }

        return dt / previousStep_ <= maximumBdf2StepRatio_;
    }

    bool hasStrictBdf2LteHistory(double targetTime) const {
        return canUseBdf2(targetTime) &&
            hasOldestSolution_ &&
            std::isfinite(previousPreviousStep_) &&
            previousPreviousStep_ > 0.0;
    }

    TransientDerivativeCoefficients coefficients(double targetTime) const {
        const double dt = targetTime - acceptedTime_;
        assert(initialized_);
        assert(dt > 0.0);

        if(!canUseBdf2(targetTime)){
            return {
                1,
                1.0 / dt,
                -1.0 / dt,
                0.0
            };
        }

        const double ratio = dt / previousStep_;
        const double denominator = (1.0 + ratio) * dt;
        return {
            2,
            (1.0 + 2.0 * ratio) / denominator,
            -(1.0 + ratio) / dt,
            ratio * ratio / denominator
        };
    }

    Eigen::VectorXd predict(double targetTime) const {
        if(!canUseBdf2(targetTime)){
            return solutionN_;
        }

        const double ratio =
            (targetTime - acceptedTime_) / previousStep_;
        return solutionN_ + ratio * (solutionN_ - solutionNm1_);
    }

    TransientErrorEstimate estimateError(
        double targetTime,
        const Eigen::VectorXd& correctedSolution,
        int voltageUnknownCount,
        const TransientSolverOptions& options
    ) const{
        if(!initialized_ || !std::isfinite(targetTime) ||
           !canUseBdf2(targetTime)){
            return {};
        }

        // Retained for callers that explicitly request the historical
        // predictor/corrector proxy.  The transient solver itself uses
        // strictBdf2Defect() plus a DAE Jacobian projection instead.
        return estimateTransientSolutionDifference(
            solutionN_,
            correctedSolution,
            predict(targetTime),
            voltageUnknownCount,
            options,
            true
        );
    }

    TransientLteDefect strictBdf2Defect(
        double targetTime,
        const Eigen::VectorXd& correctedSolution
    ) const{
        TransientLteDefect result;
        if(!hasStrictBdf2LteHistory(targetTime) ||
           correctedSolution.size() == 0 ||
           correctedSolution.size() != solutionN_.size() ||
           solutionNm1_.size() != solutionN_.size() ||
           solutionNm2_.size() != solutionN_.size()){
            return result;
        }

        const double step = targetTime - acceptedTime_;
        const double denominator20 = step + previousStep_;
        const double denominator21 = previousStep_ + previousPreviousStep_;
        const double denominator3 = step + previousStep_ +
            previousPreviousStep_;
        const double defectScale = step * denominator20;
        if(!std::isfinite(step) || !std::isfinite(denominator20) ||
           !std::isfinite(denominator21) || !std::isfinite(denominator3) ||
           !std::isfinite(defectScale) || step <= 0.0 ||
           denominator20 <= 0.0 || denominator21 <= 0.0 ||
           denominator3 <= 0.0 || defectScale <= 0.0 ||
           !correctedSolution.allFinite() || !solutionN_.allFinite() ||
           !solutionNm1_.allFinite() || !solutionNm2_.allFinite()){
            return result;
        }

        const Eigen::VectorXd firstCurrent =
            (correctedSolution - solutionN_) / step;
        const Eigen::VectorXd firstPrevious =
            (solutionN_ - solutionNm1_) / previousStep_;
        const Eigen::VectorXd firstOlder =
            (solutionNm1_ - solutionNm2_) / previousPreviousStep_;
        const Eigen::VectorXd secondCurrent =
            (firstCurrent - firstPrevious) / denominator20;
        const Eigen::VectorXd secondPrevious =
            (firstPrevious - firstOlder) / denominator21;
        result.derivativeDefect = defectScale *
            (secondCurrent - secondPrevious) / denominator3;
        if(!result.derivativeDefect.allFinite()){
            result.derivativeDefect.resize(0);
            return result;
        }

        result.valid = true;
        return result;
    }

    TransientStampContext makeContext(double targetTime) const {
        return {
            targetTime,
            targetTime - acceptedTime_,
            coefficients(targetTime),
            solutionN_,
            hasOlderSolution_ ? &solutionNm1_ : nullptr,
            acceptedTime_,
            hasOlderSolution_ ? acceptedTime_ - previousStep_ : acceptedTime_
        };
    }

    void accept(
        double nextAcceptedTime,
        Eigen::VectorXd acceptedSolution
    ){
        assert(initialized_);
        assert(nextAcceptedTime > acceptedTime_);
        assert(solutionN_.size() == acceptedSolution.size());

        if(hasOlderSolution_){
            solutionNm2_ = solutionNm1_;
            previousPreviousStep_ = previousStep_;
            hasOldestSolution_ = true;
        }else{
            previousPreviousStep_ = 0.0;
            hasOldestSolution_ = false;
        }
        previousStep_ = nextAcceptedTime - acceptedTime_;
        acceptedTime_ = nextAcceptedTime;
        std::swap(solutionNm1_, solutionN_);
        solutionN_ = std::move(acceptedSolution);
        hasOlderSolution_ = true;
    }

    double acceptedTime() const noexcept {
        return acceptedTime_;
    }

    double previousStep() const noexcept {
        return previousStep_;
    }

    double maximumBdf2StepRatio() const noexcept {
        return maximumBdf2StepRatio_;
    }

    const Eigen::VectorXd& currentSolution() const noexcept {
        return solutionN_;
    }

    const Eigen::VectorXd* olderSolution() const noexcept {
        return hasOlderSolution_ ? &solutionNm1_ : nullptr;
    }

    const Eigen::VectorXd* oldestSolution() const noexcept {
        return hasOldestSolution_ ? &solutionNm2_ : nullptr;
    }

private:
    double acceptedTime_ = 0.0;
    double previousStep_ = 0.0;
    double previousPreviousStep_ = 0.0;
    Eigen::VectorXd solutionN_;
    Eigen::VectorXd solutionNm1_;
    Eigen::VectorXd solutionNm2_;
    bool hasOlderSolution_ = false;
    bool hasOldestSolution_ = false;
    bool initialized_ = false;
    double maximumBdf2StepRatio_ = 2.0;
};
