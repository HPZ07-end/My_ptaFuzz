#include "analysis/analysis_plan.hpp"
#include "analysis/transient_analysis.hpp"
#include "analysis/pta_analysis.hpp"
#include "analysis/solver_options.hpp"
#include "circuit/circuit.hpp"
#include "devices/current_source.hpp"
#include "devices/device.hpp"
#include "devices/pseudo_device.hpp"
#include "devices/resistor.hpp"
#include "devices/voltage_source.hpp"
#include "solver/mna.hpp"
#include "solver/newton_convergence.hpp"
#include "solver/newton_step.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

// This deliberately linear stamp is marked nonlinear so the PTA solve takes
// the Newton path.  A small pseudo-capacitance makes the first Newton target
// exceed the iteration budget; one global capacitance increase restores it.
class PtaRecoveryDevice: public Device {
public:
    PtaRecoveryDevice(
        std::string name,
        std::vector<std::string> nodes,
        double conductance,
        double current
    ):
        Device(std::move(name), std::move(nodes), DeviceType::BJT),
        conductance_(conductance),
        current_(current) {}

    bool isNonlinear() const override{
        return true;
    }

    void pattern(MNA& mna) override{
        if(nodeIds[0] >= 0){
            mna.addPattern(nodeIds[0], nodeIds[0]);
        }
    }

    void bindMatrix(MNA& mna) override{
        if(nodeIds[0] >= 0){
            diagonal_ = mna.ptr(nodeIds[0], nodeIds[0]);
            rhs_ = &mna.rhs(nodeIds[0]);
        }
    }

    void stampOperatingPoint() override{
        *diagonal_ += conductance_;
        *rhs_ += current_;
    }

private:
    double conductance_;
    double current_;
    double* diagonal_ = nullptr;
    double* rhs_ = nullptr;
};

// A regression fixture for Newton convergence: its first linearized update is
// intentionally tiny, but restamping at that candidate reveals a large
// nonlinear defect.  A raw update-only criterion would stop after one pass.
class NewtonResidualTrapDevice: public Device {
public:
    NewtonResidualTrapDevice(
        std::string name,
        std::vector<std::string> nodes,
        double initialRightHandSide = 5.0e-10,
        double laterRightHandSide = 1.0
    ):
        Device(std::move(name), std::move(nodes), DeviceType::BJT),
        initialRightHandSide_(initialRightHandSide),
        laterRightHandSide_(laterRightHandSide) {}

    bool isNonlinear() const override{
        return true;
    }

    void pattern(MNA& mna) override{
        mna.addPattern(nodeIds[0], nodeIds[0]);
    }

    void bindMatrix(MNA& mna) override{
        mna_ = &mna;
        diagonal_ = mna.ptr(nodeIds[0], nodeIds[0]);
        rhs_ = &mna.rhs(nodeIds[0]);
    }

    void stampOperatingPoint() override{
        *diagonal_ += 1.0;
        *rhs_ += std::abs(mna_->solution()[nodeIds[0]]) < 1.0e-15
            ? initialRightHandSide_
            : laterRightHandSide_;
    }

private:
    MNA* mna_ = nullptr;
    double initialRightHandSide_;
    double laterRightHandSide_;
    double* diagonal_ = nullptr;
    double* rhs_ = nullptr;
};

class NewtonBacktrackingDevice: public Device {
public:
    NewtonBacktrackingDevice(std::string name, std::vector<std::string> nodes):
        Device(std::move(name), std::move(nodes), DeviceType::BJT) {}

    bool isNonlinear() const override{
        return true;
    }

    void pattern(MNA& mna) override{
        mna.addPattern(nodeIds[0], nodeIds[0]);
    }

    void bindMatrix(MNA& mna) override{
        mna_ = &mna;
        diagonal_ = mna.ptr(nodeIds[0], nodeIds[0]);
        rhs_ = &mna.rhs(nodeIds[0]);
    }

    void stampOperatingPoint() override{
        const double x = mna_->solution()[nodeIds[0]];
        const double residual = x * x + x - 2.0;
        const double jacobian = 2.0 * x + 1.0;
        *diagonal_ += jacobian;
        *rhs_ += jacobian * x - residual;
    }

private:
    MNA* mna_ = nullptr;
    double* diagonal_ = nullptr;
    double* rhs_ = nullptr;
};

// Its physical DC equation is deliberately empty, so the ordinary MNA is
// singular.  A PTA node capacitor regularizes the pseudo-time system.  This
// isolates the fallback activation transition from nonlinear convergence.
class PtaRegularizedFloatingDevice: public Device {
public:
    PtaRegularizedFloatingDevice(
        std::string name,
        std::vector<std::string> nodes
    ):
        Device(std::move(name), std::move(nodes), DeviceType::BJT) {}

    void pattern(MNA& mna) override{
        mna.addPattern(nodeIds[0], nodeIds[0]);
    }

    void bindMatrix(MNA&) override{}

    void stampOperatingPoint() override{}
};

class CircuitPtaTestAccess {
public:
    static void addNodeCapacitor(
        Circuit& circuit,
        int node,
        double capacitance,
        double previousDelta,
        bool hasPreviousDelta
    ){
        auto capacitor = std::make_unique<PseudoCapacitor>(
            node,
            -1,
            capacitance
        );
        PseudoCapacitor* rawCapacitor = capacitor.get();

        circuit.pseudoDevices_.push_back(std::move(capacitor));
        circuit.ptaNodeCaps_.push_back({
            node,
            rawCapacitor,
            capacitance,
            previousDelta,
            hasPreviousDelta
        });
    }

    static std::optional<PtaNodeCapacitanceGrowth> growForResidual(
        Circuit& circuit,
        const Eigen::VectorXd& residual,
        const PtaAnalysisConfig& config
    ){
        return circuit.growPtaNodeCapacitanceForResidual(residual, config);
    }

    static void updateAfterAcceptedStep(
        Circuit& circuit,
        const Eigen::VectorXd& currentSolution,
        const Eigen::VectorXd& previousSolution,
        const PtaAnalysisConfig& config
    ){
        circuit.updatePtaNodeCapacitancesAfterAcceptedStep(
            currentSolution,
            previousSolution,
            config
        );
    }

    static double capacitance(const Circuit& circuit, std::size_t index){
        return circuit.ptaNodeCaps_.at(index).capacitance;
    }

    static double previousDelta(const Circuit& circuit, std::size_t index){
        return circuit.ptaNodeCaps_.at(index).previousDelta;
    }

    static bool hasPreviousDelta(const Circuit& circuit, std::size_t index){
        return circuit.ptaNodeCaps_.at(index).hasPreviousDelta;
    }

    static int ptaCapacitanceGrowths(const Circuit& circuit){
        return circuit.operatingPointStats_.ptaCapacitanceGrowths;
    }

    static int ptaCapacitanceReductions(const Circuit& circuit){
        return circuit.operatingPointStats_.ptaCapacitanceReductions;
    }

    static int ptaMinimumStepRecoveries(const Circuit& circuit){
        return circuit.operatingPointStats_.ptaMinimumStepRecoveries;
    }

    static const Eigen::VectorXd& solution(const Circuit& circuit){
        return circuit.mna_->solution();
    }
};

