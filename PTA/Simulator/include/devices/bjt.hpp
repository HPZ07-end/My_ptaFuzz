#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

#include "circuit/circuit.hpp"
#include "devices/device.hpp"
#include "solver/mna.hpp"
#include "solver/voltage_limiting.hpp"
#include "models/model.hpp"

// DC-only Gummel-Poon subset.  Charge storage parameters are deliberately not
// handled here; transient BJT charge modelling needs a separate companion
// model rather than an operating-point stamp extension.
class BJT: public Device{
public:
    BJT(std::string name, std::vector<std::string> nodes, const Model* model,
        double area = 1.0,
        std::optional<std::array<double, 2>> initialCondition = std::nullopt):
        Device(name, nodes, DeviceType::BJT), model_(model), area_(area),
        initialCondition_(initialCondition) {}

    const Model* model() const { return model_; }

    bool isNonlinear() const override { return true; }

    std::vector<InitialVoltageConstraint>
    initialVoltageConstraints() const override{
        if(!initialCondition_){
            return {};
        }
        // SPICE BJT IC is VBE,VCE.  The device terminal order is C,B,E.
        return {
            {1, 2, (*initialCondition_)[0]},
            {0, 2, (*initialCondition_)[1]}
        };
    }

    void allocateUnknown(Circuit& circuit) override{
        if(!model_) return;
        const auto& dc = model_->bjtDc();
        if(dc.rc > 0.0) internalNodes_[0] = circuit.allocateUnknown();
        if(dc.rb > 0.0) internalNodes_[1] = circuit.allocateUnknown();
        if(dc.re > 0.0) internalNodes_[2] = circuit.allocateUnknown();
    }

    void pattern(MNA& mna) override{
        const auto core = coreNodes();
        addFullPattern(mna, core);
        const auto& dc = model_->bjtDc();
        addSeriesPattern(mna, nodeIds[0], core[0], dc.rc);
        addSeriesPattern(mna, nodeIds[1], core[1], dc.rb);
        addSeriesPattern(mna, nodeIds[2], core[2], dc.re);
    }

    void bindMatrix(MNA& mna) override{
        const auto core = coreNodes();
        for(int r = 0; r < 3; ++r){
            if(core[r] >= 0){
                rhs_[r] = &mna.rhs(core[r]);
                sol_[r] = mna.solutionPtr(core[r]);
            }
            for(int c = 0; c < 3; ++c){
                if(core[r] >= 0 && core[c] >= 0){
                    A_[r][c] = mna.ptr(core[r], core[c]);
                }
            }
        }

        const auto& dc = model_->bjtDc();
        bindSeries(mna, 0, nodeIds[0], core[0], dc.rc);
        bindSeries(mna, 1, nodeIds[1], core[1], dc.rb);
        bindSeries(mna, 2, nodeIds[2], core[2], dc.re);
    }

