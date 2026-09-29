#pragma once

#include <string>
#include <optional>
#include <vector>

#include <Eigen/Core>

enum class DeviceType{
    Resistor,
    Inductor,
    Capacitor,
    VoltageSource,
    CurrentSource,
    Diode,
    BJT,
    MOSFET
};

inline const char* deviceTypeName(DeviceType type){
    switch (type)
    {
        case DeviceType::Resistor:{
            return "Resistor";
        }
        case DeviceType::Inductor:{
            return "Inductor";
        }
        case DeviceType::Capacitor:{
            return "Capacitor";
        }
        case DeviceType::VoltageSource:{
            return "Voltage Source";
        }
        case DeviceType::CurrentSource:{
            return "Current Source";
        }
        case DeviceType::Diode:{
            return "Diode";
        }
        case DeviceType::BJT:{
            return "BJT";
        }
        case DeviceType::MOSFET:{
            return "MOSFET";
        }
        default:{
            return "Unknown";
        }
    }
}


class Circuit;
class MNA;
class NodeMap;
struct TransientStampContext;
struct TransientLteContext;
class Device{
public:
    struct InitialVoltageConstraint {
        std::size_t positiveTerminal = 0;
        std::size_t negativeTerminal = 0;
        double voltage = 0.0;
    };

    Device(std::string n, std::vector<std::string> ns, DeviceType t): name(n), nodes(ns), type(t){}
    virtual ~Device() = default;

    DeviceType getType() const{
        return type;
    }

    const std::vector<std::string>& getNodes() const {
        return nodes;
    }

    const std::vector<int>& getNodeIds() const {
        return nodeIds;
    }

    const std::string& getName() const {
        return name;
    }

    void bindNodes(const NodeMap& nodemap);

    virtual void allocateUnknown(Circuit&) {}

    virtual int branchUnknown() const {
        return -1;
    }

    virtual double nominalSourceValue() const {
        return 0.0;
    }

    virtual bool isNonlinear() const {
        return false;
    }

    virtual bool requiresUicChargeHistoryProtection() const {
        return false;
    }

    virtual void pattern(MNA& mna) = 0;

    virtual void bindMatrix(MNA& mna) = 0;

    virtual void stampOperatingPoint() = 0;

    virtual void stampTransient(const TransientStampContext&){
        //default: 
        stampOperatingPoint();
    }

    virtual void stampTransientLteDefect(
        const TransientLteContext&,
        Eigen::VectorXd&
    ) const {}

    virtual void setOperatingPointSourceScale(double) {}

    virtual void initializeTransientHistory(const Eigen::VectorXd&) {}

    // Initial device states are expressed as terminal-voltage constraints or
    // a current in the device's branch unknown.  Circuit resolves the former
    // together with .ic cards and uses the latter for UIC history.
    virtual std::vector<InitialVoltageConstraint>
    initialVoltageConstraints() const {
        return {};
    }

    virtual std::optional<double> initialBranchCurrent() const {
        return std::nullopt;
    }

    virtual void acceptTransientSolution(const Eigen::VectorXd&,
                                         const Eigen::VectorXd&) {}

    virtual void initializePtaState(double) {}

    virtual void initializePtaMosVgsState(double) {}

    virtual void saveIterationState() {}

    virtual void restoreIterationState() {}

    // A Newton line search needs a nested checkpoint for repeated trial
    // stamps.  Keep it separate from the outer solve checkpoint used by
    // source stepping, PTA retries, and transient step rollback.
    virtual void saveLineSearchState() {}

    virtual void restoreLineSearchState() {}

protected:
    std::string name;
    std::vector<std::string> nodes;
    std::vector<int> nodeIds;
    DeviceType type;
};
