#pragma once
#include <cassert>
#include "devices/device.hpp"
#include "solver/mna.hpp"

class Resistor: public Device{
    public:
    Resistor(std::string name, std::vector<std::string> nodes, double R): 
            Device(name, nodes, DeviceType::Resistor), r(R){
                assert(r > 0);
            }
            
    ~Resistor() = default;

    void pattern(MNA& mna) override{
        const int p = nodeIds[0];
        const int n = nodeIds[1];

        if(p >= 0){
            mna.addPattern(p,p);
        }
        if(n >= 0){
            mna.addPattern(n,n);
        }
        if(p >=0 && n >= 0){
            mna.addPattern(p,n);
            mna.addPattern(n,p);
        }
    }

    void bindMatrix(MNA& mna) override {
        const int p = nodeIds[0];
        const int n = nodeIds[1];

        if(p >= 0){
            A11 = mna.ptr(p,p);
        }
        if(n >= 0){
            A22 = mna.ptr(n,n);
        }
        if(p >=0 && n >= 0){
            A12 = mna.ptr(p,n);
            A21 = mna.ptr(n,p);
        }
    }

    void stampOperatingPoint() override {
        const double g = 1.0/r;

        if(A11) *A11 += g;
        if(A12) *A12 -= g;
        if(A21) *A21 -= g;
        if(A22) *A22 += g; 
    }

private:
    double r;

    double* A11 = nullptr;
    double* A12 = nullptr;
    double* A21 = nullptr;
    double* A22 = nullptr;
};
