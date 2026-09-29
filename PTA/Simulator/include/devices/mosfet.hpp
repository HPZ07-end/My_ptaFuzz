#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <utility>

#include "analysis/transient_analysis.hpp"
#include "circuit/circuit.hpp"
#include "devices/device.hpp"
#include "devices/mosfet/instance_parameters.hpp"
#include "devices/mosfet/level3/charge_model.hpp"
#include "devices/mosfet/level3/configuration.hpp"
#include "devices/mosfet/level3/dc_model.hpp"
#include "solver/mna.hpp"
#include "solver/voltage_limiting.hpp"
#include "models/model.hpp"

class MOSFET: public Device{
public:
    using MosInstanceParams = ::MosInstanceParams;

    MOSFET(std::string name,
           std::vector<std::string> nodes,
           const Model* model):
            MOSFET(
                std::move(name),
                std::move(nodes),
                model,
                MosInstanceParams()
            ) {}

    MOSFET(std::string name,
           std::vector<std::string> nodes,
           const Model* model,
           MosInstanceParams instance):
            Device(name, nodes, DeviceType::MOSFET),
            model_(model),
            instance_(std::move(instance)) {}

    MOSFET(std::string name,
           std::vector<std::string> nodes,
           const Model* model,
           double w,
           double l):
            Device(name, nodes, DeviceType::MOSFET),
            model_(model) {
        instance_.w = w;
        instance_.l = l;
    }

    const Model* model() const { return model_; }

    const MosInstanceParams& instanceParams() const { return instance_; }

    bool isNonlinear() const override{
        return true;
    }

    bool requiresUicChargeHistoryProtection() const override{
        return model_ && model_->isMos3();
    }

    std::vector<InitialVoltageConstraint>
    initialVoltageConstraints() const override{
        if(!instance_.ic){
            return {};
        }
        // SPICE MOS IC is VDS,VGS,VBS.  The terminal order is D,G,S,B.
        return {
            {0, 2, (*instance_.ic)[0]},
            {1, 2, (*instance_.ic)[1]},
            {3, 2, (*instance_.ic)[2]}
        };
    }

    void allocateUnknown(Circuit& circuit) override{
        if(!model_ || !model_->isMos3()) return;
        if(drainSeriesResistance() > 0.0){
            internalNodes_[0] = circuit.allocateUnknown();
        }
        if(sourceSeriesResistance() > 0.0){
            internalNodes_[1] = circuit.allocateUnknown();
        }
    }

    void pattern(MNA& mna) override{
        if(model_ && model_->isMos3()){
            const auto core = coreNodes();
            addFullPattern(mna, core);
            addSeriesPattern(mna, nodeIds[0], core[0], drainSeriesResistance());
            addSeriesPattern(mna, nodeIds[2], core[2], sourceSeriesResistance());
            return;
        }
        for(int r: {0, 2}){
            for(int c = 0; c < 4; ++c){
                addPattern(mna, r, c);
            }
        }
    }

    void bindMatrix(MNA& mna) override{
        if(model_ && model_->isMos3()){
            const auto core = coreNodes();
            for(int r = 0; r < 4; ++r){
                if(core[r] >= 0){
                    rhs_[r] = &mna.rhs(core[r]);
                    sol_[r] = mna.solutionPtr(core[r]);
                }
                for(int c = 0; c < 4; ++c){
                    if(core[r] >= 0 && core[c] >= 0){
                        A_[r][c] = mna.ptr(core[r], core[c]);
                    }
                }
            }
            bindSeries(mna, 0, nodeIds[0], core[0], drainSeriesResistance());
            bindSeries(mna, 1, nodeIds[2], core[2], sourceSeriesResistance());
            return;
        }
        for(int r: {0, 2}){
            const int row = nodeIds[r];
            if(row >= 0){
                rhs_[r] = &mna.rhs(row);
                sol_[r] = mna.solutionPtr(row);
            }

            for(int c = 0; c < 4; ++c){
                const int col = nodeIds[c];
                if(row >= 0 && col >= 0){
                    A_[r][c] = mna.ptr(row, col);
                }
            }
        }

        for(int c: {1, 3}){
            const int node = nodeIds[c];
            if(node >= 0){
                sol_[c] = mna.solutionPtr(node);
            }
        }
    }