namespace {

int checkCount = 0;
int failureCount = 0;

Eigen::VectorXd makeVector(std::initializer_list<double> values){
    Eigen::VectorXd result(static_cast<Eigen::Index>(values.size()));
    Eigen::Index index = 0;
    for(double value : values){
        result[index++] = value;
    }
    return result;
}

void expect(bool condition, const std::string& description){
    ++checkCount;
    if(condition){
        return;
    }

    ++failureCount;
    std::cerr << "FAIL: " << description << '\n';
}

template<class Callback>
void expectInvalidArgument(Callback&& callback,
                           const std::string& description){
    bool threwInvalidArgument = false;

    try {
        callback();
    } catch(const std::invalid_argument&) {
        threwInvalidArgument = true;
    } catch(...) {
    }

    expect(threwInvalidArgument, description);
}

void expectNear(
    double actual,
    double expected,
    const std::string& description
){
    const double tolerance = 32.0 * std::numeric_limits<double>::epsilon()
        * std::max({1.0, std::abs(actual), std::abs(expected)});
    expect(
        std::isfinite(actual) && std::abs(actual - expected) <= tolerance,
        description
    );
}

void expectDefaultInvalid(
    const TransientErrorEstimate& estimate,
    const std::string& description
){
    expect(!estimate.valid, description + ": invalid");
}

TransientIntegrator makeIntegratorWithHistory(
    const Eigen::VectorXd& initialSolution,
    const Eigen::VectorXd& acceptedSolution,
    double acceptedTime = 1.0
){
    TransientIntegrator integrator;
    integrator.initialize(0.0, initialSolution);
    integrator.accept(acceptedTime, acceptedSolution);
    return integrator;
}

TransientIntegrator makeIntegratorWithStrictHistory(
    const Eigen::VectorXd& solutionNm2,
    const Eigen::VectorXd& solutionNm1,
    const Eigen::VectorXd& solutionN,
    double timeNm1 = 1.0,
    double timeN = 2.0
){
    TransientIntegrator integrator;
    integrator.initialize(0.0, solutionNm2);
    integrator.accept(timeNm1, solutionNm1);
    integrator.accept(timeN, solutionN);
    return integrator;
}

TransientSolverOptions makeAbsoluteOnlyOptions(){
    TransientSolverOptions options;
    options.relativeTolerance = 0.0;
    options.voltageAbsoluteTolerance = 1.0;
    options.currentAbsoluteTolerance = 1.0;
    return options;
}

void testSolverOptionsValidation(){
    constexpr double maximumIntegrationStep = 1.0e-6;

    NewtonSolverOptions newton;
    expect(newton.valid(), "default Newton options are valid");
    newton.maximumIterations = 0;
    expect(!newton.valid(), "zero Newton iteration limit is invalid");
    newton = NewtonSolverOptions{};
    newton.tolerance = 0.0;
    expect(!newton.valid(), "zero Newton tolerance is invalid");
    newton = NewtonSolverOptions{};
    newton.currentAbsoluteTolerance = 0.0;
    expect(!newton.valid(), "zero Newton current tolerance is invalid");
    newton = NewtonSolverOptions{};
    newton.normalizedResidualTolerance = 0.0;
    expect(!newton.valid(), "zero normalized residual tolerance is invalid");
    newton = NewtonSolverOptions{};
    newton.maximumSolutionStep =
        std::numeric_limits<double>::quiet_NaN();
    expect(!newton.valid(), "non-finite Newton step bound is invalid");
    newton = NewtonSolverOptions{};
    newton.maximumConsecutiveNonMonotoneSteps = -1;
    expect(!newton.valid(), "negative non-monotone Newton step limit is invalid");
    newton = NewtonSolverOptions{};
    newton.maximumNonMonotoneResidualGrowth = 0.99;
    expect(!newton.valid(), "Newton residual growth bound below one is invalid");
    newton = NewtonSolverOptions{};
    newton.trustRegionMinimumRadius = 0.0;
    expect(!newton.valid(), "zero trust-region minimum radius is invalid");
    newton = NewtonSolverOptions{};
    newton.trustRegionAcceptanceRatio = 1.0;
    expect(!newton.valid(), "unit trust-region acceptance ratio is invalid");
    newton = NewtonSolverOptions{};
    newton.trustRegionMaximumRadius =
        newton.trustRegionMinimumRadius * 0.5;
    expect(!newton.valid(), "trust-region maximum radius below minimum is invalid");

    SourceSteppingOptions sourceStepping;
    expect(sourceStepping.valid(), "default source-stepping options are valid");
    sourceStepping.minimumStep = sourceStepping.initialStep * 2.0;
    expect(
        !sourceStepping.valid(),
        "source-stepping minimum step above initial step is invalid"
    );
    sourceStepping = SourceSteppingOptions{};
    sourceStepping.failureScale = 1.0;
    expect(
        !sourceStepping.valid(),
        "source-stepping failure scale of one is invalid"
    );

    OperatingPointSolverOptions operatingPoint;
    expect(
        operatingPoint.valid(),
        "default operating-point solver options are valid"
    );
    operatingPoint.newton.maximumIterations = -1;
    expect(
        !operatingPoint.valid(),
        "operating-point solver validates nested Newton options"
    );

    TransientSolverOptions options;

    expect(
        options.validFor(maximumIntegrationStep),
        "default solver options are valid"
    );

    options.relativeTolerance = 0.0;
    options.safetyFactor = 1.0;
    options.maximumScale = 1.0;
    options.maximumRejects = 0;
    expect(
        options.validFor(maximumIntegrationStep),
        "inclusive solver-option boundaries are valid"
    );

    options = TransientSolverOptions{};
    expect(!options.validFor(0.0), "zero maximum integration step is invalid");
    expect(
        !options.validFor(std::numeric_limits<double>::infinity()),
        "infinite maximum integration step is invalid"
    );

    options = TransientSolverOptions{};
    options.relativeTolerance = -1.0;
    expect(!options.validFor(maximumIntegrationStep), "negative relative tolerance is invalid");

    options = TransientSolverOptions{};
    options.voltageAbsoluteTolerance = 0.0;
    expect(!options.validFor(maximumIntegrationStep), "zero voltage tolerance is invalid");

    options = TransientSolverOptions{};
    options.currentAbsoluteTolerance = std::numeric_limits<double>::quiet_NaN();
    expect(!options.validFor(maximumIntegrationStep), "NaN current tolerance is invalid");

    options = TransientSolverOptions{};
    options.minimumStep = 0.0;
    expect(!options.validFor(maximumIntegrationStep), "zero minimum step is invalid");

    options = TransientSolverOptions{};
    options.minimumStep = 2.0 * maximumIntegrationStep;
    expect(
        !options.validFor(maximumIntegrationStep),
        "minimum step above maximum integration step is invalid"
    );

    options = TransientSolverOptions{};
    options.safetyFactor = 1.1;
    expect(!options.validFor(maximumIntegrationStep), "safety factor above one is invalid");

    options = TransientSolverOptions{};
    options.minimumScale = 1.0;
    expect(!options.validFor(maximumIntegrationStep), "minimum scale of one is invalid");

    options = TransientSolverOptions{};
    options.maximumScale = 0.9;
    expect(!options.validFor(maximumIntegrationStep), "maximum scale below one is invalid");

    options = TransientSolverOptions{};
    options.convergenceFailureScale = 1.0;
    expect(
        !options.validFor(maximumIntegrationStep),
        "convergence failure scale of one is invalid"
    );

    options = TransientSolverOptions{};
    options.maximumRejects = -1;
    expect(!options.validFor(maximumIntegrationStep), "negative reject limit is invalid");

    options = TransientSolverOptions{};
    options.newtonOptions.maximumIterations = 0;
    expect(
        !options.validFor(maximumIntegrationStep),
        "transient solver validates nested Newton options"
    );

    TransientAnalysisConfig transient;
    transient.outputInterval = 1.0e-9;
    transient.stopTime = 10.0e-9;
    expect(transient.valid(), "valid transient analysis configuration is accepted");
    transient.outputStartTime = transient.stopTime;
    expect(
        !transient.valid(),
        "transient output start must precede stop time"
    );
}

void testPtaConfigValidation(){
    PtaAnalysisConfig config;

    try {
        config.validate();
        expect(true, "default PTA configuration is valid");
    } catch(...) {
        expect(false, "default PTA configuration is valid");
    }

    config = PtaAnalysisConfig{};
    config.voltageSourceInductance = 0.0;
    expectInvalidArgument(
        [&config] { config.validate(); },
        "zero pseudo-inductance is invalid"
    );

    config = PtaAnalysisConfig{};
    config.minimumNodeCapacitance =
        2.0 * config.initialNodeCapacitance;
    expectInvalidArgument(
        [&config] { config.validate(); },
        "node-capacitance bounds must contain the initial value"
    );

    config = PtaAnalysisConfig{};
    config.mediumOscillationScale = config.smallOscillationScale;
    expectInvalidArgument(
        [&config] { config.validate(); },
        "oscillation scales must be strictly ordered"
    );

    config = PtaAnalysisConfig{};
    config.mediumOscillationRatio = config.heavyOscillationRatio;
    expectInvalidArgument(
        [&config] { config.validate(); },
        "oscillation ratios must be strictly ordered"
    );

    config = PtaAnalysisConfig{};
    config.successfulStepScale = 1.0;
    expectInvalidArgument(
        [&config] { config.validate(); },
        "successful-step scale must be greater than one"
    );

    config = PtaAnalysisConfig{};
    config.derivativeRelativeTolerance = -1.0;
    expectInvalidArgument(
        [&config] { config.validate(); },
        "negative derivative relative tolerance is invalid"
    );

    config = PtaAnalysisConfig{};
    config.dcResidualRelativeTolerance = -1.0;
    expectInvalidArgument(
        [&config] { config.validate(); },
        "negative DC residual relative tolerance is invalid"
    );

    config = PtaAnalysisConfig{};
    config.compoundTimeConstant = -1.0;
    expectInvalidArgument(
        [&config] { config.validate(); },
        "negative CEPTA time constant is invalid"
    );

    config = PtaAnalysisConfig{};
    config.sourceRampTime = -1.0;
    expectInvalidArgument(
        [&config] { config.validate(); },
        "negative PTA source-ramp time is invalid"
    );

    config = PtaAnalysisConfig{};
    config.initialMosVgs = std::numeric_limits<double>::quiet_NaN();
    expectInvalidArgument(
        [&config] { config.validate(); },
        "non-finite PTA MOS initial voltage is invalid"
    );

    config = PtaAnalysisConfig{};
    config.initialBjtVbe = std::numeric_limits<double>::quiet_NaN();
    expectInvalidArgument(
        [&config] { config.validate(); },
        "non-finite PTA BJT initial voltage is invalid"
    );

    config = PtaAnalysisConfig{};
    config.newtonOptions.maximumSolutionStep = 0.0;
    expectInvalidArgument(
        [&config] { config.validate(); },
        "invalid PTA Newton options are rejected"
    );
}

void testPstranKvgs0Mapping(){
    PstranAnalysisConfig pstran;
    pstran.kvgs0 = 1.25;
    pstran.kvgs0Specified = true;

    const PtaAnalysisConfig configured = pstran.makePtaConfig();
    expect(
        configured.initialMosVgs && *configured.initialMosVgs == 1.25,
        ".pstran kvgs0 maps to the MOS initial VGS limiter seed"
    );

    pstran.kvgs0Specified = false;
    expect(
        !pstran.makePtaConfig().initialMosVgs,
        "unspecified .pstran kvgs0 leaves the MOS limiter seed unset"
    );
}

void testPtaSourceRamp(){
    expectNear(
        ptaSourceRampScale(0.0, 0.0),
        1.0,
        "zero tauramp disables source ramping"
    );
    expectNear(
        ptaSourceRampScale(0.0, 2.0),
        0.0,
        "PTA source ramp starts at zero"
    );
    expectNear(
        ptaSourceRampScale(1.0, 2.0),
        0.5,
        "PTA cosine source ramp reaches half scale at its midpoint"
    );
    expectNear(
        ptaSourceRampScale(2.0, 2.0),
        1.0,
        "PTA source ramp reaches nominal scale at tauramp"
    );
    expectNear(
        ptaSourceRampScale(3.0, 2.0),
        1.0,
        "PTA source ramp remains at nominal scale"
    );
}

void testPtaCompoundPseudoElements(){
    MNA capacitorMna;
    capacitorMna.resize(2);
    capacitorMna.reservePattern(4);
    PseudoCapacitor capacitor(0, -1, 1.0, 1.0, 1.0, 1);
    capacitor.pattern(capacitorMna);
    capacitorMna.build();
    capacitor.bindMatrix(capacitorMna);

    const Eigen::VectorXd zero = Eigen::VectorXd::Zero(2);
    const TransientStampContext capContext{
        1.0,
        1.0,
        {1, 1.0, -1.0, 0.0},
        zero,
        nullptr
    };
    capacitorMna.clear();
    capacitor.stampPseudo(capContext);
    expectNear(
        *capacitorMna.ptr(1, 1),
        -(1.0 + std::exp(1.0)),
        "CEPTA RVC branch uses R0*exp(t/tau) in series with C"
    );

    MNA inductorMna;
    inductorMna.resize(2);
    inductorMna.reservePattern(1);
    PseudoInductor inductor(0, 1.0, 1.0, 1.0);
    inductor.pattern(inductorMna);
    inductorMna.build();
    inductor.bindMatrix(inductorMna);
    inductorMna.clear();
    inductor.stampPseudo(capContext);
    expectNear(
        *inductorMna.ptr(0, 0),
        -1.0 / (1.0 + std::exp(1.0)),
        "CEPTA GVL stamp uses G0*exp(t/tau) in parallel with L"
    );
}

void testFallbackDefersPtaForConvergentOperatingPoint(){
    PtaAnalysisConfig fallbackConfig;
    fallbackConfig.mode = PtaMode::Fallback;
    fallbackConfig.compoundTimeConstant = 1.0e-5;

    Circuit disabledCircuit;
    disabledCircuit.addDevice<NewtonBacktrackingDevice>(
        "XDISABLED",
        std::vector<std::string>{"node", "0", "0"}
    );
    expect(
        disabledCircuit.build(PtaAnalysisConfig{}),
        "disabled comparison circuit builds"
    );

    Circuit fallbackCircuit;
    fallbackCircuit.addDevice<NewtonBacktrackingDevice>(
        "XFALLBACK",
        std::vector<std::string>{"node", "0", "0"}
    );
    expect(
        fallbackCircuit.build(fallbackConfig),
        "compound fallback comparison circuit builds"
    );

    const CircuitDiagnostics before = fallbackCircuit.circuitDiagnostics();
    expect(
        before.unknownCount == 1 && before.pseudoDeviceCount == 0,
        "fallback build retains the physical MNA until ordinary OP fails"
    );

    OperatingPointSolverOptions options;
    options.sourceStepping.enabled = false;
    options.newton.maximumSolutionStep = 10.0;
    expect(
        disabledCircuit.solveOperatingPoint(options),
        "disabled comparison operating point converges"
    );
    expect(
        fallbackCircuit.solveOperatingPoint(options),
        "fallback ordinary operating point converges before PTA activation"
    );

    const NewtonSolveDiagnostics& disabledNewton =
        disabledCircuit.operatingPointDiagnostics().directNewton;
    const OperatingPointDiagnostics& fallbackDiagnostics =
        fallbackCircuit.operatingPointDiagnostics();
    const NewtonSolveDiagnostics& fallbackNewton =
        fallbackDiagnostics.directNewton;
    expect(
        !fallbackDiagnostics.ptaAttempted &&
            fallbackNewton.iterations == disabledNewton.iterations &&
            fallbackNewton.trustRegionTrials ==
                disabledNewton.trustRegionTrials &&
            fallbackNewton.trustRegionRejectedSteps ==
                disabledNewton.trustRegionRejectedSteps,
        "fallback ordinary Newton path matches disabled mode"
    );
    expectNear(
        CircuitPtaTestAccess::solution(fallbackCircuit)[0],
        CircuitPtaTestAccess::solution(disabledCircuit)[0],
        "fallback ordinary solution matches disabled mode"
    );

    const CircuitDiagnostics after = fallbackCircuit.circuitDiagnostics();
    expect(
        after.unknownCount == 1 && after.pseudoDeviceCount == 0,
        "successful fallback ordinary OP never materializes PTA devices"
    );
}

void testFallbackActivatesPtaAfterOrdinaryFailure(){
    PtaAnalysisConfig config;
    config.mode = PtaMode::Fallback;
    config.initialStep = 1.0e-6;
    config.minimumStep = 1.0e-9;
    config.maximumStep = 1.0e-6;
    config.maximumSteps = 4;
    config.compoundTimeConstant = 1.0e-5;

    Circuit circuit;
    circuit.addDevice<PtaRegularizedFloatingDevice>(
        "XPTAFLOAT",
        std::vector<std::string>{"node", "0", "0"}
    );
    expect(circuit.build(config), "lazy fallback activation fixture builds");

    AnalysisPlan plan;
    plan.nodeSets.push_back({"node", "0", 0.25});
    circuit.configureInitialConditions(plan);

    expect(
        !circuit.solveOperatingPoint(),
        "ordinary OP fails before lazy PTA activation"
    );
    const CircuitDiagnostics before = circuit.circuitDiagnostics();
    expect(
        before.unknownCount == 1 && before.pseudoDeviceCount == 0 &&
            circuit.operatingPointDiagnostics().directNewton.failureReason.find(
                "sparse linear system"
            ) != std::string::npos,
        "failed ordinary OP still used only the physical singular MNA"
    );

    expect(
        circuit.solveAdaptivePta(config),
        "fallback activates compound PTA after ordinary OP failure"
    );
    const CircuitDiagnostics after = circuit.circuitDiagnostics();
    const OperatingPointDiagnostics& diagnostics =
        circuit.operatingPointDiagnostics();
    expect(
        after.unknownCount == 2 && after.pseudoDeviceCount == 1 &&
            diagnostics.ptaAttempted && diagnostics.converged,
        "PTA activation expands and rebuilds the MNA exactly once"
    );
    expect(
        diagnostics.directNewton.attempted &&
            !diagnostics.directNewton.converged &&
            diagnostics.finalMethod.find("PTA") != std::string::npos,
        "fallback diagnostics retain ordinary failure before PTA success"
    );
    expectNear(
        CircuitPtaTestAccess::solution(circuit)[0],
        0.25,
        "PTA activation preserves the physical operating-point state"
    );
    expectNear(
        CircuitPtaTestAccess::solution(circuit)[1],
        0.0,
        "PTA activation zero-initializes its new branch unknown"
    );
}

void testPtaNormalizedDerivative(){
    PtaAnalysisConfig config;
    config.derivativeRelativeTolerance = 0.1;
    config.derivativeVoltageAbsoluteTolerance = 1.0;
    config.derivativeCurrentAbsoluteTolerance = 1.0e-2;

    const PtaDerivativeEstimate estimate = estimatePtaNormalizedDerivative(
        makeVector({1.0, 1.0e-2}),
        makeVector({10.0, 1.0e-1}),
        makeVector({8.0, 5.0e-2}),
        1,
        2.0,
        config
    );
    expect(estimate.valid, "scaled PTA derivative estimate is valid");
    expectNear(
        estimate.normalizedDerivative,
        1.0,
        "PTA derivative uses time step and voltage/current scales"
    );

    const PtaDerivativeEstimate smallerStepEstimate =
        estimatePtaNormalizedDerivative(
            makeVector({1.0, 1.0e-2}),
            makeVector({10.0, 1.0e-1}),
            makeVector({8.0, 5.0e-2}),
            1,
            0.5,
            config
        );
    expect(smallerStepEstimate.valid, "short-step PTA derivative is valid");
    expectNear(
        smallerStepEstimate.normalizedDerivative,
        0.25,
        "PTA derivative normalization scales with pseudo-time step"
    );
}

void testPtaNormalizedResidual(){
    PtaAnalysisConfig config;
    config.dcResidualRelativeTolerance = 0.1;
    config.dcCurrentAbsoluteTolerance = 1.0;
    config.dcVoltageAbsoluteTolerance = 1.0e-2;

    const PtaResidualEstimate estimate = estimatePtaNormalizedResidual(
        makeVector({2.0, 2.0e-2}),
        makeVector({10.0, 1.0e-1}),
        makeVector({8.0, 5.0e-2}),
        1,
        config
    );
    expect(estimate.valid, "scaled PTA residual estimate is valid");
    expectNear(
        estimate.normalizedResidual,
        1.0,
        "PTA residual uses current and voltage row scales"
    );

    const PtaResidualEstimate invalidEstimate =
        estimatePtaNormalizedResidual(
            makeVector({0.0}),
            makeVector({0.0}),
            makeVector({0.0}),
            2,
            config
        );
    expect(!invalidEstimate.valid, "invalid PTA residual dimensions are rejected");
}

void testPtaNodeCapacitanceGrowth(){
    PtaAnalysisConfig config;
    config.capacitanceGrowScale = 2.0;
    config.maximumNodeCapacitance = 1.0e-3;

    Circuit circuit;
    CircuitPtaTestAccess::addNodeCapacitor(
        circuit,
        0,
        1.0e-12,
        0.25,
        true
    );
    CircuitPtaTestAccess::addNodeCapacitor(
        circuit,
        1,
        7.5e-4,
        -0.5,
        true
    );

    const auto firstGrowth = CircuitPtaTestAccess::growForResidual(
        circuit,
        makeVector({0.25, -0.5}),
        config
    );
    expect(firstGrowth && firstGrowth->nodeIndex == 1 &&
           firstGrowth->residualMagnitude == 0.5,
           "PTA growth selects the largest residual node");
    expectNear(
        CircuitPtaTestAccess::capacitance(circuit, 0),
        1.0e-12,
        "PTA growth leaves unrelated node capacitances unchanged"
    );
    expectNear(
        CircuitPtaTestAccess::capacitance(circuit, 1),
        config.maximumNodeCapacitance,
        "PTA growth clamps a node capacitance at its maximum"
    );
    expectNear(
        CircuitPtaTestAccess::previousDelta(circuit, 0),
        0.25,
        "unselected PTA node retains its oscillation delta"
    );
    expect(
        CircuitPtaTestAccess::hasPreviousDelta(circuit, 0),
        "unselected PTA node retains its oscillation history flag"
    );
    expectNear(
        CircuitPtaTestAccess::previousDelta(circuit, 1),
        0.0,
        "PTA growth clears the capped node oscillation delta"
    );
    expect(
        !CircuitPtaTestAccess::hasPreviousDelta(circuit, 1),
        "PTA growth clears the capped node oscillation history flag"
    );

    const auto secondGrowth = CircuitPtaTestAccess::growForResidual(
        circuit,
        makeVector({0.25, -0.5}),
        config
    );
    expect(secondGrowth && secondGrowth->nodeIndex == 0,
           "PTA growth falls back to the next residual node with headroom");
    expectNear(
        CircuitPtaTestAccess::capacitance(circuit, 0),
        2.0e-12,
        "PTA growth scales the selected fallback node"
    );

    Circuit saturatedCircuit;
    CircuitPtaTestAccess::addNodeCapacitor(
        saturatedCircuit,
        0,
        config.maximumNodeCapacitance,
        0.0,
        false
    );
    expect(
        !CircuitPtaTestAccess::growForResidual(
            saturatedCircuit,
            makeVector({1.0}),
            config
        ),
        "PTA growth fails when every node capacitance is saturated"
    );
}

void testPtaNodeCapacitanceOscillationAdaptation(){
    PtaAnalysisConfig config;
    config.minimumNodeCapacitance = 3.0e-2;
    config.smallOscillationScale = 0.9;
    config.mediumOscillationScale = 0.7;
    config.heavyOscillationScale = 0.5;
    config.mediumOscillationRatio = 0.5;
    config.heavyOscillationRatio = 1.0;

    Circuit circuit;
    CircuitPtaTestAccess::addNodeCapacitor(
        circuit,
        0,
        1.0e-1,
        0.0,
        false
    );

    CircuitPtaTestAccess::updateAfterAcceptedStep(
        circuit,
        makeVector({1.1}),
        makeVector({1.0}),
        config
    );
    expectNear(
        CircuitPtaTestAccess::capacitance(circuit, 0),
        1.0e-1,
        "first PTA node delta does not change capacitance"
    );
    expectNear(
        CircuitPtaTestAccess::previousDelta(circuit, 0),
        1.0e-1,
        "first PTA node delta is recorded"
    );
    expect(
        CircuitPtaTestAccess::hasPreviousDelta(circuit, 0),
        "first PTA node delta enables oscillation history"
    );

    CircuitPtaTestAccess::updateAfterAcceptedStep(
        circuit,
        makeVector({1.2}),
        makeVector({1.1}),
        config
    );
    expectNear(
        CircuitPtaTestAccess::capacitance(circuit, 0),
        1.0e-1,
        "same-sign PTA node delta does not change capacitance"
    );

    CircuitPtaTestAccess::updateAfterAcceptedStep(
        circuit,
        makeVector({0.96}),
        makeVector({1.0}),
        config
    );
    expectNear(
        CircuitPtaTestAccess::capacitance(circuit, 0),
        9.0e-2,
        "small PTA oscillation applies the small capacitance scale"
    );

    CircuitPtaTestAccess::updateAfterAcceptedStep(
        circuit,
        makeVector({1.03}),
        makeVector({1.0}),
        config
    );
    expectNear(
        CircuitPtaTestAccess::capacitance(circuit, 0),
        6.3e-2,
        "medium PTA oscillation applies the medium capacitance scale"
    );

    CircuitPtaTestAccess::updateAfterAcceptedStep(
        circuit,
        makeVector({0.94}),
        makeVector({1.0}),
        config
    );
    expectNear(
        CircuitPtaTestAccess::capacitance(circuit, 0),
        3.15e-2,
        "heavy PTA oscillation applies the heavy capacitance scale"
    );

    CircuitPtaTestAccess::updateAfterAcceptedStep(
        circuit,
        makeVector({1.12}),
        makeVector({1.0}),
        config
    );
    expectNear(
        CircuitPtaTestAccess::capacitance(circuit, 0),
        config.minimumNodeCapacitance,
        "PTA oscillation adaptation clamps capacitance at its minimum"
    );
}

void testPtaMinimumStepCapacitanceRecovery(){
    PtaAnalysisConfig config;
    config.mode = PtaMode::Force;
    config.initialStep = 1.0;
    config.minimumStep = 1.0;
    config.maximumStep = 1024.0;
    config.maximumSteps = 100;
    config.derivativeTolerance = 1.0;
    config.dcResidualTolerance = 1.0;
    config.initialNodeCapacitance = 1.0e-6;
    config.minimumNodeCapacitance = 1.0e-6;
    config.maximumNodeCapacitance = 1.0;
    config.capacitanceGrowScale = 1.0e6;
    config.successfulStepScale = 2.0;

    Circuit circuit;
    circuit.addDevice<PtaRecoveryDevice>(
        "XPTA",
        std::vector<std::string>{"node", "0", "0"},
        1.0,
        1000.0
    );

    expect(circuit.build(config), "PTA recovery fixture builds");
    expect(
        circuit.solveAdaptivePta(config),
        "PTA recovers after a minimum-step capacitance increase"
    );
    expect(
        CircuitPtaTestAccess::ptaMinimumStepRecoveries(circuit) > 0,
        "PTA recovery fixture reaches the minimum-step recovery path"
    );
    expect(
        CircuitPtaTestAccess::ptaCapacitanceGrowths(circuit) > 0,
        "PTA recovery fixture records a capacitance growth"
    );
    expect(
        CircuitPtaTestAccess::ptaCapacitanceReductions(circuit) > 0,
        "PTA recovery fixture records a node-capacitance reduction"
    );

    const PtaDiagnostics diagnostics = circuit.ptaDiagnostics();
    expect(diagnostics.attempted, "PTA recovery fixture exposes diagnostics");
    expect(diagnostics.converged, "PTA diagnostics report convergence");
    expect(
        diagnostics.hasConvergenceMetrics,
        "PTA diagnostics include normalized convergence metrics"
    );
    expect(
        diagnostics.minimumStepRecoveries > 0 &&
            diagnostics.capacitanceGrowths > 0 &&
            diagnostics.capacitanceReductions > 0,
        "PTA diagnostics expose adaptive recovery counters"
    );
    expect(
        !diagnostics.attempts.empty(),
        "PTA diagnostics expose the per-attempt trace"
    );
    expect(
        std::any_of(
            diagnostics.attempts.begin(),
            diagnostics.attempts.end(),
            [](const PtaStepAttemptDiagnostics& attempt){
                return attempt.newton.usedTrustRegion &&
                    attempt.newton.trustRegionTrials > 0;
            }
        ),
        "PTA uses trust-region Newton trials at pseudo-time points"
    );
    expect(
        diagnostics.attempts.back().reachedSteadyState &&
            diagnostics.attempts.back().hasConvergenceMetrics,
        "PTA diagnostic trace ends with a measured steady-state decision"
    );
    expect(
        std::any_of(
            diagnostics.attempts.begin(),
            diagnostics.attempts.end(),
            [](const PtaStepAttemptDiagnostics& attempt){
                return attempt.restartedAfterCapacitanceGrowth &&
                    attempt.capacitanceGrowths == 1 &&
                    attempt.capacitanceGrowthNodes.size() == 1 &&
                    attempt.capacitanceGrowthNodes.front().nodeName == "node" &&
                    attempt.capacitanceGrowthNodes.front().residualMagnitude >= 0.0 &&
                    attempt.capacitanceGrowthNodes.front().capacitanceAfter >
                        attempt.capacitanceGrowthNodes.front().capacitanceBefore &&
                    attempt.retryTimeStep > 0.0;
            }
        ),
        "PTA trace records the residual-selected growth node and retry step"
    );
    expect(
        std::any_of(
            diagnostics.attempts.begin(),
            diagnostics.attempts.end(),
            [](const PtaStepAttemptDiagnostics& attempt){
                return attempt.capacitanceReductions > 0 &&
                    attempt.capacitanceReductions ==
                        attempt.smallOscillationCapacitanceReductions +
                        attempt.mediumOscillationCapacitanceReductions +
                        attempt.heavyOscillationCapacitanceReductions;
            }
        ),
        "PTA trace categorizes node-capacitance reductions by oscillation severity"
    );

    const OperatingPointDiagnostics& detailed =
        circuit.operatingPointDiagnostics();
    expect(
        detailed.attempted && detailed.converged && detailed.ptaAttempted,
        "detailed operating-point diagnostics retain the PTA method"
    );
    expect(
        detailed.finalMethod.find("PTA") != std::string::npos,
        "detailed operating-point diagnostics identify the final method"
    );
    expect(
        detailed.ptaIterations > 0 &&
        detailed.iterations >= detailed.ptaIterations,
        "detailed PTA diagnostics retain total iteration counts"
    );
    expect(
        detailed.ptaAcceptedSteps > 0 &&
        detailed.ptaRejectedSteps > 0,
        "detailed PTA diagnostics separate accepted and rejected steps"
    );
    expect(
        !detailed.ptaAttempts.empty() &&
        detailed.ptaAttempts.back().reachedSteadyState,
        "PTA attempt history ends with the steady-state decision"
    );
    expect(
        std::any_of(
            detailed.ptaAttempts.begin(),
            detailed.ptaAttempts.end(),
            [](const PtaStepAttemptDiagnostics& attempt){
                return attempt.restartedAfterCapacitanceGrowth;
            }
        ),
        "PTA attempt history records capacitance-growth recovery"
    );
    expect(
        detailed.ptaWallSeconds >= 0.0 && detailed.wallSeconds >= 0.0,
        "PTA diagnostics expose non-negative wall-clock durations"
    );

    const CircuitDiagnostics circuitInfo = circuit.circuitDiagnostics();
    expect(
        circuitInfo.deviceCount == 1 &&
        circuitInfo.nonlinearDeviceCount == 1 &&
        circuitInfo.nodeCount == 1 &&
        circuitInfo.unknownCount == 1,
        "circuit diagnostics expose topology and nonlinear-device counts"
    );
    expect(
        circuitInfo.pseudoDeviceCount > 0 &&
        circuitInfo.matrixNonZeros > 0 &&
        circuitInfo.hasFiniteSolution,
        "circuit diagnostics expose PTA structure and a finite solution"
    );
}

void testPtaTrustRegionExhaustionRecovery(){
    PtaAnalysisConfig config;
    config.mode = PtaMode::Force;
    config.initialStep = 1.0;
    config.minimumStep = 1.0;
    config.maximumStep = 1.0;
    config.maximumSteps = 1;
    config.derivativeTolerance = 2.0;
    config.derivativeVoltageAbsoluteTolerance = 1.0;
    config.derivativeCurrentAbsoluteTolerance = 1.0;
    config.initialBjtVbe = 0.0;
    config.newtonOptions.maximumTrustRegionRetries = 0;

    Circuit circuit;
    circuit.addDevice<NewtonResidualTrapDevice>(
        "XPTATRUST",
        std::vector<std::string>{"node", "0", "0"},
        0.5,
        1.0
    );

    expect(circuit.build(config),
           "PTA trust-region recovery fixture builds");
    expect(circuit.solveAdaptivePta(config),
           "PTA recovers from exhausted trust-region retries");

    const PtaDiagnostics diagnostics = circuit.ptaDiagnostics();
    expect(diagnostics.attempts.size() == 1,
           "PTA trust-region recovery completes at the original pseudo-time point");
    expect(
        !diagnostics.attempts.empty() &&
            diagnostics.attempts.front().newton.converged &&
            diagnostics.attempts.front().newton.usedTrustRegion &&
            diagnostics.attempts.front().newton.trustRegionRetriesExhausted &&
            diagnostics.attempts.front().newton
                .usedTrustRegionExhaustionRecovery &&
            diagnostics.attempts.front().newton
                .trustRegionExhaustionRecoveryIterations > 0 &&
            diagnostics.attempts.front().newton.nonMonotoneStepFallbacks > 0,
        "PTA trace records checkpointed non-monotone recovery after trust-region exhaustion"
    );
    expect(
        diagnostics.capacitanceGrowths == 0 &&
            diagnostics.attempts.front().newton.trustRegionTrials == 1 &&
            diagnostics.attempts.front().newton.trustRegionRejectedSteps == 1,
        "PTA recovery avoids outer capacitance growth after a local trust-region rejection"
    );
}

void testLinearSolverDiagnostics(){
    Circuit circuit;
    circuit.addDevice<Resistor>(
        "R1",
        std::vector<std::string>{"out", "0"},
        1.0e3
    );
    circuit.addDevice<CurrentSource>(
        "I1",
        std::vector<std::string>{"out", "0"},
        1.0e-3
    );

    expect(circuit.build(PtaAnalysisConfig{}), "linear diagnostics fixture builds");
    expect(circuit.solveOperatingPoint(), "linear operating point converges");

    const OperatingPointDiagnostics& operatingPoint =
        circuit.operatingPointDiagnostics();
    expect(
        operatingPoint.attempted && operatingPoint.converged &&
            operatingPoint.finalMethod == "linear",
        "linear OP diagnostics identify the direct linear method"
    );
    expect(
        operatingPoint.directNewton.attempted &&
            !operatingPoint.directNewton.usedNewtonRaphson &&
            operatingPoint.directNewton.converged &&
            operatingPoint.directNewton.iterations == 1,
        "linear OP diagnostics distinguish sparse solve from Newton-Raphson"
    );
    expect(
        operatingPoint.sourceAttempts.empty() && !operatingPoint.ptaAttempted &&
            operatingPoint.iterations == 1,
        "linear OP diagnostics do not invent continuation or PTA attempts"
    );

    const CircuitDiagnostics circuitInfo = circuit.circuitDiagnostics();
    expect(
        circuitInfo.deviceCount == 2 &&
            circuitInfo.nonlinearDeviceCount == 0 &&
            circuitInfo.nodeCount == 1 && circuitInfo.unknownCount == 1,
        "linear circuit diagnostics expose topology counts"
    );
    expect(
        circuitInfo.matrixNonZeros == 1 && circuitInfo.hasFiniteSolution &&
            circuitInfo.devicesByType.size() == 2,
        "linear circuit diagnostics expose matrix and solution characteristics"
    );

    TransientAnalysisConfig transient;
    transient.outputInterval = 1.0e-6;
    transient.stopTime = 3.0e-6;
    transient.maximumStep = transient.outputInterval;
    transient.useInitialConditions = true;
    expect(circuit.solveTransient(transient), "linear transient converges");

    const TransientDiagnostics& transientInfo = circuit.transientDiagnostics();
    expect(
        transientInfo.attempted && transientInfo.converged &&
            transientInfo.timeSteps > 0 && transientInfo.attemptedSteps > 0,
        "linear transient diagnostics record completed time stepping"
    );
    expect(
        transientInfo.lastNewton.attempted &&
            !transientInfo.lastNewton.usedNewtonRaphson &&
            transientInfo.lastNewton.converged,
        "linear transient diagnostics distinguish sparse solve from Newton-Raphson"
    );
    expect(
        transientInfo.minimumAttemptedStep > 0.0 &&
            transientInfo.maximumAcceptedStep > 0.0 &&
            transientInfo.cpuSeconds >= transientInfo.initializationCpuSeconds &&
            transientInfo.wallSeconds >= transientInfo.initializationWallSeconds,
        "linear transient diagnostics retain finite step ranges and timing"
    );
}

void testLinearFailureDiagnostics(){
    Circuit circuit;
    circuit.addDevice<VoltageSource>(
        "V1",
        std::vector<std::string>{"out", "0"},
        1.0
    );
    circuit.addDevice<VoltageSource>(
        "V2",
        std::vector<std::string>{"out", "0"},
        2.0
    );

    expect(circuit.build(PtaAnalysisConfig{}), "singular linear fixture builds");
    expect(!circuit.solveOperatingPoint(), "conflicting ideal sources fail");

    const OperatingPointDiagnostics& diagnostics =
        circuit.operatingPointDiagnostics();
    expect(
        diagnostics.attempted && !diagnostics.converged &&
            diagnostics.finalMethod == "linear",
        "failed linear OP retains its method classification"
    );
    expect(
        diagnostics.directNewton.attempted &&
            !diagnostics.directNewton.usedNewtonRaphson &&
            !diagnostics.directNewton.converged &&
            diagnostics.directNewton.iterations == 1,
        "failed linear OP records one sparse-solve attempt"
    );
    expect(
        diagnostics.sourceAttempts.empty() && !diagnostics.ptaAttempted &&
            diagnostics.failureReason.find("sparse linear system") !=
                std::string::npos,
        "failed linear OP records its sparse-system failure without fallback history"
    );
}

void testNewtonRequiresReassembledResidual(){
    Circuit circuit;
    circuit.addDevice<NewtonResidualTrapDevice>(
        "XNEWTON",
        std::vector<std::string>{"node", "0", "0"}
    );

    expect(circuit.build(PtaAnalysisConfig{}), "Newton residual fixture builds");
    OperatingPointSolverOptions options;
    options.sourceStepping.enabled = false;
    expect(!circuit.solveOperatingPoint(options),
           "Newton residual fixture rejects an inconsistent tiny update");

    const NewtonSolveDiagnostics& diagnostics =
        circuit.operatingPointDiagnostics().directNewton;
    expect(
        !diagnostics.converged && diagnostics.iterations == 1 &&
            diagnostics.usedTrustRegion &&
            diagnostics.trustRegionRejectedSteps > 0 &&
            diagnostics.trustRegionRadiusReductions > 0,
        "trust region rejects an inconsistent tiny update after restamping"
    );
    expect(
        !diagnostics.hasNormalizedUpdate && !diagnostics.hasNormalizedResidual &&
            diagnostics.trustRegionRetriesExhausted &&
            diagnostics.failureReason.find("trust-region") != std::string::npos,
        "Newton reports exhausted trust-region retries distinctly"
    );
}

void testNewtonResidualBacktracking(){
    Circuit circuit;
    circuit.addDevice<NewtonBacktrackingDevice>(
        "XBACKTRACK",
        std::vector<std::string>{"node", "0", "0"}
    );

    expect(circuit.build(PtaAnalysisConfig{}), "Newton backtracking fixture builds");
    OperatingPointSolverOptions options;
    options.sourceStepping.enabled = false;
    options.newton.maximumSolutionStep = 10.0;
    expect(circuit.solveOperatingPoint(options),
           "Newton residual backtracking fixture converges");

    const NewtonSolveDiagnostics& diagnostics =
        circuit.operatingPointDiagnostics().directNewton;
    expect(
        diagnostics.converged && diagnostics.usedTrustRegion &&
            diagnostics.trustRegionTrials > 1 &&
            diagnostics.trustRegionRejectedSteps > 0 &&
            diagnostics.trustRegionRadiusReductions > 0 &&
            diagnostics.hasTrustRegionRatio,
        "Newton accepts a trust-region candidate after a rejected full step"
    );
}

void testTrustRegionExpandsForAccurateModel(){
    Circuit circuit;
    circuit.addDevice<PtaRecoveryDevice>(
        "XTRUST",
        std::vector<std::string>{"node", "0", "0"},
        1.0,
        1.0
    );

    expect(circuit.build(PtaAnalysisConfig{}),
           "trust-region agreement fixture builds");
    OperatingPointSolverOptions options;
    options.sourceStepping.enabled = false;
    expect(circuit.solveOperatingPoint(options),
           "trust-region agreement fixture converges");

    const NewtonSolveDiagnostics& diagnostics =
        circuit.operatingPointDiagnostics().directNewton;
    expect(
        diagnostics.usedTrustRegion && diagnostics.trustRegionTrials > 0 &&
            diagnostics.trustRegionRadiusExpansions > 0 &&
            diagnostics.finalTrustRegionRadius >
                diagnostics.initialTrustRegionRadius,
        "accurate trust-region model expands its normalized radius"
    );
}

void testControlledNonMonotoneNewtonFallback(){
    Circuit circuit;
    circuit.addDevice<NewtonResidualTrapDevice>(
        "XCONTROLLED",
        std::vector<std::string>{"node", "0", "0"},
        0.5,
        1.0
    );

    expect(circuit.build(PtaAnalysisConfig{}),
           "controlled Newton fallback fixture builds");
    OperatingPointSolverOptions options;
    options.sourceStepping.enabled = false;
    options.newton.trustRegionEnabled = false;
    expect(circuit.solveOperatingPoint(options),
           "default bounded non-monotone Newton fallback converges");

    const NewtonSolveDiagnostics& diagnostics =
        circuit.operatingPointDiagnostics().directNewton;
    expect(
        diagnostics.converged && diagnostics.nonMonotoneStepFallbacks == 1 &&
            diagnostics.lineSearchEvaluations > options.newton.maximumBacktracks,
        "Newton records one default-bounded non-monotone fallback after Armijo exhaustion"
    );
}

void testRequiresBdf2History(){
    TransientIntegrator integrator;
    integrator.initialize(0.0, makeVector({0.0}));

    const TransientErrorEstimate estimate = integrator.estimateError(
        1.0,
        makeVector({0.0}),
        1,
        TransientSolverOptions{}
    );

    expectDefaultInvalid(estimate, "BE startup has no BDF2 estimate");
}

void testZeroErrorUsesMaximumScale(){
    const TransientIntegrator integrator = makeIntegratorWithHistory(
        makeVector({1.0, 1.0}),
        makeVector({2.0, 3.0})
    );
    TransientSolverOptions options = makeAbsoluteOnlyOptions();

    const TransientErrorEstimate estimate = integrator.estimateError(
        2.0,
        makeVector({3.0, 5.0}),
        1,
        options
    );

    expect(estimate.valid, "zero-error BDF2 estimate is valid");
    expectNear(estimate.normalizedError, 0.0, "zero normalized error");
    expectNear(
        estimate.suggestedScale,
        options.maximumScale,
        "zero error uses maximum scale"
    );
}

void testVoltageAndCurrentAbsoluteTolerances(){
    const TransientIntegrator integrator = makeIntegratorWithHistory(
        makeVector({0.0, 0.0}),
        makeVector({0.0, 0.0})
    );
    TransientSolverOptions options = makeAbsoluteOnlyOptions();
    options.voltageAbsoluteTolerance = 2.0;
    options.currentAbsoluteTolerance = 0.5;

    const TransientErrorEstimate estimate = integrator.estimateError(
        2.0,
        makeVector({1.0, 1.0}),
        1,
        options
    );

    expect(estimate.valid, "mixed voltage/current estimate is valid");
    expectNear(
        estimate.normalizedError,
        2.0,
        "current unknown uses current absolute tolerance"
    );
    expectNear(
        estimate.suggestedScale,
        options.safetyFactor / std::sqrt(2.0),
        "mixed tolerance scale"
    );

    const TransientErrorEstimate voltageOnlyEstimate = integrator.estimateError(
        2.0,
        makeVector({1.0, 0.0}),
        1,
        options
    );
    expectNear(
        voltageOnlyEstimate.normalizedError,
        0.5,
        "voltage unknown uses voltage absolute tolerance"
    );
}

void testRelativeWeight(){
    const TransientIntegrator integrator = makeIntegratorWithHistory(
        makeVector({1.0}),
        makeVector({2.0})
    );
    TransientSolverOptions options = makeAbsoluteOnlyOptions();
    options.relativeTolerance = 0.5;

    const TransientErrorEstimate estimate = integrator.estimateError(
        2.0,
        makeVector({4.0}),
        1,
        options
    );

    const double expectedError = 1.0 / 3.0;
    expect(estimate.valid, "relative-weight estimate is valid");
    expectNear(
        estimate.normalizedError,
        expectedError,
        "relative weight uses max(corrected, accepted)"
    );
    expectNear(
        estimate.suggestedScale,
        options.safetyFactor / std::sqrt(expectedError),
        "relative-weight suggested scale"
    );
}

void testScaleBoundaries(){
    const TransientIntegrator integrator = makeIntegratorWithHistory(
        makeVector({0.0}),
        makeVector({0.0})
    );
    const TransientSolverOptions options = makeAbsoluteOnlyOptions();

    const TransientErrorEstimate unitError = integrator.estimateError(
        2.0,
        makeVector({1.0}),
        1,
        options
    );
    expectNear(unitError.normalizedError, 1.0, "unit normalized error");
    expectNear(
        unitError.suggestedScale,
        options.safetyFactor,
        "unit error uses safety factor"
    );

    const TransientErrorEstimate smallError = integrator.estimateError(
        2.0,
        makeVector({0.01}),
        1,
        options
    );
    expectNear(smallError.normalizedError, 0.01, "small normalized error");
    expectNear(
        smallError.suggestedScale,
        options.maximumScale,
        "small error clamps to maximum scale"
    );

    const TransientErrorEstimate largeError = integrator.estimateError(
        2.0,
        makeVector({100.0}),
        1,
        options
    );
    expectNear(largeError.normalizedError, 100.0, "large normalized error");
    expectNear(
        largeError.suggestedScale,
        options.minimumScale,
        "large error clamps to minimum scale"
    );
}

void testVariableStepPredictor(){
    const TransientIntegrator integrator = makeIntegratorWithHistory(
        makeVector({0.0}),
        makeVector({4.0}),
        2.0
    );
    TransientSolverOptions options = makeAbsoluteOnlyOptions();
    options.voltageAbsoluteTolerance = 2.0;

    const TransientErrorEstimate estimate = integrator.estimateError(
        3.0,
        makeVector({7.0}),
        1,
        options
    );

    expect(estimate.valid, "variable-step estimate is valid");
    expectNear(
        estimate.normalizedError,
        0.5,
        "variable-step ratio is used by predictor"
    );
    expectNear(
        estimate.suggestedScale,
        options.safetyFactor / std::sqrt(0.5),
        "variable-step suggested scale"
    );
}

void testMaximumBdf2StepRatio(){
    const TransientIntegrator integrator = makeIntegratorWithHistory(
        makeVector({0.0}),
        makeVector({1.0})
    );
    const TransientSolverOptions options = makeAbsoluteOnlyOptions();

    const TransientErrorEstimate boundaryEstimate = integrator.estimateError(
        3.0,
        makeVector({4.0}),
        1,
        options
    );
    expect(boundaryEstimate.valid, "BDF2 ratio 2 boundary is valid");
    expectNear(
        boundaryEstimate.normalizedError,
        1.0,
        "BDF2 ratio 2 predictor"
    );

    const double aboveBoundary = std::nextafter(
        3.0,
        std::numeric_limits<double>::infinity()
    );
    const TransientErrorEstimate fallbackEstimate = integrator.estimateError(
        aboveBoundary,
        makeVector({4.0}),
        1,
        options
    );
    expectDefaultInvalid(
        fallbackEstimate,
        "step ratio above 2 falls back to BE"
    );
}

void testInvalidInputs(){
    const TransientIntegrator integrator = makeIntegratorWithHistory(
        makeVector({0.0}),
        makeVector({0.0})
    );
    const TransientSolverOptions validOptions = makeAbsoluteOnlyOptions();

    expectDefaultInvalid(
        integrator.estimateError(
            2.0, makeVector({0.0, 0.0}), 1, validOptions
        ),
        "solution size mismatch"
    );
    expectDefaultInvalid(
        integrator.estimateError(2.0, makeVector({0.0}), -1, validOptions),
        "negative voltage unknown count"
    );
    expectDefaultInvalid(
        integrator.estimateError(2.0, makeVector({0.0}), 2, validOptions),
        "voltage unknown count exceeds solution size"
    );
    expectDefaultInvalid(
        integrator.estimateError(
            2.0,
            makeVector({std::numeric_limits<double>::quiet_NaN()}),
            1,
            validOptions
        ),
        "NaN corrected solution"
    );
    expectDefaultInvalid(
        integrator.estimateError(
            2.0,
            makeVector({std::numeric_limits<double>::infinity()}),
            1,
            validOptions
        ),
        "infinite corrected solution"
    );
    expectDefaultInvalid(
        integrator.estimateError(
            std::numeric_limits<double>::quiet_NaN(),
            makeVector({0.0}),
            1,
            validOptions
        ),
        "NaN target time"
    );
    expectDefaultInvalid(
        integrator.estimateError(1.0, makeVector({0.0}), 1, validOptions),
        "target time does not advance"
    );

    const TransientIntegrator emptyIntegrator = makeIntegratorWithHistory(
        Eigen::VectorXd{},
        Eigen::VectorXd{}
    );
    expectDefaultInvalid(
        emptyIntegrator.estimateError(
            2.0, Eigen::VectorXd{}, 0, validOptions
        ),
        "empty solution"
    );

    TransientSolverOptions invalidOptions = validOptions;
    invalidOptions.relativeTolerance = -1.0;
    expectDefaultInvalid(
        integrator.estimateError(2.0, makeVector({0.0}), 1, invalidOptions),
        "negative relative tolerance"
    );

    invalidOptions = validOptions;
    invalidOptions.voltageAbsoluteTolerance = 0.0;
    expectDefaultInvalid(
        integrator.estimateError(2.0, makeVector({0.0}), 1, invalidOptions),
        "zero voltage absolute tolerance"
    );

    invalidOptions = validOptions;
    invalidOptions.currentAbsoluteTolerance = 0.0;
    expectDefaultInvalid(
        integrator.estimateError(2.0, makeVector({0.0}), 1, invalidOptions),
        "zero current absolute tolerance"
    );

    invalidOptions = validOptions;
    invalidOptions.safetyFactor = std::numeric_limits<double>::infinity();
    expectDefaultInvalid(
        integrator.estimateError(2.0, makeVector({0.0}), 1, invalidOptions),
        "infinite safety factor"
    );

    invalidOptions = validOptions;
    invalidOptions.minimumScale = 0.0;
    expectDefaultInvalid(
        integrator.estimateError(2.0, makeVector({0.0}), 1, invalidOptions),
        "zero minimum scale"
    );

    invalidOptions = validOptions;
    invalidOptions.maximumScale = invalidOptions.minimumScale / 2.0;
    expectDefaultInvalid(
        integrator.estimateError(2.0, makeVector({0.0}), 1, invalidOptions),
        "maximum scale below minimum scale"
    );

    const TransientIntegrator nonFiniteHistory = makeIntegratorWithHistory(
        makeVector({0.0}),
        makeVector({std::numeric_limits<double>::infinity()})
    );
    expectDefaultInvalid(
        nonFiniteHistory.estimateError(
            2.0, makeVector({0.0}), 1, validOptions
        ),
        "non-finite accepted history"
    );

    const double maximumFinite = std::numeric_limits<double>::max();
    const TransientIntegrator overflowingWeight = makeIntegratorWithHistory(
        makeVector({maximumFinite}),
        makeVector({maximumFinite})
    );
    TransientSolverOptions overflowingWeightOptions = validOptions;
    overflowingWeightOptions.relativeTolerance = 2.0;
    expectDefaultInvalid(
        overflowingWeight.estimateError(
            2.0,
            makeVector({maximumFinite}),
            1,
            overflowingWeightOptions
        ),
        "overflowing error weight"
    );
}

// The compatibility proxy remains available to direct callers, while Circuit
// uses the strict defect estimator below for adaptive transient stepping.
void testQuadraticSolutionCharacterizesProxy(){
    const TransientIntegrator integrator = makeIntegratorWithHistory(
        makeVector({0.0}),
        makeVector({1.0})
    );
    const TransientSolverOptions options = makeAbsoluteOnlyOptions();

    const TransientErrorEstimate estimate = integrator.estimateError(
        2.0,
        makeVector({4.0}),
        1,
        options
    );

    expect(estimate.valid, "quadratic proxy estimate is valid");
    expectNear(
        estimate.normalizedError,
        2.0,
        "quadratic solution records linear-predictor defect"
    );
    expectNear(
        estimate.suggestedScale,
        options.safetyFactor / std::sqrt(2.0),
        "quadratic proxy uses second-order scale exponent"
    );
}

void testStrictBdf2LteRejectsInsufficientHistory(){
    const TransientIntegrator integrator = makeIntegratorWithHistory(
        makeVector({0.0}),
        makeVector({1.0})
    );

    const TransientLteDefect defect = integrator.strictBdf2Defect(
        2.0,
        makeVector({4.0})
    );
    expect(!defect.valid, "strict BDF2 LTE requires three accepted states");
}

void testStrictBdf2LteIsExactForQuadratic(){
    const TransientIntegrator integrator = makeIntegratorWithStrictHistory(
        makeVector({0.0}),
        makeVector({1.0}),
        makeVector({4.0})
    );

    const TransientLteDefect defect = integrator.strictBdf2Defect(
        3.0,
        makeVector({9.0})
    );
    expect(defect.valid, "strict BDF2 LTE accepts complete history");
    expectNear(
        defect.derivativeDefect[0],
        0.0,
        "strict BDF2 derivative defect vanishes for a quadratic solution"
    );
}

void testStrictBdf2LteCubicDefect(){
    const TransientIntegrator integrator = makeIntegratorWithStrictHistory(
        makeVector({0.0}),
        makeVector({1.0}),
        makeVector({8.0})
    );

    const TransientLteDefect defect = integrator.strictBdf2Defect(
        3.0,
        makeVector({27.0})
    );
    expect(defect.valid, "strict cubic BDF2 defect is valid");
    expectNear(
        defect.derivativeDefect[0],
        2.0,
        "uniform-step cubic BDF2 derivative defect"
    );

    const TransientIntegrator variableStep = makeIntegratorWithStrictHistory(
        makeVector({0.0}),
        makeVector({1.0}),
        makeVector({27.0}),
        1.0,
        3.0
    );
    const TransientLteDefect variableDefect = variableStep.strictBdf2Defect(
        4.0,
        makeVector({64.0})
    );
    expect(variableDefect.valid, "variable-step cubic BDF2 defect is valid");
    expectNear(
        variableDefect.derivativeDefect[0],
        3.0,
        "variable-step cubic BDF2 derivative defect"
    );
}

void testStrictBdf2LteUsesThirdOrderControl(){
    const TransientSolverOptions options = makeAbsoluteOnlyOptions();
    const TransientErrorEstimate estimate = estimateTransientStateError(
        makeVector({0.0}),
        makeVector({0.0}),
        makeVector({8.0}),
        1,
        options,
        3
    );

    expect(estimate.valid, "strict BDF2 state error is valid");
    expectNear(
        estimate.normalizedError,
        8.0,
        "strict BDF2 state error is normalized"
    );
    expectNear(
        estimate.suggestedScale,
        0.45,
        "strict BDF2 state error uses the cubic-root controller"
    );
}

void testTransientStepController(){
    TransientSolverOptions options = makeAbsoluteOnlyOptions();
    options.minimumStep = 0.1;
    options.maximumRejects = 2;
    options.convergenceFailureScale = 0.25;

    TransientStepControlInput input;
    input.converged = true;
    input.requiresBdf2 = true;
    input.integrationOrder = 2;
    input.errorEstimateValid = true;
    input.normalizedError = 0.25;
    input.suggestedStepScale = 1.5;
    input.currentTime = 1.0;
    input.attemptedStep = 0.5;
    input.hardStepLimit = 1.0;

    TransientStepControlDecision decision = decideTransientStep(
        input,
        options
    );

    expect(
        decision.action == TransientStepAction::Accept,
        "accepted BDF2 attempt is committed"
    );
    expectNear(
        decision.nextStep,
        0.75,
        "accepted BDF2 attempt returns suggested next step"
    );

    input.normalizedError = 4.0;
    input.suggestedStepScale = 0.25;
    decision = decideTransientStep(input, options);

    expect(
        decision.action == TransientStepAction::RetryError,
        "large BDF2 error is retried"
    );
    expectNear(
        decision.nextStep,
        0.125,
        "error retry uses suggested scale"
    );

    input.converged = false;
    decision = decideTransientStep(input, options);

    expect(
        decision.action == TransientStepAction::RetryConvergence,
        "Newton failure is retried"
    );
    expectNear(
        decision.nextStep,
        0.125,
        "Newton failure uses convergence-failure scale"
    );

    input.converged = true;
    input.errorEstimateValid = false;
    decision = decideTransientStep(input, options);

    expect(
        decision.action == TransientStepAction::FailInvalidEstimate,
        "invalid required estimate is never accepted"
    );

    input.errorEstimateValid = true;
    input.integrationOrder = 1;
    decision = decideTransientStep(input, options);

    expect(
        decision.action == TransientStepAction::FailInvalidEstimate,
        "BDF2 history cannot silently fall back to BE"
    );

    input.requiresBdf2 = false;
    input.integrationOrder = 1;
    input.normalizedError = 0.0;
    input.suggestedStepScale = 1.0;
    decision = decideTransientStep(input, options);

    expect(
        decision.action == TransientStepAction::Accept,
        "BE step-doubling result is accepted with a valid estimate"
    );

    input.requiresBdf2 = true;
    input.integrationOrder = 2;
    input.normalizedError = 4.0;
    input.suggestedStepScale = 0.25;
    input.rejectedAttempts = options.maximumRejects;
    decision = decideTransientStep(input, options);

    expect(
        decision.action == TransientStepAction::FailRejectLimit,
        "reject limit prevents an additional retry"
    );

    input.rejectedAttempts = 0;
    input.attemptedStep = 0.11;
    input.suggestedStepScale = 0.2;
    decision = decideTransientStep(input, options);

    expect(
        decision.action == TransientStepAction::RetryError,
        "one final retry at the minimum step is allowed"
    );
    expectNear(
        decision.nextStep,
        0.1,
        "error retry is clamped to the minimum step"
    );

    input.attemptedStep = 0.1;
    decision = decideTransientStep(input, options);

    expect(
        decision.action == TransientStepAction::FailMinimumStep,
        "minimum step cannot be retried indefinitely"
    );
}

void testStepDoublingDifferenceEstimate(){
    const TransientSolverOptions options = makeAbsoluteOnlyOptions();

    const TransientErrorEstimate estimate =
        estimateTransientSolutionDifference(
            makeVector({0.0}),
            makeVector({2.0}),
            makeVector({1.0}),
            1,
            options
        );

    expect(estimate.valid, "step-doubling estimate is valid");
    expectNear(
        estimate.normalizedError,
        1.0,
        "step-doubling compares fine and coarse endpoints"
    );
    expectNear(
        estimate.suggestedScale,
        options.safetyFactor,
        "unit step-doubling error uses safety factor"
    );

    expectDefaultInvalid(
        estimateTransientSolutionDifference(
            makeVector({0.0}),
            makeVector({std::numeric_limits<double>::quiet_NaN()}),
            makeVector({1.0}),
            1,
            options
        ),
        "non-finite fine endpoint invalidates step-doubling estimate"
    );
}

void testVoltageOnlyStepDoublingEstimate(){
    const TransientSolverOptions options = makeAbsoluteOnlyOptions();
    const TransientErrorEstimate estimate =
        estimateTransientSolutionDifference(
            makeVector({0.0, 0.0}),
            makeVector({0.25, 100.0}),
            makeVector({0.0, 0.0}),
            1,
            options,
            false
        );

    expect(
        estimate.valid,
        "voltage-only step-doubling estimate is valid"
    );
    expectNear(
        estimate.normalizedError,
        0.25,
        "voltage-only step-doubling ignores ideal-source branch current jumps"
    );
}

void testRestartForcesBackwardEuler(){
    TransientIntegrator integrator = makeIntegratorWithHistory(
        makeVector({0.0}),
        makeVector({1.0})
    );

    integrator.restartFrom(1.0, makeVector({1.0}));

    expect(
        integrator.coefficients(1.5).order == 1,
        "restart clears history and forces BE"
    );
    expect(
        !integrator.canUseBdf2(1.5),
        "restarted integrator cannot select BDF2"
    );
}

void testAcceptedHistoryRotation(){
    TransientIntegrator integrator;
    integrator.initialize(0.0, makeVector({1.0, 2.0}));
    integrator.accept(1.0, makeVector({3.0, 4.0}));

    expectNear(
        integrator.currentSolution()[0],
        3.0,
        "first accept stores the new current solution"
    );
    expect(
        integrator.olderSolution() != nullptr &&
            integrator.olderSolution()->size() == 2,
        "first accept retains a full-sized older solution"
    );
    expectNear(
        (*integrator.olderSolution())[1],
        2.0,
        "first accept rotates the initial solution into history"
    );

    integrator.accept(2.0, makeVector({5.0, 6.0}));
    expectNear(
        integrator.currentSolution()[1],
        6.0,
        "second accept stores the latest solution"
    );
    expectNear(
        (*integrator.olderSolution())[0],
        3.0,
        "second accept rotates the previous solution into history"
    );
}

void testNewtonStepLimiting(){
    Eigen::VectorXd previous = makeVector({1.0, -2.0});
    Eigen::VectorXd current = makeVector({4.0, -2.5});

    const NewtonStepResult limited = limitNewtonStep(
        current,
        previous,
        1.0
    );
    expect(limited.limited, "large Newton step is limited");
    expectNear(limited.delta, 1.0, "limited Newton step reports bound");
    expectNear(current[0], 2.0, "limited Newton step updates first value");
    expectNear(
        current[1],
        -2.0 - (0.5 / 3.0),
        "limited Newton step preserves update direction"
    );

    current = makeVector({1.2, -2.1});
    const NewtonStepResult unchanged = limitNewtonStep(
        current,
        previous,
        1.0
    );
    expect(!unchanged.limited, "small Newton step is unchanged");
    expectNear(unchanged.delta, 0.2, "small Newton step reports raw delta");

    current = makeVector({std::numeric_limits<double>::quiet_NaN(), -2.0});
    const NewtonStepResult invalid = limitNewtonStep(
        current,
        previous,
        1.0
    );
    expect(!std::isfinite(invalid.delta), "non-finite Newton step is rejected");
}

void testNormalizedNewtonConvergence(){
    const NewtonUpdateEstimate update = estimateNormalizedNewtonUpdate(
        makeVector({2.0002, 2.0e-9}),
        makeVector({2.0, 0.0}),
        1,
        0.0,
        1.0e-3,
        1.0e-9
    );
    expect(update.valid, "normalized Newton update is valid");
    expectNear(
        update.normalizedUpdate,
        2.0,
        "Newton update distinguishes voltage and current absolute tolerances"
    );

    const NewtonResidualEstimate residual = estimateNormalizedNewtonResidual(
        makeVector({1.0e-3, 2.0e-1}),
        makeVector({2.0, 1.0}),
        makeVector({2.0, 0.0}),
        1,
        0.1,
        1.0e-3,
        1.0e-9
    );
    expect(residual.valid, "normalized Newton residual is valid");
    expectNear(
        residual.normalizedResidual,
        2.0e-1 / 1.01e-1,
        "Newton residual applies current tolerance to KCL rows"
    );

    const NewtonUpdateEstimate invalid = estimateNormalizedNewtonUpdate(
        makeVector({1.0}),
        makeVector({1.0, 2.0}),
        1,
        0.0,
        1.0e-6,
        1.0e-9
    );
    expect(!invalid.valid, "Newton update rejects mismatched dimensions");
}

} // namespace

