#pragma once

#include <cmath>
#include <algorithm>

// Device-specific voltage limiting used during nonlinear iteration.
inline double limitMosfetVoltage(double newV, double oldV, double threshold){
    const double absThreshold = std::abs(threshold);
    const double highStep = std::abs(2.0 * (oldV - absThreshold)) + 2.0;
    const double lowStep = 0.5 * highStep + 2.0;
    const double strongOn = absThreshold + 3.5;
    const double delta = newV - oldV;

    if(oldV >= absThreshold){
        if(oldV >= strongOn){
            if(delta <= 0.0){
                if(newV >= strongOn && -delta > lowStep){
                    return oldV - lowStep;
                }
                return std::max(newV, absThreshold + 2.0);
            }

            if(delta >= highStep){
                return oldV + highStep;
            }
            return newV;
        }

        if(delta <= 0.0){
            return std::max(newV, absThreshold - 0.5);
        }
        return std::min(newV, absThreshold + 4.0);
    }

    if(delta <= 0.0){
        if(-delta > highStep){
            return oldV - highStep;
        }
        return newV;
    }

    const double weakOn = absThreshold + 0.5;
    if(newV <= weakOn){
        if(delta > lowStep){
            return oldV + lowStep;
        }
        return newV;
    }

    return weakOn;
}

inline double pnJunctionCriticalVoltage(double vt, double is){
    return vt * std::log(vt / (is * std::sqrt(2)));
}

inline double limitPnJunctionVoltage(double newV, double oldV, double vt, double is){
    if(!std::isfinite(newV) || !std::isfinite(oldV) || vt <= 0.0 || is <= 0.0){
        return newV;
    }

    const double vcrit = pnJunctionCriticalVoltage(vt, is);

    if(newV <= vcrit)  return newV;

    const double oldArg = std::clamp(oldV / vt, -40.0, 40.0);
    const double oldExp = std::exp(oldArg);

    const double oldI = is * (oldExp - 1);
    const double oldG = is * oldExp / vt;

    const double newI = oldI + oldG * (newV - oldV);

    if(newI <= 0.0){
        return vcrit;
    }

    const double limited = vt * std::log1p(newI / is);

    return std::isfinite(limited) ? limited : vcrit;
}