    void stampOperatingPoint() override{
        if(!model_) return;
        if(model_->isMos3()){
            stampMos3OperatingPoint();
            return;
        }

        const auto& dc = model_->mosDc();
        const double polarity = model_->type() == ModelType::PMOS ? -1.0 : 1.0;
        const double vd = voltage(sol_[0]);
        const double vg = voltage(sol_[1]);
        const double vs = voltage(sol_[2]);
        const double width = instance_.w > 0.0 ? instance_.w : 1.0;
        const double length = instance_.l > 0.0 ? instance_.l : 1.0;
        const double beta = dc.kp * width / length;
        const double vto = std::abs(dc.vto);
        double vgs = polarity * (vg - vs);
        double vds = polarity * (vd - vs);
        double vgd = vgs - vds;

        if(hasPreviousVoltages_){
            vgs = limitMosfetVoltage(vgs, previousVgs_, vto);
            vgd = limitMosfetVoltage(vgd, previousVgd_, vto);
            vds = vgs - vgd;
        }

        previousVgs_ = vgs;
        previousVgd_ = vgd;
        hasPreviousVoltages_ = true;

        const double overdrive = vgs - vto;

        double id = 0.0;
        double gm = 0.0;
        double gds = 0.0;

        if(overdrive > 0.0){
            const double vdsEff = std::max(vds, 0.0);
            if(vdsEff < overdrive){
                const double channel = overdrive * vdsEff - 0.5 * vdsEff * vdsEff;
                const double lambdaTerm = 1.0 + dc.lambda * vdsEff;
                id = beta * channel * lambdaTerm;
                gm = beta * vdsEff * lambdaTerm;
                gds = beta * (overdrive - vdsEff) * lambdaTerm
                    + beta * channel * dc.lambda;
            } else {
                const double channel = 0.5 * overdrive * overdrive;
                const double lambdaTerm = 1.0 + dc.lambda * vdsEff;
                id = beta * channel * lambdaTerm;
                gm = beta * overdrive * lambdaTerm;
                gds = beta * channel * dc.lambda;
            }
        }

        const double ids = polarity * id + dc.gmin * (vd - vs);
        const double gdTotal = gds + dc.gmin;
        const double v[4] = {vd, vg, vs, voltage(sol_[3])};
        double f[4] = {ids, 0.0, -ids, 0.0};
        double j[4][4] = {};

        j[0][0] += gdTotal;
        j[0][1] += gm;
        j[0][2] -= gdTotal + gm;
        j[2][0] -= gdTotal;
        j[2][1] -= gm;
        j[2][2] += gdTotal + gm;

        if(dc.rds > 0.0){
            const double conductance =
                model_->mosDrainSourceConductance(instance_.w, instance_.l);
            const double rdsCurrent = conductance * (vd - vs);
            f[0] += rdsCurrent;
            f[2] -= rdsCurrent;
            j[0][0] += conductance;
            j[0][2] -= conductance;
            j[2][0] -= conductance;
            j[2][2] += conductance;
        }

        for(int r: {0, 2}){
            double b = -f[r];
            for(int c: {0, 1, 2}){
                if(A_[r][c]) *A_[r][c] += j[r][c];
                b += j[r][c] * v[c];
            }
            if(rhs_[r]) *rhs_[r] += b;
        }
    }

    void stampTransient(const TransientStampContext& ctx) override{
        if(!model_ || !model_->isMos3()){
            stampOperatingPoint();
            return;
        }

        stampMos3OperatingPoint();
        stampMos3JunctionCharges(ctx);
        stampMos3OverlapCharges(ctx);
        stampMos3MeyerCharges(ctx);
    }

