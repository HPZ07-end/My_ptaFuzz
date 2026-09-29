#pragma once

#include "devices/device.hpp"
#include "solver/mna.hpp"

class CurrentSource: public Device{
public: 
    CurrentSource(std::string name, std::vector<std::string> nodes, double I):
                Device(name, nodes, DeviceType::CurrentSource), i(I) {}

    ~CurrentSource() = default;

    void pattern(MNA&) override{}

    void bindMatrix(MNA& mna) override {
        const int p = nodeIds[0];
        const int n = nodeIds[1];

        if(p >= 0){
            pos = &mna.rhs(p);
        }
        if(n >= 0){
            neg = &mna.rhs(n);
        }
    }

    void stampOperatingPoint() override {
        if(pos) *pos -= operatingPointSourceScale_ * i;
        if(neg) *neg += operatingPointSourceScale_ * i;
    }

    void setOperatingPointSourceScale(double scale) override{
        operatingPointSourceScale_ = scale;
    }

private:
    double i;
    double operatingPointSourceScale_ = 1.0;

    double* pos = nullptr;
    double* neg = nullptr;
};
