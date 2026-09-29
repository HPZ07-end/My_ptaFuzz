#pragma once

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "devices/mosfet/level3/configuration.hpp"

// ngspice Level-3 channel-current path. Junction and terminal charge companion
// models are stamped by MOSFET using the operating quantities exposed here.
struct Mos3DcResult {
    double ids = 0.0;
    double gm = 0.0;
    double gds = 0.0;
    double gmb = 0.0;
};

struct Mos3MeyerCapacitances {
    double cgs = 0.0;
    double cgd = 0.0;
    double cgb = 0.0;
};

namespace mos3_detail {

constexpr double kEps0 = 8.854214871e-12;
constexpr double kEpsSi = 11.7 * kEps0;
constexpr double kCharge = 1.6021918e-19;
constexpr double kPi = 3.14159265358979323846;
constexpr double kThermalVoltage = 0.02586419;
constexpr double kIntrinsicCarrierDensity = 1.45e16;

using mos3::valueOr;

struct Parameters {
    double polarity = 1.0;
    double cox = 0.0;
    double beta0 = 0.0;
    double vto = 0.7;
    double uo = 600.0;
    double gamma = 0.0;
    double phi = 0.6;
    double eta = 0.0;
    double theta = 0.0;
    double vmax = 0.0;
    double kappa = 0.2;
    double alpha = 0.0;
    double depletionWidth = 0.0;
    double junctionDepth = 0.0;
    double lateralDiffusion = 0.0;
    double narrowFactor = 0.0;
    double leff = 0.0;
    double weff = 0.0;
    double multiplier = 1.0;
};

struct ChannelState {
    double threshold = 0.0;
    double vdsat = 0.0;
    bool forward = true;
};

inline Parameters resolve(const Model& model,
                          const MosInstanceParams& instance){
    const auto& card = model.mos3();
    const double tox = valueOr(card.tox, 1.0e-7);
    if(tox <= 0.0){
        throw std::runtime_error("MOS3 TOX must be positive");
    }

    Parameters p;
    p.polarity = model.type() == ModelType::PMOS ? -1.0 : 1.0;
    p.cox = 3.9 * kEps0 / tox;
    p.gamma = valueOr(card.gamma, 0.0);
    p.phi = valueOr(card.phi, 0.6);
    p.vto = std::abs(valueOr(
        card.vto,
        p.polarity > 0.0 ? 0.7 : -0.7
    ));
    p.uo = valueOr(card.uo, 600.0);
    p.theta = valueOr(card.theta, 0.0);
    p.vmax = valueOr(card.vmax, 0.0);
    p.kappa = valueOr(card.kappa, 0.2);
    p.junctionDepth = valueOr(card.xj, 0.0);
    p.multiplier = mos3::multiplier(instance);

    p.lateralDiffusion = valueOr(card.ld);
    const mos3::Geometry geometry = mos3::effectiveGeometry(model, instance);
    p.leff = geometry.length;
    p.weff = geometry.width;
    if(!geometry.valid()){
        throw std::runtime_error("MOS3 effective channel geometry must be positive");
    }

    p.beta0 = valueOr(card.kp, p.uo * p.cox * 1.0e-4) *
        p.multiplier * p.weff / p.leff;

    if(card.nsub && *card.nsub > 0.0){
        if(!card.phi){
            p.phi = std::max(
                0.1,
                2.0 * kThermalVoltage * std::log(
                    *card.nsub * 1.0e6 / kIntrinsicCarrierDensity
                )
            );
        }
        p.alpha = 2.0 * kEpsSi / (kCharge * *card.nsub * 1.0e6);
        p.depletionWidth = std::sqrt(p.alpha);
        if(!card.gamma){
            p.gamma = std::sqrt(
                2.0 * kEpsSi * kCharge * *card.nsub * 1.0e6
            ) / p.cox;
        }
    }
    p.narrowFactor = valueOr(card.delta, 0.0) * 0.5 * kPi * kEpsSi / p.cox;
    p.eta = valueOr(card.eta, 0.0) * 8.15e-22 /
        (p.cox * p.leff * p.leff * p.leff);
    return p;
}

inline double channelCurrent(const Parameters& p,
                             double vd,
                             double vg,
                             double vs,
                             double vb,
                             ChannelState* state = nullptr){
    const double vgs = p.polarity * (vg - vs);
    const double vds = p.polarity * (vd - vs);
    const double vbs = p.polarity * (vb - vs);
    const double mode = vds >= 0.0 ? 1.0 : -1.0;
    const double vdsAbs = mode * vds;
    const double vgsControl = mode > 0.0 ? vgs : vgs - vds;
    const double vbsControl = mode > 0.0 ? vbs : vbs - vds;

    double phibs = 0.0;
    double sqphibs = 0.0;
    if(vbsControl <= 0.0){
        phibs = p.phi - vbsControl;
        sqphibs = std::sqrt(std::max(phibs, 1.0e-12));
    } else {
        const double sqphi = std::sqrt(p.phi);
        sqphibs = sqphi / (1.0 + vbsControl / (2.0 * p.phi));
        phibs = sqphibs * sqphibs;
    }

    double fshort = 1.0;
    if(p.junctionDepth > 0.0 && p.depletionWidth > 0.0){
        const double wp = p.depletionWidth * sqphibs;
        const double xjonl = p.junctionDepth / p.leff;
        const double djonxj = p.lateralDiffusion / p.junctionDepth;
        const double wponxj = wp / p.junctionDepth;
        const double wconxj = 0.0631353 + 0.8013292 * wponxj -
            0.01110777 * wponxj * wponxj;
        const double a = wconxj + djonxj;
        const double c = wponxj / (1.0 + wponxj);
        fshort = 1.0 - xjonl * (a * std::sqrt(std::max(1.0 - c * c, 0.0)) - djonxj);
    }

    const double gamma = p.gamma * fshort;
    const double qbonco = gamma * sqphibs + p.narrowFactor * phibs / p.weff;
    const double vbi = p.vto - p.gamma * std::sqrt(p.phi);
    const double vth = vbi - p.eta * vdsAbs + qbonco;
    if(state){
        state->threshold = vth;
        state->forward = mode > 0.0;
    }
    if(vgsControl <= vth){
        return 0.0;
    }

    const double fbody = 0.25 * gamma / sqphibs + p.narrowFactor / p.weff;
    const double onfbdy = 1.0 / (1.0 + fbody);
    const double vgt = vgsControl - vth;
    const double onfg = std::max(1.0 + p.theta * vgt, 1.0e-12);
    const double fgate = 1.0 / onfg;
    const double mobility = p.uo * 1.0e-4 * fgate;

    double vdsat = vgt * onfbdy;
    double vdsc = 0.0;
    if(p.vmax > 0.0){
        vdsc = p.leff * p.vmax / mobility;
        const double a = vgt * onfbdy;
        vdsat = a + vdsc - std::sqrt(a * a + vdsc * vdsc);
    }
    vdsat = std::max(vdsat, 0.0);
    if(state) state->vdsat = vdsat;
    const double vdsx = std::min(vdsAbs, vdsat);
    const double cdo = vgt - 0.5 * (1.0 + fbody) * vdsx;
    double current = p.beta0 * fgate * std::max(cdo * vdsx, 0.0);

    double fdrain = 1.0;
    if(p.vmax > 0.0 && vdsc > 0.0){
        fdrain = 1.0 / (1.0 + vdsx / vdsc);
        current *= fdrain;
    }

    if(p.alpha > 0.0 && p.kappa > 0.0 && vdsat > 0.0){
        double deltaLength = 0.0;
        if(vdsAbs <= vdsat){
            // ngspice MOS3 retains the modern (non-badmos3) punch-through
            // correction below saturation as well.  Omitting this branch
            // makes KAPPA ineffective for long-channel devices biased just
            // below VDSAT.
            const double normalizedVds = vdsAbs / vdsat;
            deltaLength = std::sqrt(p.kappa * p.alpha * vdsat / 8.0) *
                normalizedVds * normalizedVds * normalizedVds * normalizedVds;
        } else if(p.vmax > 0.0 && vdsc > 0.0 && current > 0.0){
            const double gdsat = std::max(
                1.0e-12,
                current * (1.0 - fdrain) / vdsc
            );
            const double emax = p.kappa * current / (p.leff * gdsat);
            const double a = 0.5 * emax * p.alpha;
            deltaLength = std::sqrt(
                a * a + p.kappa * p.alpha * (vdsAbs - vdsat)
            ) - a;
        } else {
            deltaLength = std::sqrt(
                p.kappa * p.alpha * std::max(vdsAbs - vdsat + vdsat / 8.0, 0.0)
            );
        }
        if(deltaLength > 0.5 * p.leff){
            deltaLength = p.leff - p.leff * p.leff / (4.0 * deltaLength);
        }
        if(deltaLength < p.leff){
            current /= 1.0 - deltaLength / p.leff;
        }
    }
    return p.polarity * mode * current;
}

inline Mos3MeyerCapacitances qMeyer(double vgs,
                                    double vgd,
                                    double von,
                                    double vdsat,
                                    double phi,
                                    double oxideCapacitance){
    // This is DEVqmeyer from ngspice's device support code.  Its outputs are
    // the non-constant half-capacitances; combining adjacent accepted states
    // yields the physical small-signal capacitance used by MOS3load.
    constexpr double minimumVdsat = 0.025;
    const double vgst = vgs - von;
    vdsat = std::max(vdsat, minimumVdsat);

    Mos3MeyerCapacitances result;
    if(vgst <= -phi){
        result.cgb = oxideCapacitance / 2.0;
        return result;
    }
    if(vgst <= -phi / 2.0){
        result.cgb = -vgst * oxideCapacitance / (2.0 * phi);
        return result;
    }
    if(vgst <= 0.0){
        result.cgb = -vgst * oxideCapacitance / (2.0 * phi);
        result.cgs = vgst * oxideCapacitance / (1.5 * phi) +
            oxideCapacitance / 3.0;
        const double vds = vgs - vgd;
        if(vds < vdsat){
            const double difference = 2.0 * vdsat - vds;
            const double differenceSquared = difference * difference;
            result.cgd = result.cgs * (
                1.0 - vdsat * vdsat / differenceSquared
            );
            result.cgs *= 1.0 - (vdsat - vds) * (vdsat - vds) /
                differenceSquared;
        }
        return result;
    }

    const double vds = vgs - vgd;
    if(vdsat <= vds){
        result.cgs = oxideCapacitance / 3.0;
        return result;
    }
    const double difference = 2.0 * vdsat - vds;
    const double differenceSquared = difference * difference;
    result.cgd = oxideCapacitance * (
        1.0 - vdsat * vdsat / differenceSquared
    ) / 3.0;
    result.cgs = oxideCapacitance * (
        1.0 - (vdsat - vds) * (vdsat - vds) / differenceSquared
    ) / 3.0;
    return result;
}

} // namespace mos3_detail