    void stampTransientLteDefect(
        const TransientLteContext& ctx,
        Eigen::VectorXd& residual
    ) const override{
        if(!model_ || !model_->isMos3()) return;

        const auto core = coreNodes();
        // The current Level-3 transient model is a capacitance companion
        // formulation, so use the same endpoint tangent in the LTE defect
        // projection as in its converged Jacobian.
        for(int junction = 0; junction < 2; ++junction){
            const int terminal =
                mos3::ChargeHistory::kJunctionTerminals[junction];
            stampTransientLteCapacitor(
                ctx.derivativeDefect,
                residual,
                core,
                3,
                terminal,
                evaluateMos3JunctionCapacitance(
                    ctx.correctedSolution,
                    terminal
                )
            );
        }

        const mos3::OverlapCapacitances overlap =
            mos3::overlapCapacitances(*model_, instance_);
        stampTransientLteCapacitor(
            ctx.derivativeDefect, residual, core, 1, 2, overlap.cgs
        );
        stampTransientLteCapacitor(
            ctx.derivativeDefect, residual, core, 1, 0, overlap.cgd
        );
        stampTransientLteCapacitor(
            ctx.derivativeDefect, residual, core, 1, 3, overlap.cgb
        );

        const Mos3MeyerCapacitances meyer =
            evaluateMeyerCapacitances(ctx.correctedSolution);
        for(int pair = 0; pair < 3; ++pair){
            stampTransientLteCapacitor(
                ctx.derivativeDefect,
                residual,
                core,
                1,
                mos3::ChargeHistory::kMeyerNegativeTerminals[pair],
                capacitanceAt(meyer, pair)
            );
        }
    }

    void initializePtaMosVgsState(double initialVgs) override{
        // Like the BJT VBE seed, this initializes the nonlinear limiter's
        // internal, polarity-normalized voltage rather than forcing voltages
        // on nodes that may be shared by multiple devices.  With no VGD seed
        // supplied, start the companion limiter at VGD = 0.
        previousVgs_ = initialVgs;
        previousVgd_ = 0.0;
        hasPreviousVoltages_ = true;
    }

    void initializeTransientHistory(const Eigen::VectorXd& solution) override{
        if(!model_ || !model_->isMos3()) return;
        chargeHistory_.initialize(
            meyerCapacitanceArray(solution),
            junctionCapacitanceArray(solution),
            terminalVoltages(solution)
        );
    }

    void acceptTransientSolution(const Eigen::VectorXd& previous,
                                 const Eigen::VectorXd& accepted) override{
        if(!model_ || !model_->isMos3()) return;
        chargeHistory_.accept(
            meyerCapacitanceArray(previous),
            junctionCapacitanceArray(previous),
            terminalVoltages(previous),
            terminalVoltages(accepted)
        );
    }

    void saveIterationState() override{
        savedPreviousVgs_ = previousVgs_;
        savedPreviousVgd_ = previousVgd_;
        savedHasPreviousVoltages_ = hasPreviousVoltages_;
        savedChargeHistory_ = chargeHistory_;
    }

    void restoreIterationState() override{
        previousVgs_ = savedPreviousVgs_;
        previousVgd_ = savedPreviousVgd_;
        hasPreviousVoltages_ = savedHasPreviousVoltages_;
        chargeHistory_ = savedChargeHistory_;
    }

    void saveLineSearchState() override{
        lineSearchPreviousVgs_ = previousVgs_;
        lineSearchPreviousVgd_ = previousVgd_;
        lineSearchHasPreviousVoltages_ = hasPreviousVoltages_;
        lineSearchChargeHistory_ = chargeHistory_;
    }

    void restoreLineSearchState() override{
        previousVgs_ = lineSearchPreviousVgs_;
        previousVgd_ = lineSearchPreviousVgd_;
        hasPreviousVoltages_ = lineSearchHasPreviousVoltages_;
        chargeHistory_ = lineSearchChargeHistory_;
    }

private:
    using Vec4 = std::array<double, 4>;
    using Mat4 = std::array<std::array<double, 4>, 4>;

    static double voltage(const double* ptr){
        return ptr ? *ptr : 0.0;
    }

