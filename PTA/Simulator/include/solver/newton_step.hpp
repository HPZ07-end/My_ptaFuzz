#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

#include <Eigen/Core>

// Bound a Newton update in place and report the effective infinity-norm
// change.  Keeping this numerical policy separate from Circuit lets the
// solver orchestration reuse it without creating a temporary step vector.
struct NewtonStepResult {
    double delta = 0.0;
    bool limited = false;
};

inline NewtonStepResult limitNewtonStep(
    Eigen::VectorXd& current,
    const Eigen::VectorXd& previous,
    double maximumStep
){
    if(current.size() != previous.size() ||
       !std::isfinite(maximumStep) || maximumStep <= 0.0){
        return {std::numeric_limits<double>::quiet_NaN(), false};
    }

    double rawDelta = 0.0;
    for(Eigen::Index i = 0; i < current.size(); ++i){
        const double difference = current[i] - previous[i];
        if(!std::isfinite(difference)){
            return {std::numeric_limits<double>::quiet_NaN(), false};
        }
        rawDelta = std::max(rawDelta, std::abs(difference));
    }

    if(!std::isfinite(rawDelta) || rawDelta <= maximumStep){
        return {rawDelta, false};
    }

    const double scale = maximumStep / rawDelta;
    for(Eigen::Index i = 0; i < current.size(); ++i){
        current[i] = previous[i] + (current[i] - previous[i]) * scale;
    }
    return {maximumStep, true};
}