inline Mos3DcResult evaluateMos3Dc(double vd,
                                   double vg,
                                   double vs,
                                   double vb,
                                   const Model& model,
                                   const MosInstanceParams& instance){
    const auto parameters = mos3_detail::resolve(model, instance);
    const auto current = [&](double d, double g, double s, double b){
        return mos3_detail::channelCurrent(parameters, d, g, s, b);
    };
    const double ids = current(vd, vg, vs, vb);
    constexpr double h = 1.0e-6;
    const auto derivative = [&](double plus, double minus){
        return (plus - minus) / (2.0 * h);
    };
    return {
        ids,
        derivative(current(vd, vg + h, vs, vb), current(vd, vg - h, vs, vb)),
        derivative(current(vd + h, vg, vs, vb), current(vd - h, vg, vs, vb)),
        derivative(current(vd, vg, vs, vb + h), current(vd, vg, vs, vb - h))
    };
}

inline Mos3MeyerCapacitances evaluateMos3MeyerCapacitances(
    double vd,
    double vg,
    double vs,
    double vb,
    const Model& model,
    const MosInstanceParams& instance
){
    const auto parameters = mos3_detail::resolve(model, instance);
    mos3_detail::ChannelState state;
    static_cast<void>(mos3_detail::channelCurrent(
        parameters, vd, vg, vs, vb, &state
    ));

    const double vgs = parameters.polarity * (vg - vs);
    const double vgd = parameters.polarity * (vg - vd);
    const double oxideCapacitance = parameters.cox * parameters.leff *
        parameters.weff * parameters.multiplier;
    Mos3MeyerCapacitances result = state.forward
        ? mos3_detail::qMeyer(
            vgs, vgd, state.threshold, state.vdsat, parameters.phi,
            oxideCapacitance
        )
        : mos3_detail::qMeyer(
            vgd, vgs, state.threshold, state.vdsat, parameters.phi,
            oxideCapacitance
        );

    if(!state.forward) std::swap(result.cgs, result.cgd);

    // MOS3load sums the current and prior qMeyer values.  The device API
    // stamps a capacitance frozen at the accepted state, so double qMeyer to
    // recover that same stationary small-signal capacitance.
    result.cgs *= 2.0;
    result.cgd *= 2.0;
    result.cgb *= 2.0;
    return result;
}
