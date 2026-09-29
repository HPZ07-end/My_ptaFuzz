#pragma once

#include <algorithm>
#include <cmath>

#include <Eigen/Core>

// Unknowns and residual rows have different physical dimensions.  Node
// unknowns are voltages while branch unknowns are currents; conversely, node
// KCL residuals are currents while branch-constraint residuals are voltages.
struct NewtonUpdateEstimate {
    bool valid = false;
    double normalizedUpdate = 0.0;
};

struct NewtonResidualEstimate {
    bool valid = false;
    double normalizedResidual = 0.0;
};

inline NewtonUpdateEstimate estimateNormalizedNewtonUpdate(
    const Eigen::VectorXd& current,
    const Eigen::VectorXd& previous,
    int voltageUnknownCount,
    double relativeTolerance,
    double voltageAbsoluteTolerance,
    double currentAbsoluteTolerance
){
    NewtonUpdateEstimate result;
    const Eigen::Index size = current.size();
    if(size == 0 || previous.size() != size ||
       voltageUnknownCount < 0 || voltageUnknownCount > size ||
       !std::isfinite(relativeTolerance) || relativeTolerance < 0.0 ||
       !std::isfinite(voltageAbsoluteTolerance) ||
       voltageAbsoluteTolerance <= 0.0 ||
       !std::isfinite(currentAbsoluteTolerance) ||
       currentAbsoluteTolerance <= 0.0){
        return result;
    }

    double normalizedUpdate = 0.0;
    for(Eigen::Index i = 0; i < size; ++i){
        const double currentValue = current[i];
        const double previousValue = previous[i];
        if(!std::isfinite(currentValue) || !std::isfinite(previousValue)){
            return result;
        }

        const double absoluteTolerance = i < voltageUnknownCount
            ? voltageAbsoluteTolerance
            : currentAbsoluteTolerance;
        const double scale = std::max(
            std::abs(currentValue),
            std::abs(previousValue)
        );
        const double weight = absoluteTolerance + relativeTolerance * scale;
        const double component =
            std::abs(currentValue - previousValue) / weight;
        if(!std::isfinite(weight) || weight <= 0.0 ||
           !std::isfinite(component)){
            return result;
        }
        normalizedUpdate = std::max(normalizedUpdate, component);
    }

    result.valid = true;
    result.normalizedUpdate = normalizedUpdate;
    return result;
}

inline NewtonResidualEstimate estimateNormalizedNewtonResidual(
    const Eigen::VectorXd& residual,
    const Eigen::VectorXd& matrixProduct,
    const Eigen::VectorXd& rhs,
    int voltageUnknownCount,
    double relativeTolerance,
    double voltageAbsoluteTolerance,
    double currentAbsoluteTolerance
){
    NewtonResidualEstimate result;
    const Eigen::Index size = residual.size();
    if(size == 0 || matrixProduct.size() != size || rhs.size() != size ||
       voltageUnknownCount < 0 || voltageUnknownCount > size ||
       !std::isfinite(relativeTolerance) || relativeTolerance < 0.0 ||
       !std::isfinite(voltageAbsoluteTolerance) ||
       voltageAbsoluteTolerance <= 0.0 ||
       !std::isfinite(currentAbsoluteTolerance) ||
       currentAbsoluteTolerance <= 0.0){
        return result;
    }

    double normalizedResidual = 0.0;
    for(Eigen::Index i = 0; i < size; ++i){
        const double residualValue = residual[i];
        const double matrixValue = matrixProduct[i];
        const double rhsValue = rhs[i];
        if(!std::isfinite(residualValue) || !std::isfinite(matrixValue) ||
           !std::isfinite(rhsValue)){
            return result;
        }

        // KCL rows (one per node voltage) are currents.  Rows associated with
        // branch-current unknowns are voltage constraints.
        const double absoluteTolerance = i < voltageUnknownCount
            ? currentAbsoluteTolerance
            : voltageAbsoluteTolerance;
        const double scale = std::max(std::abs(matrixValue), std::abs(rhsValue));
        const double weight = absoluteTolerance + relativeTolerance * scale;
        const double component = std::abs(residualValue) / weight;
        if(!std::isfinite(weight) || weight <= 0.0 ||
           !std::isfinite(component)){
            return result;
        }
        normalizedResidual = std::max(normalizedResidual, component);
    }

    result.valid = true;
    result.normalizedResidual = normalizedResidual;
    return result;
}
