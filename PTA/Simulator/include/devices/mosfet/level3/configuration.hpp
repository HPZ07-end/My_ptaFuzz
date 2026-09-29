#pragma once

#include <algorithm>
#include <optional>

#include "devices/mosfet/instance_parameters.hpp"
#include "models/model.hpp"

namespace mos3 {

inline double valueOr(const std::optional<double>& value, double fallback = 0.0){
    return value ? *value : fallback;
}

struct Geometry {
    double length = 0.0;
    double width = 0.0;

    bool valid() const { return length > 0.0 && width > 0.0; }
};

inline Geometry effectiveGeometry(const Model& model,
                                  const MosInstanceParams& instance){
    const auto& card = model.mos3();
    return {
        instance.l - 2.0 * valueOr(card.ld) + valueOr(card.xl),
        instance.w - 2.0 * valueOr(card.wd) + valueOr(card.xw)
    };
}

inline double multiplier(const MosInstanceParams& instance){
    return std::max(instance.m, 1.0e-30);
}

inline double seriesResistance(const Model& model,
                               const MosInstanceParams& instance,
                               bool drain){
    if(!model.isMos3()) return 0.0;
    const auto& card = model.mos3();
    const double explicitResistance = valueOr(drain ? card.rd : card.rs);
    if(explicitResistance > 0.0){
        return explicitResistance / multiplier(instance);
    }
    const double sheetResistance = valueOr(card.rsh);
    const double squares = drain ? instance.nrd : instance.nrs;
    return sheetResistance > 0.0 && squares > 0.0
        ? sheetResistance * squares / multiplier(instance)
        : 0.0;
}

struct OverlapCapacitances {
    double cgs = 0.0;
    double cgd = 0.0;
    double cgb = 0.0;
};

inline OverlapCapacitances overlapCapacitances(
    const Model& model,
    const MosInstanceParams& instance
){
    const Geometry geometry = effectiveGeometry(model, instance);
    if(!geometry.valid()) return {};

    const auto& card = model.mos3();
    const double m = multiplier(instance);
    return {
        valueOr(card.cgso) * m * geometry.width,
        valueOr(card.cgdo) * m * geometry.width,
        valueOr(card.cgbo) * m * geometry.length
    };
}

struct JunctionCapacitanceConfig {
    double bottom = 0.0;
    double sidewall = 0.0;
    double potential = 0.8;
    double bottomGrading = 0.5;
    double sidewallGrading = 0.33;
    double forwardBiasCoeff = 0.5;
};

inline JunctionCapacitanceConfig junctionCapacitanceConfig(
    const Model& model,
    const MosInstanceParams& instance,
    bool drain
){
    const auto& card = model.mos3();
    const double m = multiplier(instance);
    const double area = drain ? instance.ad : instance.as;
    const double perimeter = drain ? instance.pd : instance.ps;
    const std::optional<double>& explicitCapacitance = drain ? card.cbd : card.cbs;
    return {
        explicitCapacitance
            ? *explicitCapacitance * m
            : valueOr(card.cj) * std::max(area, 0.0) * m,
        valueOr(card.cjsw) * std::max(perimeter, 0.0) * m,
        valueOr(card.pb) > 0.0 ? valueOr(card.pb) : 0.8,
        valueOr(card.mj, 0.5),
        valueOr(card.mjsw, 0.33),
        valueOr(card.fc, 0.5)
    };
}

} // namespace mos3
