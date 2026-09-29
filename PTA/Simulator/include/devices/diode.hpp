#pragma once

#include <algorithm>
#include <cmath>

#include "circuit/circuit.hpp"
#include "devices/device.hpp"
#include "solver/mna.hpp"
#include "solver/voltage_limiting.hpp"
#include "models/model.hpp"

class Diode: public Device{
public:
    Diode(std::string name, std::vector<std::string> nodes, const Model* model, double area = 1.0):
            Device(name, nodes, DeviceType::Diode), model_(model), area_(area) {}

    const Model* model() const { return model_; }

    bool isNonlinear() const override{
        return true;
    }

    void allocateUnknown(Circuit& circuit) override{
        if(model_ && model_->diodeDc().rs > 0.0){
            internalAnode_ = circuit.allocateUnknown();
        }
    }

    void pattern(MNA& mna) override{
        const int p = intrinsicAnode();
        const int n = nodeIds[1];

        if(p >= 0) mna.addPattern(p, p);
        if(n >= 0) mna.addPattern(n, n);
        if(p >= 0 && n >= 0){
            mna.addPattern(p, n);
            mna.addPattern(n, p);
        }

        if(internalAnode_ >= 0){
            const int externalAnode = nodeIds[0];
            if(externalAnode >= 0){
                mna.addPattern(externalAnode, externalAnode);
            }
            mna.addPattern(internalAnode_, internalAnode_);
            if(externalAnode >= 0){
                mna.addPattern(externalAnode, internalAnode_);
                mna.addPattern(internalAnode_, externalAnode);
            }
        }
    }

    void bindMatrix(MNA& mna) override{
        const int p = intrinsicAnode();
        const int n = nodeIds[1];

        if(p >= 0) A11 = mna.ptr(p, p);
        if(n >= 0) A22 = mna.ptr(n, n);
        if(p >= 0 && n >= 0){
            A12 = mna.ptr(p, n);
            A21 = mna.ptr(n, p);
        }

        if(p >= 0){
            rhsP_ = &mna.rhs(p);
            solP_ = mna.solutionPtr(p);
        }
        if(n >= 0){
            rhsN_ = &mna.rhs(n);
            solN_ = mna.solutionPtr(n);
        }

        if(internalAnode_ >= 0){
            const int externalAnode = nodeIds[0];
            if(externalAnode >= 0){
                seriesA11_ = mna.ptr(externalAnode, externalAnode);
                seriesA12_ = mna.ptr(externalAnode, internalAnode_);
                seriesA21_ = mna.ptr(internalAnode_, externalAnode);
            }
            seriesA22_ = mna.ptr(internalAnode_, internalAnode_);
        }
    }

    void stampOperatingPoint() override{
        if(!model_) return;

        const auto& dc = model_->diodeDc();
        const double area = area_ > 0.0 ? area_ : 1.0;

        double vd = voltage(solP_) - voltage(solN_);
        const double nvt = dc.n * dc.vt;

        const double is = dc.is * area;

        if(hasPreviousVd_){
            vd = limitPnJunctionVoltage(vd, previousVd_, nvt, is);
        }
        
        previousVd_ = vd;
        hasPreviousVd_ = true;

        const double arg = std::clamp(vd / nvt, -40.0, 40.0);
        const double evd = std::exp(arg);
        const double id = is * (evd - 1.0) + dc.gmin * vd;
        const double gd = is * evd / nvt + dc.gmin;
        const double bp = gd * vd - id;

        if(A11) *A11 += gd;
        if(A12) *A12 -= gd;
        if(A21) *A21 -= gd;
        if(A22) *A22 += gd;
        if(rhsP_) *rhsP_ += bp;
        if(rhsN_) *rhsN_ -= bp;

        stampSeriesResistance();
    }

    void saveIterationState() override{
        savedPreviousVd_ = previousVd_;
        savedHasPreviousVd_ = hasPreviousVd_;
    }

    void restoreIterationState() override{
        previousVd_ = savedPreviousVd_;
        hasPreviousVd_ = savedHasPreviousVd_;
    }

    void saveLineSearchState() override{
        lineSearchPreviousVd_ = previousVd_;
        lineSearchHasPreviousVd_ = hasPreviousVd_;
    }

    void restoreLineSearchState() override{
        previousVd_ = lineSearchPreviousVd_;
        hasPreviousVd_ = lineSearchHasPreviousVd_;
    }

private:
    static double voltage(const double* ptr){
        return ptr ? *ptr : 0.0;
    }

    int intrinsicAnode() const{
        return internalAnode_ >= 0 ? internalAnode_ : nodeIds[0];
    }

    void stampSeriesResistance(){
        if(internalAnode_ < 0 || !model_){
            return;
        }

        const double conductance = model_->diodeConductance(area_);
        if(seriesA11_) *seriesA11_ += conductance;
        if(seriesA12_) *seriesA12_ -= conductance;
        if(seriesA21_) *seriesA21_ -= conductance;
        if(seriesA22_) *seriesA22_ += conductance;
    }

    const Model* model_;
    double area_;
    int internalAnode_ = -1;

    double* A11 = nullptr;
    double* A12 = nullptr;
    double* A21 = nullptr;
    double* A22 = nullptr;
    double* seriesA11_ = nullptr;
    double* seriesA12_ = nullptr;
    double* seriesA21_ = nullptr;
    double* seriesA22_ = nullptr;
    double* rhsP_ = nullptr;
    double* rhsN_ = nullptr;
    const double* solP_ = nullptr;
    const double* solN_ = nullptr;

    double previousVd_ = 0.0;
    bool hasPreviousVd_ = false;
    double savedPreviousVd_ = 0.0;
    bool savedHasPreviousVd_ = false;
    double lineSearchPreviousVd_ = 0.0;
    bool lineSearchHasPreviousVd_ = false;
};
