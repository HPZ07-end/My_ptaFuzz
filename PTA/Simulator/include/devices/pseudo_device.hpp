#pragma once

#include <cassert>

#include "analysis/pta_analysis.hpp"
#include "analysis/transient_analysis.hpp"
#include "devices/device.hpp"
#include "solver/mna.hpp"

enum class PtaPlacementKind{
    CurrentSourceParallelCap,
    TransistorNodeCap,
    VoltageSourceSeriesInductor
};

struct PendingPtaPlacement{
    PtaPlacementKind kind;
    const Device* owner;
    int terminal = -1;
};

class PseudoDevice{
public:
    virtual void pattern(MNA& mna) = 0;

    virtual void bindMatrix(MNA& mna) = 0;

    virtual void stampPseudo(const TransientStampContext& ctx) = 0;

    void setValue(double newValue){
        assert(newValue > 0.0);
        value = newValue;
    }

    virtual ~PseudoDevice() = default;

protected:
    double value;
};

class PseudoCapacitor: public PseudoDevice{
public:
    PseudoCapacitor(int pNode, int nNode, double capacitance,
                    double initialSeriesResistance = 0.0,
                    double timeConstant = 0.0,
                    int branchIdx = -1):
        p(pNode), n(nNode),
        initialSeriesResistance_(initialSeriesResistance),
        timeConstant_(timeConstant), branch_(branchIdx){
        assert(capacitance > 0.0);
        assert(initialSeriesResistance >= 0.0);
        assert(timeConstant >= 0.0);
        value = capacitance;
    }

    void pattern(MNA& mna) override{
        if(branch_ >= 0){
            if(p >= 0){
                mna.addPattern(p, branch_);
                mna.addPattern(branch_, p);
            }
            if(n >= 0){
                mna.addPattern(n, branch_);
                mna.addPattern(branch_, n);
            }
            mna.addPattern(branch_, branch_);
            return;
        }

        if(p >= 0){
            mna.addPattern(p, p);
        }

        if(n >= 0){
            mna.addPattern(n, n);
        }

        if(p >= 0 && n >=0){
            mna.addPattern(p, n);
            mna.addPattern(n, p);
        }
    }

    void bindMatrix(MNA& mna) override{
        if(branch_ >= 0){
            if(p >= 0){
                pBranch_ = mna.ptr(p, branch_);
                branchP_ = mna.ptr(branch_, p);
            }
            if(n >= 0){
                nBranch_ = mna.ptr(n, branch_);
                branchN_ = mna.ptr(branch_, n);
            }
            branchBranch_ = mna.ptr(branch_, branch_);
            branchRhs_ = &mna.rhs(branch_);
            return;
        }

        if(p >= 0){
            pp = mna.ptr(p, p);
            rhsP = &mna.rhs(p);
        }

        if(n >= 0){
            nn = mna.ptr(n, n);
            rhsN = &mna.rhs(n);
        }

        if(p >=0 && n >= 0){
            pn = mna.ptr(p, n);
            np = mna.ptr(n, p);
        }
    }

    void stampPseudo(const TransientStampContext& ctx) override{
        const double seriesResistance = timeVaryingValue(
            initialSeriesResistance_,
            ctx.targetTime
        );

        if(branch_ >= 0){
            const double capacitorResistance =
                1.0 / (ctx.derivative.alpha0 * value);
            double history = ctx.derivative.alpha1 *
                capacitorVoltage(ctx.previousSolution, ctx.previousTime);
            if(ctx.derivative.alpha2 != 0.0 && ctx.olderSolution != nullptr){
                history += ctx.derivative.alpha2 *
                    capacitorVoltage(*ctx.olderSolution, ctx.olderTime);
            }

            if(pBranch_) *pBranch_ += 1.0;
            if(nBranch_) *nBranch_ -= 1.0;
            if(branchP_) *branchP_ += 1.0;
            if(branchN_) *branchN_ -= 1.0;
            *branchBranch_ -= seriesResistance + capacitorResistance;
            *branchRhs_ -= history / ctx.derivative.alpha0;
            return;
        }

        const double capacitorResistance =
            1.0 / (ctx.derivative.alpha0 * value);
        const double g = 1.0 / (capacitorResistance + seriesResistance);
        const double history = value * 
            ctx.historyDerivativeDifference(p, n);
        const double historyScale = g * capacitorResistance;

        if(pp)  *pp += g;
        if(nn)  *nn += g;
        if(pn)  *pn -= g;
        if(np)  *np -= g;

        if(rhsP)    *rhsP -= historyScale * history;
        if(rhsN)    *rhsN += historyScale * history;
    }

private:
    int p;
    int n;

