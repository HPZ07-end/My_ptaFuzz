#pragma once

#include <iosfwd>
#include <string>

class Circuit;
struct AnalysisPlan;

class SpiceOutputWriter {
public:
    static void writeOperatingPoint(std::ostream& os,
                                    const Circuit& circuit,
                                    const std::string& title,
                                    const AnalysisPlan& plan);

    static void writeTransient(std::ostream& os,
                               const Circuit& circuit,
                               const std::string& title,
                               const AnalysisPlan& plan);

    static void writePtaDiagnostics(std::ostream& os,
                                    const Circuit& circuit);
};

class SpiceRawWriter {
public:
    static void writeOperatingPoint(std::ostream& os,
                                    const Circuit& circuit,
                                    const std::string& title);

    static void writeTransient(std::ostream& os,
                               const Circuit& circuit,
                               const std::string& title);
};
