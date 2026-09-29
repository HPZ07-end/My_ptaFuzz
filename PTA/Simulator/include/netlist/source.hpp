#pragma once

#include <cstddef>
#include <string>
#include <vector>

// A physical netlist statement after comments and continuation lines are
// been normalized.  Higher-level parsing can therefore work solely on SPICE
// statements and preserve their original location for diagnostics.
struct LogicalNetlistLine {
    std::size_t lineNumber = 0;
    // Raw text is retained only for directives whose grammar needs it beyond
    // tokenization (`.title` and `.print`); ordinary element lines keep just
    // tokens to reduce memory pressure on large netlists.
    std::string text;
    std::vector<std::string> tokens;
};

struct NetlistSource {
    std::string title;
    std::vector<LogicalNetlistLine> lines;
};
