#pragma once

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <utility>
#include <optional>

#include "models/mosfet/level3_model_card.hpp"

enum class ModelType {
    Diode,
    NPN,
    PNP,
    NMOS,
    PMOS,
    Unknown
};

enum class MosLevel {
    Level1 = 1,
    Level3 = 3
};

class Model {
public:
    using Parameters = std::unordered_map<std::string, double>;
    using Mos3CardParams = ::Mos3CardParams;

    MosLevel mosLevel() const { return mosLevel_; }

    bool isMos3() const {
        return isMosfet() && mosLevel_ == MosLevel::Level3;
    }

    const Mos3CardParams& mos3() const {
        return mos3_;
    }

    std::optional<double> suppliedParam(const std::string& key) const {
        const auto it = params_.find(key);
        if(it == params_.end()){
            return std::nullopt;
        }
        return it->second;
    }

    Model(std::string n, ModelType t, Parameters params = {}):
        name_(std::move(n)),
        type_(t),
        params_(std::move(params)) {
        rebuildDcCache();
    }

    const std::string& name() const { return name_; }
    ModelType type() const { return type_; }
    const Parameters& parameters() const { return params_; }

    void setParam(std::string key, double value) {
        params_[std::move(key)] = value;
        rebuildDcCache();
    }

    double param(const std::string& key, double fallback) const {
        auto it = params_.find(key);
        return it == params_.end() ? fallback : it->second;
    }

    bool isDiode() const {
        return type_ == ModelType::Diode;
    }

    bool isBjt() const {
        return type_ == ModelType::NPN || type_ == ModelType::PNP;
    }

    bool isMosfet() const {
        return type_ == ModelType::NMOS || type_ == ModelType::PMOS;
    }

    struct DiodeDcParams {
        double is = 1.0e-14;
        double n = 1.0;
        double vt = 0.025852;
        double rs = 0.0;
        double gmin = 1.0e-12;
    };

    struct BjtDcParams {
        double is = 1.0e-16;
        double bf = 100.0;
        double br = 1.0;
        double nf = 1.0;
        double nr = 1.0;
        double vt = 0.025852;
        double gmin = 1.0e-12;
        double rb = 0.0;
        double rc = 0.0;
        double re = 0.0;
        double rbe = 0.0;
        double rce = 0.0;
        double va = 0.0;
        double var = 0.0;
        double ikf = 0.0;
        double ikr = 0.0;
        double ise = 0.0;
        double isc = 0.0;
        double ne = 1.5;
        double nc = 2.0;
    };

    struct MosDcParams {
        double vto = 0.7;
        double kp = 20.0e-6;
        double lambda = 0.0;
        double gmin = 1.0e-12;
        double rds = 0.0;
    };

    const DiodeDcParams& diodeDc() const {
        return diodeDc_;
    }

    const BjtDcParams& bjtDc() const {
        return bjtDc_;
    }

    const MosDcParams& mosDc() const {
        return mosDc_;
    }

    double diodeConductance(double area) const {
        return positive(area, 1.0) / dc_.diodeRs;
    }

    double bjtBaseEmitterConductance(double area) const {
        return positive(area, 1.0) / dc_.bjtRbe;
    }

    double bjtCollectorEmitterConductance(double area) const {
        return positive(area, 1.0) / dc_.bjtRce;
    }

    double mosDrainSourceConductance(double w, double l) const {
        const double width = positive(w, 1.0);
        const double length = positive(l, 1.0);
        return (width / length) / dc_.mosRds;
    }

private:
    struct DcCache {
        double diodeRs = 1.0e12;
        double bjtRbe = 1.0e12;
        double bjtRce = 1.0e12;
        double mosRds = 1.0e12;
    };

    static double positive(double value, double fallback) {
        return value > 0.0 && std::isfinite(value) ? value : fallback;
    }

    static double nonNegative(double value, double fallback) {
        return value >= 0.0 && std::isfinite(value) ? value : fallback;
    }

