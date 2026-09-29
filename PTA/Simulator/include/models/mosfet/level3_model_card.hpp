#pragma once

#include <optional>
#include <string>
#include <unordered_map>

// Parsed MOSFET Level-3 model-card configuration. Keep this separate from Model's
// common DC caches so MOS3 parameter aliases and defaults have one owner.
struct Mos3CardParams {
    std::optional<double> vto;
    std::optional<double> kp;
    std::optional<double> gamma;
    std::optional<double> phi;

    std::optional<double> rd;
    std::optional<double> rs;
    std::optional<double> rsh;

    std::optional<double> cbd;
    std::optional<double> cbs;
    std::optional<double> is;
    std::optional<double> js;
    std::optional<double> pb;
    std::optional<double> fc;
    std::optional<double> cj;
    std::optional<double> mj;
    std::optional<double> cjsw;
    std::optional<double> mjsw;

    std::optional<double> cgso;
    std::optional<double> cgdo;
    std::optional<double> cgbo;

    std::optional<double> tox;
    std::optional<double> ld;
    std::optional<double> xl;
    std::optional<double> xw;
    std::optional<double> wd;
    std::optional<double> uo;
    std::optional<double> nsub;
    std::optional<double> tpg;
    std::optional<double> nss;
    std::optional<double> vmax;
    std::optional<double> xj;
    std::optional<double> nfs;

    std::optional<double> eta;
    std::optional<double> delta;
    std::optional<double> theta;
    std::optional<double> kappa;

    std::optional<double> tnom;
    std::optional<double> kf;
    std::optional<double> af;
};

namespace mos3 {

using ParameterMap = std::unordered_map<std::string, double>;

inline std::optional<double> suppliedParameter(
    const ParameterMap& parameters,
    const std::string& primary,
    const std::string& alias = {}
){
    const auto value = parameters.find(primary);
    if(value != parameters.end()) return value->second;
    if(alias.empty()) return std::nullopt;

    const auto aliasValue = parameters.find(alias);
    return aliasValue == parameters.end()
        ? std::nullopt
        : std::optional<double>(aliasValue->second);
}

inline Mos3CardParams parseModelCard(const ParameterMap& parameters){
    Mos3CardParams card;
    const auto param = [&](const std::string& key){
        return suppliedParameter(parameters, key);
    };

    card.vto = suppliedParameter(parameters, "vto", "vt0");
    card.kp = suppliedParameter(parameters, "kp", "k");
    card.gamma = param("gamma");
    card.phi = param("phi");

    card.rd = param("rd");
    card.rs = param("rs");
    card.rsh = param("rsh");

    card.cbd = param("cbd");
    card.cbs = param("cbs");
    card.is = param("is");
    card.js = param("js");
    card.pb = param("pb");
    card.fc = param("fc");
    card.cj = param("cj");
    card.mj = param("mj");
    card.cjsw = param("cjsw");
    card.mjsw = param("mjsw");

    card.cgso = param("cgso");
    card.cgdo = param("cgdo");
    card.cgbo = param("cgbo");

    card.tox = param("tox");
    card.ld = param("ld");
    card.xl = param("xl");
    card.xw = param("xw");
    card.wd = param("wd");
    card.uo = suppliedParameter(parameters, "uo", "u0");
    card.nsub = param("nsub");
    card.tpg = param("tpg");
    card.nss = param("nss");
    card.vmax = param("vmax");
    card.xj = param("xj");
    card.nfs = param("nfs");

    card.eta = param("eta");
    card.delta = param("delta");
    card.theta = param("theta");
    card.kappa = param("kappa");

    card.tnom = param("tnom");
    card.kf = param("kf");
    card.af = param("af");
    return card;
}

} // namespace mos3
