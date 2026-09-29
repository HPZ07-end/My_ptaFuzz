#include "circuit/circuit.hpp"

#include <algorithm>

#include "analysis/pta_analysis.hpp"
#include "circuit/node_map.hpp"
#include "devices/device.hpp"
#include "devices/pseudo_device.hpp"
#include "models/model.hpp"
#include "solver/mna.hpp"

Circuit::Circuit():
    mna_(std::make_unique<MNA>()),
    nodeMap_(std::make_unique<NodeMap>()) {}

Circuit::~Circuit() = default;

const Model* Circuit::addModel(std::unique_ptr<Model> model){
    const std::string name = model->name();
    auto& slot = models_[name];
    slot = std::move(model);
    return slot.get();
}

const Model* Circuit::findModel(const std::string& name) const{
    auto it = models_.find(name);
    return it == models_.end() ? nullptr : it->second.get();
}

PtaDiagnostics Circuit::ptaDiagnostics() const{
    return {
        operatingPointStats_.ptaAttempted,
        operatingPointStats_.converged,
        operatingPointStats_.hasPtaConvergenceMetrics,
        operatingPointStats_.ptaIterations,
        operatingPointStats_.ptaCapacitanceGrowths,
        operatingPointStats_.ptaCapacitanceReductions,
        operatingPointStats_.ptaMinimumStepRecoveries,
        operatingPointStats_.ptaNormalizedDerivative,
        operatingPointStats_.ptaNormalizedDcResidual,
        operatingPointStats_.ptaAttempts
    };
}

const OperatingPointDiagnostics&
Circuit::operatingPointDiagnostics() const noexcept {
    return operatingPointStats_;
}

const TransientDiagnostics& Circuit::transientDiagnostics() const noexcept {
    return transientStats_;
}

CircuitDiagnostics Circuit::circuitDiagnostics() const {
    CircuitDiagnostics diagnostics;
    diagnostics.deviceCount = static_cast<int>(devices_.size());
    diagnostics.modelCount = static_cast<int>(models_.size());
    diagnostics.nodeCount = nodeMap_ ? nodeMap_->nodeCount() : 0;
    diagnostics.unknownCount = mna_ ? mna_->size() : 0;
    diagnostics.matrixNonZeros = mna_ ? mna_->nonZeroCount() : 0;
    diagnostics.pseudoDeviceCount = static_cast<int>(pseudoDevices_.size());

    constexpr int deviceTypeCount = 8;
    int counts[deviceTypeCount] = {};
    for(const auto& device: devices_){
        const int typeIndex = static_cast<int>(device->getType());
        if(typeIndex >= 0 && typeIndex < deviceTypeCount){
            ++counts[typeIndex];
        }
        if(device->isNonlinear()){
            ++diagnostics.nonlinearDeviceCount;
        }
    }
    for(int typeIndex = 0; typeIndex < deviceTypeCount; ++typeIndex){
        if(counts[typeIndex] == 0){
            continue;
        }
        diagnostics.devicesByType.push_back({
            deviceTypeName(static_cast<DeviceType>(typeIndex)),
            counts[typeIndex]
        });
    }

    if(!mna_ || mna_->solution().size() == 0 ||
       !mna_->solution().allFinite()){
        return diagnostics;
    }

    diagnostics.hasFiniteSolution = true;
    const Eigen::VectorXd& solution = mna_->solution();
    diagnostics.maximumAbsoluteSolution = solution.cwiseAbs().maxCoeff();

    if(nodeMap_ && nodeMap_->nodeCount() > 0){
        const auto nodeVoltages = solution.head(nodeMap_->nodeCount());
        diagnostics.minimumNodeVoltage = nodeVoltages.minCoeff();
        diagnostics.maximumNodeVoltage = nodeVoltages.maxCoeff();

        Eigen::Index maximumNode = 0;
        nodeVoltages.cwiseAbs().maxCoeff(&maximumNode);
        diagnostics.maximumAbsoluteSolutionVariable =
            "v(" + nodeMap_->nodeNameByIdx()[
                static_cast<std::size_t>(maximumNode)] + ")";
    }

    for(const auto& device: devices_){
        const int branch = device->branchUnknown();
        if(branch < 0 ||
           static_cast<Eigen::Index>(branch) >= solution.size()){
            continue;
        }
        const double magnitude = std::abs(solution[branch]);
        if(magnitude >= diagnostics.maximumAbsoluteBranchCurrent){
            diagnostics.maximumAbsoluteBranchCurrent = magnitude;
            diagnostics.maximumAbsoluteBranchCurrentDevice = device->getName();
        }
        if(magnitude >= diagnostics.maximumAbsoluteSolution){
            diagnostics.maximumAbsoluteSolution = magnitude;
            diagnostics.maximumAbsoluteSolutionVariable =
                "i(" + device->getName() + ")";
        }
    }
    return diagnostics;
}

