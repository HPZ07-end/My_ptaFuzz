#pragma once

#include <optional>
#include <string>
#include <vector>

#include "analysis/pta_analysis.hpp"
#include "analysis/transient_analysis.hpp"

enum class PrintQuantity {
    Voltage,
    BranchCurrent
};

// One SPICE .print expression. For a voltage, name is the positive node and
// reference is the optional negative node. For a current, name is the device.
struct PrintVariable {
    PrintQuantity quantity = PrintQuantity::Voltage;
    std::string name;
    std::string reference = "0";
};

// A node-voltage constraint parsed from .nodeset or .ic.  The voltage is
// positiveNode minus negativeNode, where a missing reference is ground.
struct NodeVoltageConstraint {
    std::string positiveNode;
    std::string negativeNode = "0";
    double voltage = 0.0;
};

// Parameters accepted by the .pstran pseudo-transient control card.  The
// solver has direct equivalents for convval, the step limits, CEPTA tau,
// source ramping, and the BJT/MOS nonlinear-voltage limiter seeds.
struct PstranAnalysisConfig {
    double convergenceValue = 1.0;
    bool convergenceValueSpecified = false;
    double initialStep = 1.0e-9;
    bool initialStepSpecified = false;
    double minimumStep = 1.0e-15;
    bool minimumStepSpecified = false;
    double maximumStep = 1.0e3;
    bool maximumStepSpecified = false;
    double tau = 0.0;
    bool tauSpecified = false;
    double vbe0 = 0.0;
    bool vbe0Specified = false;
    double kvgs0 = 0.0;
    bool kvgs0Specified = false;
    double tauRamp = 0.0;
    bool tauRampSpecified = false;

    PtaAnalysisConfig makePtaConfig() const {
        PtaAnalysisConfig config;
        config.mode = PtaMode::Force;
        config.initialStep = initialStep;
        config.minimumStep = minimumStep;
        config.maximumStep = maximumStep;
        config.derivativeTolerance = convergenceValue;
        config.dcResidualTolerance = convergenceValue;
        config.compoundTimeConstant = tau;
        config.sourceRampTime = tauRamp;
        config.initialBjtVbe = vbe0;
        if(kvgs0Specified){
            config.initialMosVgs = kvgs0;
        }
        return config;
    }
};

struct TransientNetlistParameterPresence {
    bool outputStartTime = false;
    bool maximumStep = false;
    bool useInitialConditions = false;
};

// Analysis requests found in one netlist. More analysis types can be added here
// without changing Parser's public result interface.
struct AnalysisPlan {
    bool operatingPointRequested = false;
    bool operatingPointPrintRequested = false;
    bool transientPrintRequested = false;
    std::optional<TransientAnalysisConfig> transient;
    std::optional<TransientNetlistParameterPresence>
        transientNetlistParameters;
    // HSPICE-compatible .option(s) DELMAX.  Once a .tran card is present it
    // is folded into TransientAnalysisConfig::maximumStep as a hard cap.
    std::optional<double> delmax;
    std::optional<PstranAnalysisConfig> pseudoTransient;
    // .nodeset is an OP/Newton initial guess only.  .ic supplies the UIC
    // state and is also a stronger initial guess when UIC is not requested.
    std::vector<NodeVoltageConstraint> nodeSets;
    std::vector<NodeVoltageConstraint> initialConditions;
    std::vector<PrintVariable> operatingPointPrints;
    std::vector<PrintVariable> transientPrints;
};