    std::array<int, 4> coreNodes() const{
        return {
            internalNodes_[0] >= 0 ? internalNodes_[0] : nodeIds[0],
            nodeIds[1],
            internalNodes_[1] >= 0 ? internalNodes_[1] : nodeIds[2],
            nodeIds[3]
        };
    }

    double seriesResistance(bool drain) const{
        return model_ ? mos3::seriesResistance(*model_, instance_, drain) : 0.0;
    }

    double drainSeriesResistance() const { return seriesResistance(true); }
    double sourceSeriesResistance() const { return seriesResistance(false); }

    void addPattern(MNA& mna, int r, int c){
        const int row = nodeIds[r];
        const int col = nodeIds[c];
        if(row >= 0 && col >= 0){
            mna.addPattern(row, col);
        }
    }

    static void addFullPattern(MNA& mna, const std::array<int, 4>& nodes){
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
        stampSeries(0, drainSeriesResistance());
        stampSeries(1, sourceSeriesResistance());
    }

    static void stampBranch(Vec4& f, Mat4& j, int p, int n, double i,
                            double g){
        f[p] += i;
        f[n] -= i;
        j[p][p] += g;
        j[p][n] -= g;
        j[n][p] -= g;
        j[n][n] += g;
    }

    void stampLinearization(const Vec4& f, const Mat4& j){
        const Vec4 v = {
            voltage(sol_[0]), voltage(sol_[1]), voltage(sol_[2]), voltage(sol_[3])
        };
        for(int r = 0; r < 4; ++r){
            double b = -f[r];
            for(int c = 0; c < 4; ++c){
                if(A_[r][c]) *A_[r][c] += j[r][c];
                b += j[r][c] * v[c];
            }
            if(rhs_[r]) *rhs_[r] += b;
        }
    }

    void stampMos3OperatingPoint(){
        const auto& card = model_->mos3();
        const Vec4 v = {
            voltage(sol_[0]), voltage(sol_[1]), voltage(sol_[2]), voltage(sol_[3])
        };
        const double polarity =
            model_->type() == ModelType::PMOS ? -1.0 : 1.0;
        const double vto = std::abs(mos3::valueOr(
            card.vto,
            polarity > 0.0 ? 0.7 : -0.7
        ));
        double vgs = polarity * (v[1] - v[2]);
        double vgd = polarity * (v[1] - v[0]);
        if(hasPreviousVoltages_){
            vgs = limitMosfetVoltage(vgs, previousVgs_, vto);
            vgd = limitMosfetVoltage(vgd, previousVgd_, vto);
        }
        previousVgs_ = vgs;
        previousVgd_ = vgd;
        hasPreviousVoltages_ = true;

        // Evaluate the Level-3 channel at the limited intrinsic terminal
        // voltages, but linearize it against the unmodified MNA iterate.
        // This is the same voltage-limiting contract used by the compact MOS
        // path above: it bounds an individual Newton update without imposing
        // a voltage on any shared circuit node.
        const double limitedVg = v[2] + polarity * vgs;
        const double limitedVd = limitedVg - polarity * vgd;
        const Mos3DcResult result = instance_.off
            ? Mos3DcResult{}
            : evaluateMos3Dc(
                limitedVd,
                limitedVg,
                v[2],
                v[3],
                *model_,
                instance_
            );

        Vec4 f = {result.ids, 0.0, -result.ids, 0.0};
        Mat4 j = {};
        j[0][0] = result.gds;
        j[0][1] = result.gm;
        j[0][3] = result.gmb;
        j[0][2] = -(result.gds + result.gm + result.gmb);
        for(int c = 0; c < 4; ++c){
            j[2][c] = -j[0][c];
        }

        const double thermalVoltage = 0.02585;
        const auto stampJunction = [&](int terminal, double area){
            const double isDensity = mos3::valueOr(card.js);
            const double is = isDensity > 0.0 && area > 0.0
                ? isDensity * area * std::max(instance_.m, 1e-30)
                : mos3::valueOr(card.is) * std::max(instance_.m, 1e-30);
            if(is <= 0.0) return;
            const double junctionVoltage = polarity * (v[3] - v[terminal]);
            const double exponential = std::exp(
                std::clamp(junctionVoltage / thermalVoltage, -40.0, 40.0)
            );
            const double current = polarity * is * (exponential - 1.0);
            const double conductance = is * exponential / thermalVoltage;
            stampBranch(f, j, 3, terminal, current, conductance);
        };
        stampJunction(0, instance_.ad);
        stampJunction(2, instance_.as);

        stampLinearization(f, j);
        stampSeriesResistors();
    }