    void stampOperatingPoint() override{
        if(!model_) return;

        const auto& dc = model_->bjtDc();
        const double polarity = model_->type() == ModelType::PNP ? -1.0 : 1.0;
        const double area = area_ > 0.0 ? area_ : 1.0;
        const double vc = voltage(sol_[0]);
        const double vb = voltage(sol_[1]);
        const double ve = voltage(sol_[2]);
        double vbe = polarity * (vb - ve);
        double vbc = polarity * (vb - vc);
        const double nvtBe = dc.nf * dc.vt;
        const double nvtBc = dc.nr * dc.vt;
        const double is = dc.is * area;

        if(hasPreviousVoltages_){
            vbe = limitPnJunctionVoltage(vbe, previousVbe_, nvtBe, is);
            vbc = limitPnJunctionVoltage(vbc, previousVbc_, nvtBc, is);
        }
        previousVbe_ = vbe;
        previousVbc_ = vbc;
        hasPreviousVoltages_ = true;

        const double ebe = std::exp(std::clamp(vbe / nvtBe, -40.0, 40.0));
        const double ebc = std::exp(std::clamp(vbc / nvtBc, -40.0, 40.0));
        const double forward = is * (ebe - 1.0);
        const double reverse = is * (ebc - 1.0);
        const double dForward = is * ebe / nvtBe;
        const double dReverse = is * ebc / nvtBc;

        const double vce = polarity * (vc - ve);
        const double forwardEarly = dc.va > 0.0 ? 1.0 + vce / dc.va : 1.0;
        const double reverseEarly = dc.var > 0.0 ? 1.0 + vce / dc.var : 1.0;
        const double rawTransport = forward * forwardEarly - reverse * reverseEarly;
        const double dRawBe = dForward * forwardEarly;
        const double dRawBc = -dReverse * reverseEarly;
        const double dRawVce =
            (dc.va > 0.0 ? forward / dc.va : 0.0) -
            (dc.var > 0.0 ? reverse / dc.var : 0.0);

        double highInjection = 1.0;
        double dHighBe = 0.0;
        double dHighBc = 0.0;
        if(dc.ikf > 0.0 && forward > 0.0){
            highInjection += forward / dc.ikf;
            dHighBe += dForward / dc.ikf;
        }
        if(dc.ikr > 0.0 && reverse > 0.0){
            highInjection += reverse / dc.ikr;
            dHighBc += dReverse / dc.ikr;
        }

        const double transport = rawTransport / highInjection;
        const double dTransportBe =
            (dRawBe * highInjection - rawTransport * dHighBe) /
            (highInjection * highInjection);
        const double dTransportBc =
            (dRawBc * highInjection - rawTransport * dHighBc) /
            (highInjection * highInjection);
        const double dTransportVce = dRawVce / highInjection;

        const double nvtLeakBe = dc.ne * dc.vt;
        const double nvtLeakBc = dc.nc * dc.vt;
        const double ebeLeak = std::exp(
            std::clamp(vbe / nvtLeakBe, -40.0, 40.0)
        );
        const double ebcLeak = std::exp(
            std::clamp(vbc / nvtLeakBc, -40.0, 40.0)
        );
        const double beCurrent = polarity * (
            forward / dc.bf + dc.ise * area * (ebeLeak - 1.0)
        );
        const double bcCurrent = polarity * (
            reverse / dc.br + dc.isc * area * (ebcLeak - 1.0)
        );
        const double beConductance =
            dForward / dc.bf + dc.ise * area * ebeLeak / nvtLeakBe + dc.gmin;
        const double bcConductance =
            dReverse / dc.br + dc.isc * area * ebcLeak / nvtLeakBc + dc.gmin;

        Vec3 f = {};
        Mat3 j = {};
        stampBranch(f, j, 1, 2, beCurrent + dc.gmin * (vb - ve), beConductance);
        stampBranch(f, j, 1, 0, bcCurrent + dc.gmin * (vb - vc), bcConductance);
        if(dc.rbe > 0.0){
            const double conductance =
                model_->bjtBaseEmitterConductance(area_);
            stampBranch(f, j, 1, 2, conductance * (vb - ve), conductance);
        }
        if(dc.rce > 0.0){
            const double conductance =
                model_->bjtCollectorEmitterConductance(area_);
            stampBranch(f, j, 0, 2, conductance * (vc - ve), conductance);
        }

        const double collectorCurrent = polarity * transport;
        const double dIcDb = dTransportBe + dTransportBc;
        const double dIcDc = -dTransportBc + dTransportVce;
        const double dIcDe = -dIcDb - dIcDc;
        f[0] += collectorCurrent;
        f[2] -= collectorCurrent;
        j[0][0] += dIcDc;
        j[0][1] += dIcDb;
        j[0][2] += dIcDe;
        j[2][0] -= dIcDc;
        j[2][1] -= dIcDb;
        j[2][2] -= dIcDe;

        stampLinearization(f, j);
        stampSeriesResistors();
    }

    void initializePtaState(double initialVbe) override{
        // vbe is stored in intrinsic polarity, so one value works for both
        // NPN and PNP models.  This seeds the first junction-limiting step
        // without imposing mutually inconsistent voltages on shared nodes.
        previousVbe_ = initialVbe;
        previousVbc_ = 0.0;
        hasPreviousVoltages_ = true;
    }

    void saveIterationState() override{
        savedPreviousVbe_ = previousVbe_;
        savedPreviousVbc_ = previousVbc_;
        savedHasPreviousVoltages_ = hasPreviousVoltages_;
    }

    void restoreIterationState() override{
        previousVbe_ = savedPreviousVbe_;
        previousVbc_ = savedPreviousVbc_;
        hasPreviousVoltages_ = savedHasPreviousVoltages_;
    }

    void saveLineSearchState() override{
        lineSearchPreviousVbe_ = previousVbe_;
        lineSearchPreviousVbc_ = previousVbc_;
        lineSearchHasPreviousVoltages_ = hasPreviousVoltages_;
    }

