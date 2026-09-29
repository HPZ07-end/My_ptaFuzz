#pragma once
#include <Eigen/Core>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "analysis/solver_diagnostics.hpp"

class Device;
class PseudoDevice;
class PseudoCapacitor;
class MNA;
class Model;
class NodeMap;
class SpiceOutputAccess;
class CircuitPtaTestAccess;
class PtaTraceWriter;
class TransientIntegrator;
class CircuitInitialStateBuilder;

struct TransientAnalysisConfig;
struct TransientErrorEstimate;
struct TransientStampContext;
struct TransientSolverOptions;
struct PendingPtaPlacement;
struct PtaAnalysisConfig;
struct PtaDiagnostics;
struct NewtonSolverOptions;
struct OperatingPointSolverOptions;
struct AnalysisPlan;
class Circuit{
public:
    Circuit();
    ~Circuit();

    template<class T, class... Args>
    void addDevice(Args&&... args){
        devices_.emplace_back(
            std::make_unique<T>(
                std::forward<Args>(args)...
            )
        );
    }

    const Model* addModel(std::unique_ptr<Model> model);

    const Model* findModel(const std::string& name) const;

    bool build(const PtaAnalysisConfig& config);

    // Must be called after build(), when netlist node names and MNA unknowns
    // are available.  It prepares distinct OP-guess and UIC state vectors.
    void configureInitialConditions(const AnalysisPlan& plan);

    int allocateUnknown();

    bool solve(); // Backward-compatible OP entry point.

    bool solveOperatingPoint();

    bool solveOperatingPoint(
        const OperatingPointSolverOptions& options
    );

    bool solveTransient(const TransientAnalysisConfig& config);

    bool solveTransient(
        const TransientAnalysisConfig& config,
        const OperatingPointSolverOptions& operatingPointOptions
    );

    bool solveAdaptivePta(const PtaAnalysisConfig& config);

    PtaDiagnostics ptaDiagnostics() const;

    const OperatingPointDiagnostics& operatingPointDiagnostics() const noexcept;

    const TransientDiagnostics& transientDiagnostics() const noexcept;

    CircuitDiagnostics circuitDiagnostics() const;

private:
    friend class SpiceOutputAccess;
    friend class CircuitPtaTestAccess;
    friend class PtaTraceWriter;
    friend class CircuitInitialStateBuilder;

    using AssembleCallback = std::function<void()>;

    struct TransientStepAttempt{
        int integrationOrder = 1;
        bool errorEstimateValid = false;
        double normalizedError = 0.0;
        double suggestedStepScale = 1.0;
        bool converged = false;
        bool transientHistoryAdvanced = false;
        NewtonSolveDiagnostics newtonStats;
        Eigen::VectorXd solution;
    };

    struct TransientSample {
        double time = 0.0;
        Eigen::VectorXd solution;
    };

    struct PtaNodeCapState{
        int node = -1;
        PseudoCapacitor* capacitor = nullptr;
        double capacitance = 0.0;
        double previousDelta = 0.0;
        bool hasPreviousDelta = false;
    };

    struct PtaCapacitanceReductionSummary {
        int total = 0;
        int smallOscillation = 0;
        int mediumOscillation = 0;
        int heavyOscillation = 0;
    };

    TransientStepAttempt tryTransientStep(
        const TransientIntegrator& integrator,
        double targetTime,
        const TransientSolverOptions& options
    );

    TransientErrorEstimate estimateStrictTransientLte(
        const TransientIntegrator& integrator,
        double targetTime,
        const Eigen::VectorXd& correctedSolution,
        const TransientSolverOptions& options
    );

    void addTransientStats(const NewtonSolveDiagnostics& stats);

    void restoreTransientCheckpoint(
        const Eigen::VectorXd& acceptedSolution
    );

    void advanceTransientHistory(const Eigen::VectorXd& previous,
                                 const Eigen::VectorXd& accepted);

    bool solveNewtonSystem(const AssembleCallback& assemble,
                           NewtonSolveDiagnostics& stats,
                           const NewtonSolverOptions& options);

    bool solveLinearSystem(const AssembleCallback& assemble,
                           NewtonSolveDiagnostics& stats);

    bool solveOperatingPointWithSourceStepping(
        const AssembleCallback& assemble,
        const OperatingPointSolverOptions& options
    );

    void addNewtonStats(const NewtonSolveDiagnostics& stats);

    void cacheOperatingPointDeviceRoles();

    void assembleOperatingPointSystem();

    void assembleTransientSystem(const TransientStampContext& ctx);

    void assemblePtaSystem(const TransientStampContext& ctx);

    bool hasNonlinearDevices() const;

    void setOperatingPointSourceScale(double scale);

    void initializePtaStates(const PtaAnalysisConfig& config, double time);

    void saveNonlinearIterationStates();

    void restoreNonlinearIterationStates();

    void saveNonlinearLineSearchStates();

    void restoreNonlinearLineSearchStates();

    void recordTransientSample(double time);

    void collectPendingPtaPlacements(const PtaAnalysisConfig& config);

    void materializePseudoDevices(const PtaAnalysisConfig& config);

    bool rebuildMna();

    bool activatePta(const PtaAnalysisConfig& config);

    std::optional<PtaNodeCapacitanceGrowth>
    growPtaNodeCapacitanceForResidual(
        const Eigen::VectorXd& residual,
        const PtaAnalysisConfig& config
    );

    PtaCapacitanceReductionSummary updatePtaNodeCapacitancesAfterAcceptedStep(
        const Eigen::VectorXd& currentSolution,
        const Eigen::VectorXd& previousSolution,
        const PtaAnalysisConfig& config
    );

    std::unique_ptr<MNA> mna_;

    std::vector<std::unique_ptr<Device>> devices_;

    std::vector<Device*> sourceSteppingDevices_;

    std::vector<Device*> iterationStateDevices_;

    std::vector<std::unique_ptr<PseudoDevice>> pseudoDevices_;

    std::unordered_map<std::string, std::unique_ptr<Model>> models_;

    int nextUnknown_ = 0;

    int physicalUnknownCount_ = 0;

    bool ptaActivated_ = false;

    bool hasNonlinearDevices_ = false;

    double operatingPointSourceScale_ = 1.0;

    std::unique_ptr<NodeMap> nodeMap_;

    OperatingPointDiagnostics operatingPointStats_;

    TransientDiagnostics transientStats_;

    std::vector<TransientSample> transientSamples_;

    std::vector<PendingPtaPlacement> pendingPtaPlacements_;

    std::vector<PtaNodeCapState> ptaNodeCaps_;

    Eigen::VectorXd operatingPointInitialGuess_;
    Eigen::VectorXd uicInitialSolution_;
    bool hasOperatingPointInitialGuess_ = false;
    bool hasUicInitialSolution_ = false;
};
