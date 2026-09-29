#pragma once

#include <string>

#include "netlist/source.hpp"

// Owns lexical input concerns: title handling, comments, continuation lines,
// and `.end` placement.  Semantic directives are handled by Parser.
class NetlistReader {
public:
    explicit NetlistReader(std::string filename);

    bool read(NetlistSource& source) const;

private:
    std::string filename_;
};
