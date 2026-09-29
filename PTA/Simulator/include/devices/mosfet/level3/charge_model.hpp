#pragma once

#include <algorithm>
#include <array>
#include <cmath>

#include "devices/mosfet/level3/configuration.hpp"

namespace mos3 {

inline double evaluateJunctionCapacitance(
    double voltage,
    const JunctionCapacitanceConfig& config
){
    if((config.bottom <= 0.0 && config.sidewall <= 0.0) ||
       config.potential <= 0.0){
        return 0.0;
    }

    const double fc = std::clamp(config.forwardBiasCoeff, 0.0, 0.99);
    const double depletionVoltage = fc * config.potential;
    const auto depletion = [&](double capacitance, double grading){
        if(capacitance <= 0.0) return 0.0;
        const double arg = std::max(1.0 - voltage / config.potential, 1.0e-12);
        return capacitance * std::pow(arg, -grading);
    };

    if(voltage < depletionVoltage){
        return depletion(config.bottom, config.bottomGrading) +
            depletion(config.sidewall, config.sidewallGrading);
    }

    const double arg = std::max(1.0 - fc, 1.0e-12);
    const auto forward = [&](double capacitance, double grading){
        if(capacitance <= 0.0) return 0.0;
        const double scale = std::pow(arg, -grading);
        const double f2 = capacitance * (1.0 - fc * (1.0 + grading)) *
            scale / arg;
        const double f3 = capacitance * grading * scale / arg / config.potential;
        return f2 + voltage * f3;
    };
    return forward(config.bottom, config.bottomGrading) +
        forward(config.sidewall, config.sidewallGrading);
}

// Persistent q-state for charge companions.  The state is intentionally
// independent of MNA bindings so transient lifecycle logic can be tested and
// maintained without touching the MOSFET topology code.
class ChargeHistory {
public:
    static constexpr std::array<int, 3> kMeyerNegativeTerminals = {2, 0, 3};
    static constexpr std::array<int, 2> kJunctionTerminals = {0, 2};

    void initialize(const std::array<double, 3>& meyerCapacitances,
                    const std::array<double, 2>& junctionCapacitances,
                    const std::array<double, 4>& voltages){
        for(int pair = 0; pair < 3; ++pair){
            meyerN_[pair] = meyerCapacitances[pair] * (
                voltages[1] - voltages[kMeyerNegativeTerminals[pair]]
            );
        }
        meyerNm1_ = meyerN_;
        for(int junction = 0; junction < 2; ++junction){
            junctionN_[junction] = junctionCapacitances[junction] * (
                voltages[3] - voltages[kJunctionTerminals[junction]]
            );
        }
        junctionNm1_ = junctionN_;
    }

    void accept(const std::array<double, 3>& meyerCapacitances,
                const std::array<double, 2>& junctionCapacitances,
                const std::array<double, 4>& previousVoltages,
                const std::array<double, 4>& acceptedVoltages){
        auto nextMeyer = meyerN_;
        for(int pair = 0; pair < 3; ++pair){
            const int negative = kMeyerNegativeTerminals[pair];
            const double voltageChange = acceptedVoltages[1] - acceptedVoltages[negative] -
                previousVoltages[1] + previousVoltages[negative];
            nextMeyer[pair] += meyerCapacitances[pair] * voltageChange;
        }
        meyerNm1_ = meyerN_;
        meyerN_ = nextMeyer;

        auto nextJunction = junctionN_;
        for(int junction = 0; junction < 2; ++junction){
            const int terminal = kJunctionTerminals[junction];
            const double voltageChange = acceptedVoltages[3] - acceptedVoltages[terminal] -
                previousVoltages[3] + previousVoltages[terminal];
            nextJunction[junction] += junctionCapacitances[junction] * voltageChange;
        }
        junctionNm1_ = junctionN_;
        junctionN_ = nextJunction;
    }

    const std::array<double, 3>& meyerCurrent() const { return meyerN_; }
    const std::array<double, 3>& meyerPrevious() const { return meyerNm1_; }
    const std::array<double, 2>& junctionCurrent() const { return junctionN_; }
    const std::array<double, 2>& junctionPrevious() const { return junctionNm1_; }

private:
    std::array<double, 3> meyerN_ = {};
    std::array<double, 3> meyerNm1_ = {};
    std::array<double, 2> junctionN_ = {};
    std::array<double, 2> junctionNm1_ = {};
};

} // namespace mos3
