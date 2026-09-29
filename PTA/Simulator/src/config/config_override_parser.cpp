#include "config/overrides.hpp"

// Strictly parse the external JSON configuration into typed overrides.

#include <cmath>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>

#include "config/config_loader.hpp"
#include "netlist/spice_syntax.hpp"

namespace simulator::config {
namespace {
using json = nlohmann::json;

std::string configPathName(const LoadedConfig& loadedConfig){
    return loadedConfig.path.empty() ?
        "config.json" : loadedConfig.path.string();
}

[[noreturn]] void failConfig(
    const LoadedConfig& loadedConfig,
    const std::string& jsonPath,
    const std::string& reason
){
    throw std::runtime_error(
        "Configuration file <" + configPathName(loadedConfig) +
        ">: " + jsonPath + " " + reason
    );
}

std::string childPath(
    const std::string& parentPath,
    const char* fieldName
){
    return parentPath + "." + fieldName;
}

const json* findField(const json& object, const char* fieldName){
    const auto found = object.find(fieldName);
    return found == object.end() ? nullptr : &(*found);
}

const json& requireObject(
    const json& value,
    const LoadedConfig& loadedConfig,
    const std::string& jsonPath
){
    if(!value.is_object()){
        failConfig(loadedConfig, jsonPath, "must be an object");
    }
    return value;
}

void rejectUnknownFields(
    const json& object,
    const LoadedConfig& loadedConfig,
    const std::string& jsonPath,
    std::initializer_list<const char*> allowedFields
){
    for(auto iterator = object.begin(); iterator != object.end(); ++iterator){
        bool allowed = false;

        for(const char* allowedField: allowedFields){
            if(iterator.key() == allowedField){
                allowed = true;
                break;
            }
        }

        if(!allowed){
            failConfig(
                loadedConfig,
                childPath(jsonPath, iterator.key().c_str()),
                "is not supported"
            );
        }
    }
}

double readFiniteNumber(
    const json& value,
    const LoadedConfig& loadedConfig,
    const std::string& jsonPath
){
    double result = 0.0;

    if(value.is_number()){
        try {
            result = value.get<double>();
        } catch(const json::exception&) {
            failConfig(
                loadedConfig,
                jsonPath,
                "must be a finite number or SPICE numeric string"
            );
        }
    } else if(value.is_string()){
        try {
            result = parse_spice_number(value.get<std::string>());
        } catch(const std::runtime_error& error) {
            failConfig(
                loadedConfig,
                jsonPath,
                std::string("contains an invalid SPICE number: ") +
                    error.what()
            );
        }
    } else {
        failConfig(
            loadedConfig,
            jsonPath,
            "must be a number or SPICE numeric string"
        );
    }

    if(!std::isfinite(result)){
        failConfig(
            loadedConfig,
            jsonPath,
            "must be finite"
        );
    }

    return result;
}

int readInteger(
    const json& value,
    const LoadedConfig& loadedConfig,
    const std::string& jsonPath,
    int minimum
){
    if(!value.is_number_integer() && !value.is_number_unsigned()){
        failConfig(
            loadedConfig,
            jsonPath,
            minimum > 0
                ? "must be a positive integer"
                : "must be a non-negative integer"
        );
    }

    if(value.is_number_unsigned()){
        const unsigned long long parsed =
            value.get<unsigned long long>();

        if(parsed > static_cast<unsigned long long>(
            std::numeric_limits<int>::max()))
        {
            failConfig(
                loadedConfig,
                jsonPath,
                "is too large for an integer"
            );
        }

        const int result = static_cast<int>(parsed);
        if(result < minimum){
            failConfig(
                loadedConfig,
                jsonPath,
                minimum > 0
                    ? "must be a positive integer"
                    : "must be a non-negative integer"
            );
        }

        return result;
    }

    const long long parsed = value.get<long long>();
    if(parsed < minimum ||
       parsed > static_cast<long long>(std::numeric_limits<int>::max()))
    {
        failConfig(
            loadedConfig,
            jsonPath,
            minimum > 0
                ? "must be a positive integer"
                : "must be a non-negative integer"
        );
    }

    return static_cast<int>(parsed);
}

bool readBoolean(
    const json& value,
    const LoadedConfig& loadedConfig,
    const std::string& jsonPath
){
    if(!value.is_boolean()){
        failConfig(loadedConfig, jsonPath, "must be a boolean");
    }

    return value.get<bool>();
}

void readOptionalNumber(
    const json& object,
    const char* fieldName,
    const LoadedConfig& loadedConfig,
    const std::string& parentPath,
    std::optional<double>& target
){
    const json* value = findField(object, fieldName);
    if(!value){
        return;
    }

    target = readFiniteNumber(
        *value,
        loadedConfig,
        childPath(parentPath, fieldName)
    );
}

void readOptionalInteger(
    const json& object,
    const char* fieldName,
    const LoadedConfig& loadedConfig,
    const std::string& parentPath,
    int minimum,
    std::optional<int>& target
){
    const json* value = findField(object, fieldName);
    if(!value){
        return;
    }

    target = readInteger(
        *value,
        loadedConfig,
        childPath(parentPath, fieldName),
        minimum
    );
}

void readOptionalBoolean(
    const json& object,
    const char* fieldName,
    const LoadedConfig& loadedConfig,
    const std::string& parentPath,
    std::optional<bool>& target
){
    const json* value = findField(object, fieldName);
    if(!value){
        return;
    }

    target = readBoolean(
        *value,
        loadedConfig,
        childPath(parentPath, fieldName)
    );
}

NewtonOverrides parseNewtonOverrides(
    const json& object,
    const LoadedConfig& loadedConfig,
    const std::string& jsonPath
){
    rejectUnknownFields(
        object,
        loadedConfig,
        jsonPath,
        {
            "maximum_iterations",
            "tolerance",
            "relative_tolerance",
            "voltage_absolute_tolerance",
            "current_absolute_tolerance",
            "normalized_update_tolerance",
            "normalized_residual_tolerance",
            "maximum_backtracks",
            "backtrack_scale",
            "sufficient_decrease",
            "maximum_solution_step",
            "maximum_consecutive_non_monotone_steps",
            "maximum_non_monotone_residual_growth",
            "trust_region_enabled",
            "trust_region_initial_radius",
            "trust_region_minimum_radius",
            "trust_region_maximum_radius",
            "maximum_trust_region_retries",
            "trust_region_acceptance_ratio",
            "trust_region_shrink_threshold",
            "trust_region_grow_threshold",
            "trust_region_shrink_factor",
            "trust_region_grow_factor",
            "trust_region_boundary_fraction"
        }
    );

    NewtonOverrides result;
    readOptionalInteger(
        object,
        "maximum_iterations",
        loadedConfig,
        jsonPath,
        1,
        result.maximumIterations
    );
    readOptionalNumber(
        object,
        "tolerance",
        loadedConfig,
        jsonPath,
        result.tolerance
    );
    readOptionalNumber(
        object,
        "relative_tolerance",
        loadedConfig,
        jsonPath,
        result.relativeTolerance
    );
    readOptionalNumber(
        object,
        "voltage_absolute_tolerance",
        loadedConfig,
        jsonPath,
        result.voltageAbsoluteTolerance
    );
    readOptionalNumber(
        object,
        "current_absolute_tolerance",
        loadedConfig,
        jsonPath,
        result.currentAbsoluteTolerance
    );
    readOptionalNumber(
        object,
        "normalized_update_tolerance",
        loadedConfig,
        jsonPath,
        result.normalizedUpdateTolerance
    );
    readOptionalNumber(
        object,
        "normalized_residual_tolerance",
        loadedConfig,
        jsonPath,
        result.normalizedResidualTolerance
    );
    readOptionalInteger(
        object,
        "maximum_backtracks",
        loadedConfig,
        jsonPath,
        0,
        result.maximumBacktracks
    );
    readOptionalNumber(
        object,
        "backtrack_scale",
        loadedConfig,
        jsonPath,
        result.backtrackScale
    );
    readOptionalNumber(
        object,
        "sufficient_decrease",
        loadedConfig,
        jsonPath,
        result.sufficientDecrease
    );
    readOptionalNumber(
        object,
        "maximum_solution_step",
        loadedConfig,
        jsonPath,
        result.maximumSolutionStep
    );
    readOptionalInteger(
        object,
        "maximum_consecutive_non_monotone_steps",
        loadedConfig,
        jsonPath,
        0,
        result.maximumConsecutiveNonMonotoneSteps
    );
    readOptionalNumber(
        object,
        "maximum_non_monotone_residual_growth",
        loadedConfig,
        jsonPath,
        result.maximumNonMonotoneResidualGrowth
    );
    readOptionalBoolean(
        object,
        "trust_region_enabled",
        loadedConfig,
        jsonPath,
        result.trustRegionEnabled
    );
    readOptionalNumber(
        object,
        "trust_region_initial_radius",
        loadedConfig,
        jsonPath,
        result.trustRegionInitialRadius
    );
    readOptionalNumber(
        object,
        "trust_region_minimum_radius",
        loadedConfig,
        jsonPath,
        result.trustRegionMinimumRadius
    );
    readOptionalNumber(
        object,
        "trust_region_maximum_radius",
        loadedConfig,
        jsonPath,
        result.trustRegionMaximumRadius
    );
    readOptionalInteger(
        object,
        "maximum_trust_region_retries",
        loadedConfig,
        jsonPath,
        0,
        result.maximumTrustRegionRetries
    );
    readOptionalNumber(
        object,
        "trust_region_acceptance_ratio",
        loadedConfig,
        jsonPath,
        result.trustRegionAcceptanceRatio
    );
    readOptionalNumber(
        object,
        "trust_region_shrink_threshold",
        loadedConfig,
        jsonPath,
        result.trustRegionShrinkThreshold
    );
    readOptionalNumber(
        object,
        "trust_region_grow_threshold",
        loadedConfig,
        jsonPath,
        result.trustRegionGrowThreshold
    );
    readOptionalNumber(
        object,
        "trust_region_shrink_factor",
        loadedConfig,
        jsonPath,
        result.trustRegionShrinkFactor
    );
    readOptionalNumber(
        object,
        "trust_region_grow_factor",
        loadedConfig,
        jsonPath,
        result.trustRegionGrowFactor
    );
    readOptionalNumber(
        object,
        "trust_region_boundary_fraction",
        loadedConfig,
        jsonPath,
        result.trustRegionBoundaryFraction
    );
    return result;
}

SourceSteppingOverrides parseSourceSteppingOverrides(
    const json& object,
    const LoadedConfig& loadedConfig,
    const std::string& jsonPath
){
    rejectUnknownFields(
        object,
        loadedConfig,
        jsonPath,
        {
            "enabled",
            "initial_step",
            "maximum_step",
            "minimum_step",
            "growth_factor",
            "failure_scale"
        }
    );

    SourceSteppingOverrides result;
    readOptionalBoolean(
        object,
        "enabled",
        loadedConfig,
        jsonPath,
        result.enabled
    );
    readOptionalNumber(
        object,
        "initial_step",
        loadedConfig,
        jsonPath,
        result.initialStep
    );
    readOptionalNumber(
        object,
        "maximum_step",
        loadedConfig,
        jsonPath,
        result.maximumStep
    );
    readOptionalNumber(
        object,
        "minimum_step",
        loadedConfig,
        jsonPath,
        result.minimumStep
    );
    readOptionalNumber(
        object,
        "growth_factor",
        loadedConfig,
        jsonPath,
        result.growthFactor
    );
    readOptionalNumber(
        object,
        "failure_scale",
        loadedConfig,
        jsonPath,
        result.failureScale
    );
    return result;
}

OperatingPointOverrides parseOperatingPointOverrides(
    const json& object,
    const LoadedConfig& loadedConfig
){
    rejectUnknownFields(
        object,
        loadedConfig,
        "$.op",
        {
            "newton",
            "source_stepping"
        }
    );

    OperatingPointOverrides result;

    const json* newton = findField(object, "newton");
    if(newton){
        result.newton = parseNewtonOverrides(
            requireObject(*newton, loadedConfig, "$.op.newton"),
            loadedConfig,
            "$.op.newton"
        );
    }

    const json* sourceStepping = findField(object, "source_stepping");
    if(sourceStepping){
        result.sourceStepping = parseSourceSteppingOverrides(
            requireObject(
                *sourceStepping,
                loadedConfig,
                "$.op.source_stepping"
            ),
            loadedConfig,
            "$.op.source_stepping"
        );
    }

    return result;
}

void readOptionalPtaMode(
    const json& object,
    const LoadedConfig& loadedConfig,
    PtaOverrides& target
){
    const json* value = findField(object, "mode");
    if(!value){
        return;
    }

    if(!value->is_string()){
        failConfig(loadedConfig, "$.pta.mode", "must be a string");
    }

    const std::string mode = to_lower_copy(value->get<std::string>());
    if(mode == "disabled"){
        target.mode = PtaModeOverride::Disabled;
        return;
    }
    if(mode == "force"){
        target.mode = PtaModeOverride::Force;
        return;
    }
    if(mode == "fallback"){
        target.mode = PtaModeOverride::Fallback;
        return;
    }

    failConfig(
        loadedConfig,
        "$.pta.mode",
        "must be disabled, force, or fallback"
    );
}

void readNullableBjtVbe(
    const json& object,
    const LoadedConfig& loadedConfig,
    PtaOverrides& target
){
    const json* value = findField(object, "initial_bjt_vbe");
    if(!value){
        return;
    }

    target.initialBjtVbe.specified = true;
    if(value->is_null()){
        target.initialBjtVbe.value.reset();
        return;
    }

    target.initialBjtVbe.value = readFiniteNumber(
        *value,
        loadedConfig,
        "$.pta.initial_bjt_vbe"
    );
}

void readNullableMosVgs(
    const json& object,
    const LoadedConfig& loadedConfig,
    PtaOverrides& target
){
    const json* value = findField(object, "initial_mos_vgs");
    if(!value){
        return;
    }

    target.initialMosVgs.specified = true;
    if(value->is_null()){
        target.initialMosVgs.value.reset();
        return;
    }

    target.initialMosVgs.value = readFiniteNumber(
        *value,
        loadedConfig,
        "$.pta.initial_mos_vgs"
    );
}

PtaOverrides parsePtaOverrides(
    const json& object,
    const LoadedConfig& loadedConfig
){
    rejectUnknownFields(
        object,
        loadedConfig,
        "$.pta",
        {
            "mode",
            "newton",
            "initial_step",
            "minimum_step",
            "maximum_step",
            "maximum_steps",
            "derivative_tolerance",
            "derivative_relative_tolerance",
            "derivative_voltage_absolute_tolerance",
            "derivative_current_absolute_tolerance",
            "dc_residual_tolerance",
            "dc_residual_relative_tolerance",
            "dc_voltage_absolute_tolerance",
            "dc_current_absolute_tolerance",
            "initial_node_capacitance",
            "minimum_node_capacitance",
            "maximum_node_capacitance",
            "current_source_capacitance",
            "voltage_source_inductance",
            "compound_time_constant",
            "compound_initial_resistance",
            "compound_initial_conductance",
            "source_ramp_time",
            "initial_mos_vgs",
            "initial_bjt_vbe",
            "failed_step_scale",
            "successful_step_scale",
            "capacitance_grow_scale",
            "small_oscillation_scale",
            "medium_oscillation_scale",
            "heavy_oscillation_scale",
            "medium_oscillation_ratio",
            "heavy_oscillation_ratio",
            "include_mos_bulk",
            "include_diodes"
        }
    );

    PtaOverrides result;
    readOptionalPtaMode(object, loadedConfig, result);

    const json* newton = findField(object, "newton");
    if(newton){
        result.newton = parseNewtonOverrides(
            requireObject(*newton, loadedConfig, "$.pta.newton"),
            loadedConfig,
            "$.pta.newton"
        );
    }

    readOptionalNumber(object, "initial_step", loadedConfig,
                       "$.pta", result.initialStep);
    readOptionalNumber(object, "minimum_step", loadedConfig,
                       "$.pta", result.minimumStep);
    readOptionalNumber(object, "maximum_step", loadedConfig,
                       "$.pta", result.maximumStep);
    readOptionalInteger(object, "maximum_steps", loadedConfig,
                        "$.pta", 1, result.maximumSteps);

    readOptionalNumber(object, "derivative_tolerance", loadedConfig,
                       "$.pta", result.derivativeTolerance);
    readOptionalNumber(object, "derivative_relative_tolerance", loadedConfig,
                       "$.pta", result.derivativeRelativeTolerance);
    readOptionalNumber(object, "derivative_voltage_absolute_tolerance",
                       loadedConfig, "$.pta",
                       result.derivativeVoltageAbsoluteTolerance);
    readOptionalNumber(object, "derivative_current_absolute_tolerance",
                       loadedConfig, "$.pta",
                       result.derivativeCurrentAbsoluteTolerance);

    readOptionalNumber(object, "dc_residual_tolerance", loadedConfig,
                       "$.pta", result.dcResidualTolerance);
    readOptionalNumber(object, "dc_residual_relative_tolerance",
                       loadedConfig, "$.pta",
                       result.dcResidualRelativeTolerance);
    readOptionalNumber(object, "dc_voltage_absolute_tolerance",
                       loadedConfig, "$.pta",
                       result.dcVoltageAbsoluteTolerance);
    readOptionalNumber(object, "dc_current_absolute_tolerance",
                       loadedConfig, "$.pta",
                       result.dcCurrentAbsoluteTolerance);

    readOptionalNumber(object, "initial_node_capacitance",
                       loadedConfig, "$.pta",
                       result.initialNodeCapacitance);
    readOptionalNumber(object, "minimum_node_capacitance",
                       loadedConfig, "$.pta",
                       result.minimumNodeCapacitance);
    readOptionalNumber(object, "maximum_node_capacitance",
                       loadedConfig, "$.pta",
                       result.maximumNodeCapacitance);
    readOptionalNumber(object, "current_source_capacitance",
                       loadedConfig, "$.pta",
                       result.currentSourceCapacitance);
    readOptionalNumber(object, "voltage_source_inductance",
                       loadedConfig, "$.pta",
                       result.voltageSourceInductance);

    readOptionalNumber(object, "compound_time_constant",
                       loadedConfig, "$.pta",
                       result.compoundTimeConstant);
    readOptionalNumber(object, "compound_initial_resistance",
                       loadedConfig, "$.pta",
                       result.compoundInitialResistance);
    readOptionalNumber(object, "compound_initial_conductance",
                       loadedConfig, "$.pta",
                       result.compoundInitialConductance);
    readOptionalNumber(object, "source_ramp_time",
                       loadedConfig, "$.pta",
                       result.sourceRampTime);
    readNullableMosVgs(object, loadedConfig, result);
    readNullableBjtVbe(object, loadedConfig, result);

    readOptionalNumber(object, "failed_step_scale",
                       loadedConfig, "$.pta",
                       result.failedStepScale);
    readOptionalNumber(object, "successful_step_scale",
                       loadedConfig, "$.pta",
                       result.successfulStepScale);
    readOptionalNumber(object, "capacitance_grow_scale",
                       loadedConfig, "$.pta",
                       result.capacitanceGrowScale);
    readOptionalNumber(object, "small_oscillation_scale",
                       loadedConfig, "$.pta",
                       result.smallOscillationScale);
    readOptionalNumber(object, "medium_oscillation_scale",
                       loadedConfig, "$.pta",
                       result.mediumOscillationScale);
    readOptionalNumber(object, "heavy_oscillation_scale",
                       loadedConfig, "$.pta",
                       result.heavyOscillationScale);
    readOptionalNumber(object, "medium_oscillation_ratio",
                       loadedConfig, "$.pta",
                       result.mediumOscillationRatio);
    readOptionalNumber(object, "heavy_oscillation_ratio",
                       loadedConfig, "$.pta",
                       result.heavyOscillationRatio);

    readOptionalBoolean(object, "include_mos_bulk",
                        loadedConfig, "$.pta",
                        result.includeMosBulk);
    readOptionalBoolean(object, "include_diodes",
                        loadedConfig, "$.pta",
                        result.includeDiodes);

    return result;
}

TransientSolverOverrides parseTransientSolverOverrides(
    const json& object,
    const LoadedConfig& loadedConfig
){
    rejectUnknownFields(
        object,
        loadedConfig,
        "$.tran.solver",
        {
            "newton",
            "relative_tolerance",
            "voltage_absolute_tolerance",
            "current_absolute_tolerance",
            "minimum_step",
            "safety_factor",
            "minimum_scale",
            "maximum_scale",
            "convergence_failure_scale",
            "maximum_rejects"
        }
    );

    TransientSolverOverrides result;

    const json* newton = findField(object, "newton");
    if(newton){
        result.newton = parseNewtonOverrides(
            requireObject(*newton, loadedConfig, "$.tran.solver.newton"),
            loadedConfig,
            "$.tran.solver.newton"
        );
    }

    readOptionalNumber(object, "relative_tolerance",
                       loadedConfig, "$.tran.solver",
                       result.relativeTolerance);
    readOptionalNumber(object, "voltage_absolute_tolerance",
                       loadedConfig, "$.tran.solver",
                       result.voltageAbsoluteTolerance);
    readOptionalNumber(object, "current_absolute_tolerance",
                       loadedConfig, "$.tran.solver",
                       result.currentAbsoluteTolerance);
    readOptionalNumber(object, "minimum_step",
                       loadedConfig, "$.tran.solver",
                       result.minimumStep);

    readOptionalNumber(object, "safety_factor",
                       loadedConfig, "$.tran.solver",
                       result.safetyFactor);
    readOptionalNumber(object, "minimum_scale",
                       loadedConfig, "$.tran.solver",
                       result.minimumScale);
    readOptionalNumber(object, "maximum_scale",
                       loadedConfig, "$.tran.solver",
                       result.maximumScale);
    readOptionalNumber(object, "convergence_failure_scale",
                       loadedConfig, "$.tran.solver",
                       result.convergenceFailureScale);
    readOptionalInteger(object, "maximum_rejects",
                        loadedConfig, "$.tran.solver",
                        0, result.maximumRejects);

    return result;
}

TransientOverrides parseTransientOverrides(
    const json& object,
    const LoadedConfig& loadedConfig
){
    rejectUnknownFields(
        object,
        loadedConfig,
        "$.tran",
        {
            "enabled",
            "output_interval",
            "stop_time",
            "output_start_time",
            "maximum_step",
            "use_initial_conditions",
            "solver"
        }
    );

    TransientOverrides result;

    readOptionalBoolean(object, "enabled",
                        loadedConfig, "$.tran",
                        result.enabled);
    readOptionalNumber(object, "output_interval",
                       loadedConfig, "$.tran",
                       result.outputInterval);
    readOptionalNumber(object, "stop_time",
                       loadedConfig, "$.tran",
                       result.stopTime);
    readOptionalNumber(object, "output_start_time",
                       loadedConfig, "$.tran",
                       result.outputStartTime);
    readOptionalNumber(object, "maximum_step",
                       loadedConfig, "$.tran",
                       result.maximumStep);
    readOptionalBoolean(object, "use_initial_conditions",
                        loadedConfig, "$.tran",
                        result.useInitialConditions);

    const json* solver = findField(object, "solver");
    if(solver){
        result.solver = parseTransientSolverOverrides(
            requireObject(*solver, loadedConfig, "$.tran.solver"),
            loadedConfig
        );
    }

    return result;
}

}  // namespace

ConfigOverrides parseConfigOverrides(
    const LoadedConfig& loadedConfig
){
    if(!loadedConfig.found){
        return {};
    }

    const json& root = requireObject(
        loadedConfig.document,
        loadedConfig,
        "$"
    );

    rejectUnknownFields(
        root,
        loadedConfig,
        "$",
        {
            "schema_version",
            "debug",
            "op",
            "pta",
            "tran"
        }
    );

    const json* schemaVersion = findField(root, "schema_version");
    if(!schemaVersion){
        failConfig(
            loadedConfig,
            "$.schema_version",
            "is required"
        );
    }

    ConfigOverrides result;
    result.schemaVersion = readInteger(
        *schemaVersion,
        loadedConfig,
        "$.schema_version",
        1
    );

    if(*result.schemaVersion != 1){
        failConfig(
            loadedConfig,
            "$.schema_version",
            "must be 1"
        );
    }

    readOptionalBoolean(root, "debug", loadedConfig, "$", result.debug);

    const json* operatingPoint = findField(root, "op");
    if(operatingPoint){
        result.operatingPoint = parseOperatingPointOverrides(
            requireObject(*operatingPoint, loadedConfig, "$.op"),
            loadedConfig
        );
    }

    const json* pta = findField(root, "pta");
    if(pta){
        result.pta = parsePtaOverrides(
            requireObject(*pta, loadedConfig, "$.pta"),
            loadedConfig
        );
    }

    const json* tran = findField(root, "tran");
    if(tran){
        result.transient = parseTransientOverrides(
            requireObject(*tran, loadedConfig, "$.tran"),
            loadedConfig
        );
    }

    return result;
}

}  // namespace simulator::config