    void restoreLineSearchState() override{
        previousVbe_ = lineSearchPreviousVbe_;
        previousVbc_ = lineSearchPreviousVbc_;
        hasPreviousVoltages_ = lineSearchHasPreviousVoltages_;
    }

private:
    using Vec3 = std::array<double, 3>;
    using Mat3 = std::array<std::array<double, 3>, 3>;

    static double voltage(const double* ptr){ return ptr ? *ptr : 0.0; }

    std::array<int, 3> coreNodes() const{
        return {
            internalNodes_[0] >= 0 ? internalNodes_[0] : nodeIds[0],
            internalNodes_[1] >= 0 ? internalNodes_[1] : nodeIds[1],
            internalNodes_[2] >= 0 ? internalNodes_[2] : nodeIds[2]
        };
    }

    static void addFullPattern(MNA& mna, const std::array<int, 3>& nodes){
        for(int row: nodes){
            if(row < 0) continue;
            for(int col: nodes){
                if(col >= 0) mna.addPattern(row, col);
            }
        }
    }

    static void addSeriesPattern(MNA& mna, int external, int internal,
                                 double resistance){
        if(resistance <= 0.0) return;
        if(external >= 0) mna.addPattern(external, external);
        if(internal >= 0) mna.addPattern(internal, internal);
        if(external >= 0 && internal >= 0){
            mna.addPattern(external, internal);
            mna.addPattern(internal, external);
        }
    }

    void bindSeries(MNA& mna, int terminal, int external, int internal,
                    double resistance){
        if(resistance <= 0.0) return;
        if(external >= 0) seriesA_[terminal][0][0] = mna.ptr(external, external);
        if(internal >= 0) seriesA_[terminal][1][1] = mna.ptr(internal, internal);
        if(external >= 0 && internal >= 0){
            seriesA_[terminal][0][1] = mna.ptr(external, internal);
            seriesA_[terminal][1][0] = mna.ptr(internal, external);
        }
    }

    void stampSeries(int terminal, double resistance){
        if(resistance <= 0.0) return;
        const double g = 1.0 / resistance;
        if(seriesA_[terminal][0][0]) *seriesA_[terminal][0][0] += g;
        if(seriesA_[terminal][0][1]) *seriesA_[terminal][0][1] -= g;
        if(seriesA_[terminal][1][0]) *seriesA_[terminal][1][0] -= g;
        if(seriesA_[terminal][1][1]) *seriesA_[terminal][1][1] += g;
    }

    void stampSeriesResistors(){
        const auto& dc = model_->bjtDc();
        stampSeries(0, dc.rc);
        stampSeries(1, dc.rb);
        stampSeries(2, dc.re);
    }

    static void stampBranch(Vec3& f, Mat3& j, int p, int n, double i,
                            double g){
        f[p] += i;
        f[n] -= i;
        j[p][p] += g;
        j[p][n] -= g;
        j[n][p] -= g;
        j[n][n] += g;
    }

    void stampLinearization(const Vec3& f, const Mat3& j){
        const Vec3 v = {voltage(sol_[0]), voltage(sol_[1]), voltage(sol_[2])};
        for(int r = 0; r < 3; ++r){
            double b = -f[r];
            for(int c = 0; c < 3; ++c){
                if(A_[r][c]) *A_[r][c] += j[r][c];
                b += j[r][c] * v[c];
            }
            if(rhs_[r]) *rhs_[r] += b;
        }
    }

    const Model* model_;
    double area_;
    std::optional<std::array<double, 2>> initialCondition_;
    std::array<int, 3> internalNodes_ = {-1, -1, -1};
    std::array<std::array<double*, 3>, 3> A_ = {};
    std::array<double*, 3> rhs_ = {};
    std::array<const double*, 3> sol_ = {};
    std::array<std::array<std::array<double*, 2>, 2>, 3> seriesA_ = {};

    double previousVbe_ = 0.0;
    double previousVbc_ = 0.0;
    bool hasPreviousVoltages_ = false;
    double savedPreviousVbe_ = 0.0;
    double savedPreviousVbc_ = 0.0;
    bool savedHasPreviousVoltages_ = false;
    double lineSearchPreviousVbe_ = 0.0;
    double lineSearchPreviousVbc_ = 0.0;
    bool lineSearchHasPreviousVoltages_ = false;
};