    double* pp = nullptr;
    double* pn = nullptr;
    double* np = nullptr;
    double* nn = nullptr;

    double* rhsP = nullptr;
    double* rhsN = nullptr;

    double initialSeriesResistance_ = 0.0;
    double timeConstant_ = 0.0;
    int branch_ = -1;

    double* pBranch_ = nullptr;
    double* nBranch_ = nullptr;
    double* branchP_ = nullptr;
    double* branchN_ = nullptr;
    double* branchBranch_ = nullptr;
    double* branchRhs_ = nullptr;

    double timeVaryingValue(double initial, double time) const{
        if(initial == 0.0 || timeConstant_ == 0.0){
            return initial;
        }
        const double exponent = std::min(
            time / timeConstant_,
            std::log(std::numeric_limits<double>::max() / initial)
        );
        return initial * std::exp(exponent);
    }

    double capacitorVoltage(const Eigen::VectorXd& solution,
                            double time) const{
        const double pVoltage = p >= 0 ? solution[p] : 0.0;
        const double nVoltage = n >= 0 ? solution[n] : 0.0;
        const double branchCurrent = solution[branch_];
        return pVoltage - nVoltage -
            timeVaryingValue(initialSeriesResistance_, time) * branchCurrent;
    }
};

class PseudoInductor: public PseudoDevice{
public:
    PseudoInductor(int branchIdx, double inductance,
                   double initialParallelConductance = 0.0,
                   double timeConstant = 0.0,
                   int pNode = -1,
                   int nNode = -1,
                   double sourceValue = 0.0,
                   double sourceRampTime = 0.0):
        branch(branchIdx),
        initialParallelConductance_(initialParallelConductance),
        timeConstant_(timeConstant),
        p_(pNode),
        n_(nNode),
        sourceValue_(sourceValue),
        sourceRampTime_(sourceRampTime){
        assert(inductance > 0.0);
        assert(initialParallelConductance >= 0.0);
        assert(timeConstant >= 0.0);
        value = inductance;
    }

    void pattern(MNA& mna) override{
        mna.addPattern(branch, branch);
    }

    void bindMatrix(MNA& mna) override{
        bb = mna.ptr(branch, branch);
        rhsB = &mna.rhs(branch);
    }

    void stampPseudo(const TransientStampContext& ctx) override{
        const double conductance = timeVaryingValue(
            initialParallelConductance_,
            ctx.targetTime
        );
        const double inductorResistance =
            value * ctx.derivative.alpha0;
        const double r = 1.0 /
            (1.0 / inductorResistance + conductance);

        double historyTerm = ctx.historyDerivativeVal(branch);
        if(initialParallelConductance_ > 0.0){
            historyTerm -= ctx.derivative.alpha1 *
                previousConductanceVoltage(
                    ctx.previousSolution,
                    ctx.previousTime
                );
            if(ctx.derivative.alpha2 != 0.0 && ctx.olderSolution != nullptr){
                historyTerm -= ctx.derivative.alpha2 *
                    previousConductanceVoltage(
                        *ctx.olderSolution,
                        ctx.olderTime
                    );
            }
        }
        const double history =
            (r / ctx.derivative.alpha0) * historyTerm;

        *bb -= r;
        *rhsB += history;
    }

private:
    int branch;

    double* bb = nullptr;
    
    double* rhsB = nullptr;

    double initialParallelConductance_ = 0.0;
    double timeConstant_ = 0.0;
    int p_ = -1;
    int n_ = -1;
    double sourceValue_ = 0.0;
    double sourceRampTime_ = 0.0;

    double timeVaryingValue(double initial, double time) const{
        if(initial == 0.0 || timeConstant_ == 0.0){
            return initial;
        }
        const double exponent = std::min(
            time / timeConstant_,
            std::log(std::numeric_limits<double>::max() / initial)
        );
        return initial * std::exp(exponent);
    }

    double previousConductanceVoltage(const Eigen::VectorXd& solution,
                                      double time) const{
        const double pVoltage = p_ >= 0 ? solution[p_] : 0.0;
        const double nVoltage = n_ >= 0 ? solution[n_] : 0.0;
        const double pseudoVoltage = pVoltage - nVoltage -
            ptaSourceRampScale(time, sourceRampTime_) * sourceValue_;
        return timeVaryingValue(initialParallelConductance_, time) *
            pseudoVoltage;
    }
};