int Circuit::allocateUnknown(){
    return nextUnknown_++;
}

bool Circuit::build(const PtaAnalysisConfig& config){
    hasOperatingPointInitialGuess_ = false;
    hasUicInitialSolution_ = false;
    operatingPointInitialGuess_.resize(0);
    uicInitialSolution_.resize(0);
    pendingPtaPlacements_.clear();
    pseudoDevices_.clear();
    ptaNodeCaps_.clear();
    ptaActivated_ = false;

    nodeMap_->build(devices_);

    for(auto& device: devices_){
        device->bindNodes(*nodeMap_);
    }

    cacheOperatingPointDeviceRoles();

    nextUnknown_ = nodeMap_->nodeCount();

    for(auto& device: devices_){
        device->allocateUnknown(*this);
    }
    physicalUnknownCount_ = nextUnknown_;

    const bool forcePta = config.mode == PtaMode::Force;
    if(forcePta){
        collectPendingPtaPlacements(config);
        materializePseudoDevices(config);
    }

    if(!rebuildMna()){
        return false;
    }
    ptaActivated_ = forcePta;
    return true;
}

bool Circuit::rebuildMna(){
    mna_->resize(nextUnknown_);
    mna_->reservePattern(
        devices_.size() * 12 + static_cast<std::size_t>(nextUnknown_)
    );

    for(auto& device: devices_){
        device->pattern(*mna_);
    }
    for(auto& pseudoDevice: pseudoDevices_){
        pseudoDevice->pattern(*mna_);
    }

    mna_->build();

    for(auto& device: devices_){
        device->bindMatrix(*mna_);
    }
    for(auto& pseudoDevice: pseudoDevices_){
        pseudoDevice->bindMatrix(*mna_);
    }
    mna_->releaseBuildMetadata();

    return true;
}

bool Circuit::activatePta(const PtaAnalysisConfig& config){
    if(ptaActivated_){
        return true;
    }
    if(config.mode == PtaMode::Disabled ||
       nextUnknown_ != physicalUnknownCount_ ||
       mna_->size() != physicalUnknownCount_ ||
       mna_->solution().size() != physicalUnknownCount_)
    {
        return false;
    }

    const auto isPhysicalState = [this](const Eigen::VectorXd& state) {
        return state.size() == 0 || state.size() == physicalUnknownCount_;
    };
    if(!isPhysicalState(operatingPointInitialGuess_) ||
       !isPhysicalState(uicInitialSolution_))
    {
        return false;
    }

    const Eigen::VectorXd physicalSolution = mna_->solution();

    nextUnknown_ = physicalUnknownCount_;
    collectPendingPtaPlacements(config);
    materializePseudoDevices(config);

    const auto expandState = [this](const Eigen::VectorXd& physicalState) {
        if(physicalState.size() == 0){
            return Eigen::VectorXd{};
        }

        Eigen::VectorXd expandedState = Eigen::VectorXd::Zero(nextUnknown_);
        expandedState.head(physicalUnknownCount_) = physicalState;
        return expandedState;
    };

    Eigen::VectorXd expandedSolution = expandState(physicalSolution);
    Eigen::VectorXd expandedOperatingPointInitialGuess =
        expandState(operatingPointInitialGuess_);
    Eigen::VectorXd expandedUicInitialSolution =
        expandState(uicInitialSolution_);

    if(!rebuildMna()){
        return false;
    }

    mna_->setSolution(expandedSolution);
    operatingPointInitialGuess_ =
        std::move(expandedOperatingPointInitialGuess);
    uicInitialSolution_ = std::move(expandedUicInitialSolution);
    ptaActivated_ = true;
    return true;
}