    void stampMos3JunctionCharges(const TransientStampContext& ctx){
        const auto core = coreNodes();
        for(int junction = 0; junction < 2; ++junction){
            const int terminal = mos3::ChargeHistory::kJunctionTerminals[junction];
            stampChargeCompanion(
                ctx, core, 3, terminal,
                evaluateMos3JunctionCapacitance(ctx.previousSolution, terminal),
                chargeHistory_.junctionCurrent()[junction],
                chargeHistory_.junctionPrevious()[junction]
            );
        }
    }

    void stampMos3OverlapCharges(const TransientStampContext& ctx){
        const mos3::OverlapCapacitances capacitances =
            mos3::overlapCapacitances(*model_, instance_);
        const auto core = coreNodes();
        stampTransientCapacitor(ctx, core, 1, 2, capacitances.cgs);
        stampTransientCapacitor(ctx, core, 1, 0, capacitances.cgd);
        stampTransientCapacitor(ctx, core, 1, 3, capacitances.cgb);
    }

    void stampMos3MeyerCharges(const TransientStampContext& ctx){
        const auto core = coreNodes();
        const Mos3MeyerCapacitances capacitances =
            evaluateMeyerCapacitances(ctx.previousSolution);
        for(int pair = 0; pair < 3; ++pair){
            stampChargeCompanion(
                ctx, core, 1, mos3::ChargeHistory::kMeyerNegativeTerminals[pair],
                capacitanceAt(capacitances, pair), chargeHistory_.meyerCurrent()[pair],
                chargeHistory_.meyerPrevious()[pair]
            );
        }
    }

    double evaluateMos3JunctionCapacitance(const Eigen::VectorXd& solution,
                                           int terminal) const{
        const bool drain = terminal == 0;
        const auto core = coreNodes();
        const double polarity = model_->type() == ModelType::PMOS ? -1.0 : 1.0;
        const double junctionVoltage = polarity * (
            voltageAt(solution, core, 3) - voltageAt(solution, core, terminal)
        );
        return mos3::evaluateJunctionCapacitance(
            junctionVoltage,
            mos3::junctionCapacitanceConfig(*model_, instance_, drain)
        );
    }

    static double voltageAt(const Eigen::VectorXd& solution,
                            const std::array<int, 4>& core,
                            int terminal){
        return core[terminal] >= 0 ? solution[core[terminal]] : 0.0;
    }

    std::array<double, 4> terminalVoltages(
        const Eigen::VectorXd& solution
    ) const{
        const auto core = coreNodes();
        return {
            voltageAt(solution, core, 0),
            voltageAt(solution, core, 1),
            voltageAt(solution, core, 2),
            voltageAt(solution, core, 3)
        };
    }

    Mos3MeyerCapacitances evaluateMeyerCapacitances(
        const Eigen::VectorXd& solution
    ) const{
        const auto core = coreNodes();
        return evaluateMos3MeyerCapacitances(
            voltageAt(solution, core, 0), voltageAt(solution, core, 1),
            voltageAt(solution, core, 2), voltageAt(solution, core, 3),
            *model_, instance_
        );
    }

    static double capacitanceAt(const Mos3MeyerCapacitances& capacitances,
                                int pair){
        switch(pair){
            case 0: return capacitances.cgs;
            case 1: return capacitances.cgd;
            default: return capacitances.cgb;
        }
    }

    std::array<double, 3> meyerCapacitanceArray(
        const Eigen::VectorXd& solution
    ) const{
        const Mos3MeyerCapacitances capacitances =
            evaluateMeyerCapacitances(solution);
        return {
            capacitances.cgs,
            capacitances.cgd,
            capacitances.cgb
        };
    }