    void rebuildDcCache() {
        mosLevel_ = param("level", 1.0) == 3.0 ?
            MosLevel::Level3 : MosLevel::Level1;

        rebuildMos3Card();

        dc_.diodeRs = positive(param("rs", 1.0e12), 1.0e12);
        dc_.bjtRbe = positive(param("rbe", 1.0e12), 1.0e12);
        dc_.bjtRce = positive(param("rce", 1.0e12), 1.0e12);
        dc_.mosRds = positive(param("rds", 1.0e12), 1.0e12);

        diodeDc_.is = positive(param("is", diodeDc_.is), 1.0e-14);
        diodeDc_.n = positive(param("n", diodeDc_.n), 1.0);
        diodeDc_.vt = positive(param("vt", diodeDc_.vt), 0.025852);
        diodeDc_.rs = positive(param("rs", diodeDc_.rs), 0.0);
        diodeDc_.gmin = positive(param("gmin", diodeDc_.gmin), 1.0e-12);

        bjtDc_.is = positive(param("is", bjtDc_.is), 1.0e-16);
        bjtDc_.bf = positive(param("bf", param("beta", bjtDc_.bf)), 100.0);
        bjtDc_.br = positive(param("br", bjtDc_.br), 1.0);
        bjtDc_.nf = positive(param("nf", bjtDc_.nf), 1.0);
        bjtDc_.nr = positive(param("nr", bjtDc_.nr), 1.0);
        bjtDc_.vt = positive(param("vt", bjtDc_.vt), 0.025852);
        bjtDc_.gmin = positive(param("gmin", bjtDc_.gmin), 1.0e-12);
        bjtDc_.rb = nonNegative(param("rb", bjtDc_.rb), 0.0);
        bjtDc_.rc = nonNegative(param("rc", bjtDc_.rc), 0.0);
        bjtDc_.re = nonNegative(param("re", bjtDc_.re), 0.0);
        bjtDc_.rbe = nonNegative(param("rbe", bjtDc_.rbe), 0.0);
        bjtDc_.rce = nonNegative(param("rce", bjtDc_.rce), 0.0);
        bjtDc_.va = nonNegative(
            param("vaf", param("va", bjtDc_.va)),
            0.0
        );
        bjtDc_.var = nonNegative(param("var", bjtDc_.var), 0.0);
        bjtDc_.ikf = nonNegative(param("ikf", bjtDc_.ikf), 0.0);
        bjtDc_.ikr = nonNegative(param("ikr", bjtDc_.ikr), 0.0);
        bjtDc_.ise = nonNegative(param("ise", bjtDc_.ise), 0.0);
        bjtDc_.isc = nonNegative(param("isc", bjtDc_.isc), 0.0);
        bjtDc_.ne = positive(param("ne", bjtDc_.ne), 1.5);
        bjtDc_.nc = positive(param("nc", bjtDc_.nc), 2.0);

        if(isMosfet() && !isMos3()){
            const double defaultVto = type_ == ModelType::PMOS ? -0.7 : 0.7;
            mosDc_.vto = param("vto", param("vt0", defaultVto));
            mosDc_.kp = positive(param("kp", param("k", mosDc_.kp)), 20.0e-6);
            mosDc_.lambda = positive(param("lambda", param("lam", mosDc_.lambda)), 0.0);
            mosDc_.gmin = positive(param("gmin", mosDc_.gmin), 1.0e-12);
            mosDc_.rds = nonNegative(param("rds", mosDc_.rds), 0.0);
        }

        const double is = positive(param("is", 0.0), 0.0);
        if(is > 0.0 && params_.find("rs") == params_.end()){
            dc_.diodeRs = std::min(dc_.diodeRs, 0.026 / is);
        }

        const double beta = positive(param("bf", param("beta", 0.0)), 0.0);
        if(is > 0.0 && beta > 0.0 && params_.find("rbe") == params_.end()){
            dc_.bjtRbe = std::min(dc_.bjtRbe, 0.026 / (is * beta));
        }
    }

    void rebuildMos3Card() {
        mos3_ = isMos3() ? mos3::parseModelCard(params_) : Mos3CardParams{};
    }

    std::string name_;
    ModelType type_;
    Parameters params_;
    DcCache dc_;
    DiodeDcParams diodeDc_;
    BjtDcParams bjtDc_;
    MosDcParams mosDc_;
    MosLevel mosLevel_ = MosLevel::Level1;
    Mos3CardParams mos3_;
};