int main(){
    testSolverOptionsValidation();
    testPtaConfigValidation();
    testPstranKvgs0Mapping();
    testPtaSourceRamp();
    testPtaCompoundPseudoElements();
    testFallbackDefersPtaForConvergentOperatingPoint();
    testFallbackActivatesPtaAfterOrdinaryFailure();
    testPtaNormalizedDerivative();
    testPtaNormalizedResidual();
    testPtaNodeCapacitanceGrowth();
    testPtaNodeCapacitanceOscillationAdaptation();
    testPtaMinimumStepCapacitanceRecovery();
    testPtaTrustRegionExhaustionRecovery();
    testLinearSolverDiagnostics();
    testLinearFailureDiagnostics();
    testNewtonRequiresReassembledResidual();
    testNewtonResidualBacktracking();
    testTrustRegionExpandsForAccurateModel();
    testControlledNonMonotoneNewtonFallback();
    testRequiresBdf2History();
    testZeroErrorUsesMaximumScale();
    testVoltageAndCurrentAbsoluteTolerances();
    testRelativeWeight();
    testScaleBoundaries();
    testVariableStepPredictor();
    testMaximumBdf2StepRatio();
    testInvalidInputs();
    testQuadraticSolutionCharacterizesProxy();
    testStrictBdf2LteRejectsInsufficientHistory();
    testStrictBdf2LteIsExactForQuadratic();
    testStrictBdf2LteCubicDefect();
    testStrictBdf2LteUsesThirdOrderControl();
    testTransientStepController();
    testStepDoublingDifferenceEstimate();
    testVoltageOnlyStepDoublingEstimate();
    testRestartForcesBackwardEuler();
    testAcceptedHistoryRotation();
    testNewtonStepLimiting();
    testNormalizedNewtonConvergence();

    if(failureCount != 0){
        std::cerr << failureCount << " of " << checkCount
                  << " transient analysis checks failed\n";
        return 1;
    }

    std::cout << "Transient analysis unit tests: " << checkCount
              << "/" << checkCount << " checks passed\n";
    return 0;
}
