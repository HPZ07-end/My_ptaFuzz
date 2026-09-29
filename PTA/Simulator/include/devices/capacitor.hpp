#pragma once

#include <cassert>
#include <optional>

#include "analysis/transient_analysis.hpp"
#include "devices/device.hpp"

class Capacitor: public Device{
public:
    Capacitor(std::string name, std::vector<std::string> nodes, double C,
              std::optional<double> initialVoltage = std::nullopt):
            Device(name, nodes, DeviceType::Capacitor), capacitance_(C),
            initialVoltage_(initialVoltage) {
                assert(C > 0.0);
            }

    std::vector<InitialVoltageConstraint>
    initialVoltageConstraints() const override{
        if(!initialVoltage_){
            return {};
        }
        return {{0, 1, *initialVoltage_}};
    }

    void pattern(MNA& mna) override{
        const int p = nodeIds[0];
        const int n = nodeIds[1];

        if(p >= 0){
            mna.addPattern(p, p);       
        }
        if(n >= 0){
            mna.addPattern(n, n);
        }
        if(p >= 0 && n >= 0){
            mna.addPattern(p, n);
            mna.addPattern(n, p);
        }
    }

    void bindMatrix(MNA& mna) override{
        const int p = nodeIds[0];
        const int n = nodeIds[1];

        if(p >= 0){
            aPp_ = mna.ptr(p, p);
            rhsP_ = &mna.rhs(p);
        }
        if(n >= 0){
            aNn_ = mna.ptr(n, n);
            rhsN_ = &mna.rhs(n);
        }
        if(p >=0 && n >= 0){
            aPn_ = mna.ptr(p, n);
            aNp_ = mna.ptr(n, p);
        }
    }

    void stampOperatingPoint() override{}

    void stampTransient(const TransientStampContext& ctx) override{
        const int p = nodeIds[0];
        const int n = nodeIds[1];

        const double g = capacitance_ * ctx.derivative.alpha0;
        const double history = capacitance_ *
                    ctx.historyDerivativeDifference(p, n);

        if(aPp_) *aPp_ += g;
        if(aNn_) *aNn_ += g;
        if(aPn_) *aPn_ -= g;
        if(aNp_) *aNp_ -= g;

        if(rhsP_) *rhsP_ -= history;
        if(rhsN_) *rhsN_ += history;
    }

    void stampTransientLteDefect(
        const TransientLteContext& ctx,
        Eigen::VectorXd& residual
    ) const override{
        const int p = nodeIds[0];
        const int n = nodeIds[1];
        const double positiveDerivative = p >= 0 ? ctx.derivativeDefect[p] : 0.0;
        const double negativeDerivative = n >= 0 ? ctx.derivativeDefect[n] : 0.0;
        const double current = capacitance_ *
            (positiveDerivative - negativeDerivative);

        if(p >= 0) residual[p] += current;
        if(n >= 0) residual[n] -= current;
    }

private:
    double capacitance_;
    std::optional<double> initialVoltage_;

    double* aPp_ = nullptr;
    double* aPn_ = nullptr;
    double* aNp_ = nullptr;
    double* aNn_ = nullptr;

    double* rhsP_ = nullptr;
    double* rhsN_ = nullptr;
};
