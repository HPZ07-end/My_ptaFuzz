#include "circuit/circuit.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "analysis/analysis_plan.hpp"
#include "circuit/node_map.hpp"
#include "devices/device.hpp"
#include "solver/mna.hpp"

struct CircuitInitialState {
    Eigen::VectorXd solution;
    bool configured = false;
};

class CircuitInitialStateBuilder {
public:
    static CircuitInitialState make(
        const Circuit& circuit,
        const std::vector<NodeVoltageConstraint>& nodeConstraints,
        bool includeDeviceStates
    );
};

namespace {

constexpr double kInitialConditionRelativeTolerance = 64.0e-12;

bool sameInitialValue(double lhs, double rhs){
    return std::abs(lhs - rhs) <= kInitialConditionRelativeTolerance *
        std::max({1.0, std::abs(lhs), std::abs(rhs)});
}

void assignInitialNode(std::vector<std::optional<double>>& values,
                       int node,
                       double value,
                       const std::string& description){
    if(node < 0){
        if(!sameInitialValue(value, 0.0)){
            throw std::invalid_argument(
                "Initial-condition conflict: " + description +
                " requires ground to be " + std::to_string(value) + " V"
            );
        }
        return;
    }

    std::optional<double>& existing = values.at(static_cast<std::size_t>(node));
    if(existing && !sameInitialValue(*existing, value)){
        throw std::invalid_argument(
            "Initial-condition conflict at node " + std::to_string(node) +
            " while applying " + description
        );
    }
    existing = value;
}

void applyVoltageConstraint(std::vector<std::optional<double>>& values,
                            int positive,
                            int negative,
                            double voltage,
                            const std::string& description){
    if(!std::isfinite(voltage)){
        throw std::invalid_argument(
            "Initial-condition voltage must be finite for " + description
        );
    }

    const std::optional<double> positiveValue = positive >= 0
        ? values.at(static_cast<std::size_t>(positive))
        : std::optional<double>(0.0);
    const std::optional<double> negativeValue = negative >= 0
        ? values.at(static_cast<std::size_t>(negative))
        : std::optional<double>(0.0);

    if(positiveValue && negativeValue){
        if(!sameInitialValue(*positiveValue - *negativeValue, voltage)){
            throw std::invalid_argument(
                "Initial-condition conflict for " + description
            );
        }
        return;
    }
    if(positiveValue){
        assignInitialNode(values, negative, *positiveValue - voltage, description);
        return;
    }
    if(negativeValue){
        assignInitialNode(values, positive, *negativeValue + voltage, description);
        return;
    }

    // A floating IC constraint fixes one endpoint to ground as a deterministic
    // gauge choice.  This preserves the requested differential voltage; any
    // later incompatible constraint is diagnosed rather than silently ignored.
    if(positive >= 0){
        assignInitialNode(values, positive, voltage, description);
        assignInitialNode(values, negative, 0.0, description);
    } else {
        assignInitialNode(values, negative, -voltage, description);
    }
}

bool deviceHasInitialState(const Device& device){
    return !device.initialVoltageConstraints().empty() ||
        device.initialBranchCurrent().has_value();
}

}  // namespace

CircuitInitialState CircuitInitialStateBuilder::make(
    const Circuit& circuit,
    const std::vector<NodeVoltageConstraint>& nodeConstraints,
    bool includeDeviceStates
){
    CircuitInitialState state;
    state.solution = Eigen::VectorXd::Zero(circuit.mna_->size());
    std::vector<std::optional<double>> nodeValues(
        static_cast<std::size_t>(circuit.nodeMap_->nodeCount())
    );

    for(const NodeVoltageConstraint& constraint: nodeConstraints){
        const int positive = circuit.nodeMap_->idxOf(constraint.positiveNode);
        const int negative = circuit.nodeMap_->idxOf(constraint.negativeNode);
        applyVoltageConstraint(
            nodeValues,
            positive,
            negative,
            constraint.voltage,
            "V(" + constraint.positiveNode + "," +
                constraint.negativeNode + ")"
        );
        state.configured = true;
    }

    if(includeDeviceStates){
        for(const auto& device: circuit.devices_){
            const std::vector<Device::InitialVoltageConstraint> voltages =
                device->initialVoltageConstraints();
            const auto& nodes = device->getNodeIds();
            for(const auto& constraint: voltages){
                if(constraint.positiveTerminal >= nodes.size() ||
                   constraint.negativeTerminal >= nodes.size()){
                    throw std::logic_error(
                        "Device initial-condition terminal is out of range"
                    );
                }
                applyVoltageConstraint(
                    nodeValues,
                    nodes[constraint.positiveTerminal],
                    nodes[constraint.negativeTerminal],
                    constraint.voltage,
                    device->getName() + " IC"
                );
                state.configured = true;
            }

            const std::optional<double> branchCurrent =
                device->initialBranchCurrent();
            if(branchCurrent){
                const int branch = device->branchUnknown();
                if(branch < 0 || branch >= state.solution.size()){
                    throw std::logic_error(
                        "Device IC current has no MNA branch unknown"
                    );
                }
                if(!std::isfinite(*branchCurrent)){
                    throw std::invalid_argument(
                        "Initial-condition current must be finite for " +
                        device->getName()
                    );
                }
                state.solution[branch] = *branchCurrent;
                state.configured = true;
            }
        }
    }

    for(std::size_t node = 0; node < nodeValues.size(); ++node){
        if(nodeValues[node]){
            state.solution[static_cast<Eigen::Index>(node)] = *nodeValues[node];
        }
    }
    return state;
}

void Circuit::configureInitialConditions(const AnalysisPlan& plan){
    hasOperatingPointInitialGuess_ = false;
    hasUicInitialSolution_ = false;
    operatingPointInitialGuess_.resize(0);
    uicInitialSolution_.resize(0);

    const bool hasDeviceState = std::any_of(
        devices_.begin(), devices_.end(),
        [](const std::unique_ptr<Device>& device){
            return deviceHasInitialState(*device);
        }
    );

    // .ic and device IC values take precedence over .nodeset, matching their
    // role as an explicit state rather than a loose Newton hint.
    if(!plan.initialConditions.empty() || hasDeviceState){
        CircuitInitialState guess = CircuitInitialStateBuilder::make(
            *this,
            plan.initialConditions,
            true
        );
        operatingPointInitialGuess_ = std::move(guess.solution);
        hasOperatingPointInitialGuess_ = guess.configured;
    } else if(!plan.nodeSets.empty()){
        CircuitInitialState guess = CircuitInitialStateBuilder::make(
            *this, plan.nodeSets, false
        );
        operatingPointInitialGuess_ = std::move(guess.solution);
        hasOperatingPointInitialGuess_ = guess.configured;
    }

    CircuitInitialState uic = CircuitInitialStateBuilder::make(
        *this, plan.initialConditions, true
    );
    uicInitialSolution_ = std::move(uic.solution);
    hasUicInitialSolution_ = uic.configured;
}