    std::array<double, 2> junctionCapacitanceArray(
        const Eigen::VectorXd& solution
    ) const{
        return {
            evaluateMos3JunctionCapacitance(solution, 0),
            evaluateMos3JunctionCapacitance(solution, 2)
        };
    }

    void stampChargeCompanion(const TransientStampContext& ctx,
                              const std::array<int, 4>& core,
                              int positive,
                              int negative,
                              double capacitance,
                              double previousCharge,
                              double olderCharge){
        if(capacitance <= 0.0) return;
        const double conductance = ctx.derivative.alpha0 * capacitance;
        const double previousVoltage =
            voltageAt(ctx.previousSolution, core, positive) -
            voltageAt(ctx.previousSolution, core, negative);
        const double history =
            (ctx.derivative.alpha0 + ctx.derivative.alpha1) * previousCharge +
            ctx.derivative.alpha2 * olderCharge -
            ctx.derivative.alpha0 * capacitance * previousVoltage;

        if(A_[positive][positive]) *A_[positive][positive] += conductance;
        if(A_[positive][negative]) *A_[positive][negative] -= conductance;
        if(A_[negative][positive]) *A_[negative][positive] -= conductance;
        if(A_[negative][negative]) *A_[negative][negative] += conductance;
        if(rhs_[positive]) *rhs_[positive] -= history;
        if(rhs_[negative]) *rhs_[negative] += history;
    }

    void stampTransientCapacitor(const TransientStampContext& ctx,
                                 const std::array<int, 4>& core,
                                 int positive,
                                 int negative,
                                 double capacitance){
        if(capacitance <= 0.0) return;
        const double conductance = ctx.derivative.alpha0 * capacitance;
        const double history = capacitance *
            ctx.historyDerivativeDifference(core[positive], core[negative]);

        if(A_[positive][positive]) *A_[positive][positive] += conductance;
        if(A_[positive][negative]) *A_[positive][negative] -= conductance;
        if(A_[negative][positive]) *A_[negative][positive] -= conductance;
        if(A_[negative][negative]) *A_[negative][negative] += conductance;
        if(rhs_[positive]) *rhs_[positive] -= history;
        if(rhs_[negative]) *rhs_[negative] += history;
    }

    static void stampTransientLteCapacitor(
        const Eigen::VectorXd& derivativeDefect,
        Eigen::VectorXd& residual,
        const std::array<int, 4>& core,
        int positive,
        int negative,
        double capacitance
    ){
        if(capacitance <= 0.0) return;

        const int positiveNode = core[positive];
        const int negativeNode = core[negative];
        const double positiveDerivative = positiveNode >= 0
            ? derivativeDefect[positiveNode]
            : 0.0;
        const double negativeDerivative = negativeNode >= 0
            ? derivativeDefect[negativeNode]
            : 0.0;
        const double current = capacitance *
            (positiveDerivative - negativeDerivative);

        if(positiveNode >= 0) residual[positiveNode] += current;
        if(negativeNode >= 0) residual[negativeNode] -= current;
    }

    const Model* model_;
    MosInstanceParams instance_;

    std::array<int, 2> internalNodes_ = {-1, -1};
    std::array<std::array<double*, 4>, 4> A_ = {};
    std::array<double*, 4> rhs_ = {};
    std::array<const double*, 4> sol_ = {};
    std::array<std::array<std::array<double*, 2>, 2>, 2> seriesA_ = {};

    double previousVgs_ = 0.0;
    double previousVgd_ = 0.0;
    bool hasPreviousVoltages_ = false;
    double savedPreviousVgs_ = 0.0;
    double savedPreviousVgd_ = 0.0;
    bool savedHasPreviousVoltages_ = false;
    mos3::ChargeHistory chargeHistory_;
    mos3::ChargeHistory savedChargeHistory_;
    double lineSearchPreviousVgs_ = 0.0;
    double lineSearchPreviousVgd_ = 0.0;
    bool lineSearchHasPreviousVoltages_ = false;
    mos3::ChargeHistory lineSearchChargeHistory_;
};
