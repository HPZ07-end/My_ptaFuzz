#pragma once

#include <array>
#include <optional>

// Parameters supplied on an individual MOSFET netlist element.
struct MosInstanceParams {
    double w = 1.0;
    double l = 1.0;
    double ad = 0.0;
    double as = 0.0;
    double pd = 0.0;
    double ps = 0.0;
    double nrd = 0.0;
    double nrs = 0.0;
    double m = 1.0;
    bool off = false;
    std::optional<std::array<double, 3>> ic;
    std::optional<double> temp;
};
