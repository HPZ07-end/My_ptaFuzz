#include "devices/mosfet/level3/dc_model.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

int checks = 0;
int failures = 0;

void expect(bool condition, const std::string& description){
    ++checks;
    if(!condition){
        ++failures;
        std::cerr << "FAIL: " << description << '\n';
    }
}

void expectNear(double actual, double expected, const std::string& description){
    const double tolerance = 2.0e-4 *
        std::max({1.0e-9, std::abs(actual), std::abs(expected)});
    expect(
        std::isfinite(actual) && std::abs(actual - expected) <= tolerance,
        description
    );
}

Model makeNmos(){
    return Model("nmos3", ModelType::NMOS, {
        {"level", 3.0}, {"tox", 20.0e-9}, {"vto", 0.7},
        {"kp", 100.0e-6}, {"gamma", 0.5}, {"phi", 0.7},
        {"theta", 0.1}, {"eta", 1.0e-6}, {"vmax", 1.0e5},
        {"kappa", 0.3}, {"nsub", 1.0e17}, {"xj", 0.5e-6},
        {"ld", 0.1e-6}
    });
}

MosInstanceParams nominalInstance(){
    MosInstanceParams instance;
    instance.w = 4.0e-6;
    instance.l = 1.2e-6;
    return instance;
}

void testCutoffAndDerivatives(){
    const Model model = makeNmos();
    const auto instance = nominalInstance();
    const auto cutoff = evaluateMos3Dc(2.0, 0.0, 0.0, -0.2, model, instance);
    expectNear(cutoff.ids, 0.0, "MOS3 cutoff current is zero without NFS");
    expectNear(cutoff.gm, 0.0, "MOS3 cutoff gm is zero without NFS");

    const auto result = evaluateMos3Dc(2.0, 2.0, 0.0, -0.2, model, instance);
    expect(result.ids > 0.0, "NMOS drain current has the expected polarity");
    expect(result.gm > 0.0, "NMOS gm is positive");
    expect(result.gds > 0.0, "NMOS gds is positive");
    expect(result.gmb > 0.0, "NMOS gmb is positive");

    constexpr double h = 1.0e-5;
    const auto currentAt = [&](double vd, double vg, double vb){
        return evaluateMos3Dc(vd, vg, 0.0, vb, model, instance).ids;
    };
    expectNear(
        result.gm,
        (currentAt(2.0, 2.0 + h, -0.2) - currentAt(2.0, 2.0 - h, -0.2)) /
            (2.0 * h),
        "MOS3 gm agrees with an independent finite difference"
    );
    expectNear(
        result.gds,
        (currentAt(2.0 + h, 2.0, -0.2) - currentAt(2.0 - h, 2.0, -0.2)) /
            (2.0 * h),
        "MOS3 gds agrees with an independent finite difference"
    );
    expectNear(
        result.gmb,
        (currentAt(2.0, 2.0, -0.2 + h) - currentAt(2.0, 2.0, -0.2 - h)) /
            (2.0 * h),
        "MOS3 gmb agrees with an independent finite difference"
    );
}

void testMultiplierAndGeometry(){
    const Model model = makeNmos();
    const auto single = nominalInstance();
    auto parallel = single;
    parallel.m = 2.5;

    const auto one = evaluateMos3Dc(2.0, 2.0, 0.0, -0.2, model, single);
    const auto many = evaluateMos3Dc(2.0, 2.0, 0.0, -0.2, model, parallel);
    expectNear(many.ids, 2.5 * one.ids, "MOS3 multiplier scales drain current");
    expectNear(many.gm, 2.5 * one.gm, "MOS3 multiplier scales gm");

    auto invalid = single;
    invalid.l = 0.1e-6;
    bool rejected = false;
    try {
        static_cast<void>(evaluateMos3Dc(2.0, 2.0, 0.0, 0.0, model, invalid));
    } catch(const std::runtime_error&) {
        rejected = true;
    }
    expect(rejected, "invalid MOS3 effective geometry is rejected");
}

void testMeyerCapacitances(){
    const Model model = makeNmos();
    const auto instance = nominalInstance();
    const auto parameters = mos3_detail::resolve(model, instance);
    const double oxideCapacitance = parameters.cox * parameters.leff *
        parameters.weff * parameters.multiplier;

    const auto cutoff = evaluateMos3MeyerCapacitances(
        1.0, 0.0, 0.0, 0.0, model, instance
    );
    expectNear(cutoff.cgs, 0.0, "Meyer cutoff Cgs is zero");
    expectNear(cutoff.cgd, 0.0, "Meyer cutoff Cgd is zero");
    expectNear(cutoff.cgb, oxideCapacitance, "Meyer cutoff Cgb is Cox");

    const auto saturation = evaluateMos3MeyerCapacitances(
        3.0, 3.0, 0.0, 0.0, model, instance
    );
    expectNear(
        saturation.cgs, 2.0 * oxideCapacitance / 3.0,
        "Meyer saturation Cgs is two thirds Cox"
    );
    expectNear(saturation.cgd, 0.0, "Meyer saturation Cgd is zero");
    expectNear(saturation.cgb, 0.0, "Meyer saturation Cgb is zero");

    const auto forwardLinear = evaluateMos3MeyerCapacitances(
        0.05, 3.0, 0.0, 0.0, model, instance
    );
    const auto reverseLinear = evaluateMos3MeyerCapacitances(
        0.0, 3.0, 0.05, 0.0, model, instance
    );
    expect(
        forwardLinear.cgs > 0.0 && forwardLinear.cgd > 0.0,
        "Meyer linear region distributes channel charge to source and drain"
    );
    expectNear(
        reverseLinear.cgs, forwardLinear.cgd,
        "Meyer reverse mode swaps Cgs with Cgd"
    );
    expectNear(
        reverseLinear.cgd, forwardLinear.cgs,
        "Meyer reverse mode swaps Cgd with Cgs"
    );

    Model pmos("pmos3", ModelType::PMOS, {
        {"level", 3.0}, {"tox", 20.0e-9}, {"vto", -0.7},
        {"kp", 100.0e-6}, {"gamma", 0.5}, {"phi", 0.7},
        {"theta", 0.1}, {"eta", 1.0e-6}, {"vmax", 1.0e5},
        {"kappa", 0.3}, {"nsub", 1.0e17}, {"xj", 0.5e-6},
        {"ld", 0.1e-6}
    });
    const auto pmosLinear = evaluateMos3MeyerCapacitances(
        4.95, 2.0, 5.0, 5.0, pmos, instance
    );
    expectNear(
        pmosLinear.cgs, forwardLinear.cgs,
        "Meyer PMOS uses the polarity-normalized Cgs"
    );
    expectNear(
        pmosLinear.cgd, forwardLinear.cgd,
        "Meyer PMOS uses the polarity-normalized Cgd"
    );
}

} // namespace

int main(){
    testCutoffAndDerivatives();
    testMultiplierAndGeometry();
    testMeyerCapacitances();
    std::cout << "MOS3 DC unit tests: " << (checks - failures) << "/" << checks
              << " checks passed\n";
    return failures == 0 ? 0 : 1;
}
