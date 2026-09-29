#include "config/config_loader.hpp"
#include "config/overrides.hpp"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

int checkCount = 0;
int failureCount = 0;

void expect(bool condition, const std::string& description){
    ++checkCount;
    if(condition){
        return;
    }

    ++failureCount;
    std::cerr << "FAIL: " << description << '\n';
}

template<class Callback>
void expectRuntimeError(Callback&& callback, const std::string& description){
    bool threw = false;
    try {
        callback();
    } catch(const std::runtime_error&) {
        threw = true;
    } catch(...) {
    }
    expect(threw, description);
}

class TemporaryDirectory {
public:
    TemporaryDirectory(){
        const std::filesystem::path base =
            std::filesystem::temp_directory_path();

        static unsigned long long counter = 0;
        for(int attempt = 0; attempt < 100; ++attempt){
            const auto ticks = std::chrono::steady_clock::now()
                .time_since_epoch().count();
            const std::filesystem::path candidate = base /
                ("simulator-config-test-" + std::to_string(ticks) +
                 "-" + std::to_string(++counter));
            std::error_code error;
            if(std::filesystem::create_directory(candidate, error)){
                path_ = candidate;
                return;
            }
            if(error){
                throw std::runtime_error(
                    "Cannot create temporary test directory: " +
                    error.message()
                );
            }
        }

        throw std::runtime_error("Cannot allocate temporary test directory");
    }

    ~TemporaryDirectory(){
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const{
        return path_;
    }

private:
    std::filesystem::path path_;
};

void createDirectory(const std::filesystem::path& path){
    std::error_code error;
    if(!std::filesystem::create_directories(path, error) && error){
        throw std::runtime_error(
            "Cannot create test directory <" + path.string() + ">: " +
            error.message()
        );
    }
}

void writeFile(const std::filesystem::path& path, const std::string& contents){
    std::ofstream output(path);
    if(!output){
        throw std::runtime_error(
            "Cannot create test file <" + path.string() + ">"
        );
    }
    output << contents;
    if(!output){
        throw std::runtime_error(
            "Cannot write test file <" + path.string() + ">"
        );
    }
}

simulator::config::LoadedConfig loadedConfigFor(
    const nlohmann::json& document
){
    simulator::config::LoadedConfig loaded;
    loaded.found = true;
    loaded.path = "unit-test-config.json";
    loaded.document = document;
    return loaded;
}

void testAutomaticDiscovery(){
    TemporaryDirectory temporary;
    const std::filesystem::path project = temporary.path() / "project";
    const std::filesystem::path nested = project / "nested";
    createDirectory(nested);
    writeFile(
        project / simulator::config::kDefaultConfigFilename,
        R"({"scope":"project"})"
    );
    writeFile(
        temporary.path() / simulator::config::kDefaultConfigFilename,
        R"({"scope":"parent"})"
    );

    simulator::config::ConfigSearchOptions options;
    const simulator::config::LoadedConfig loaded =
        simulator::config::loadConfig(options, nested);

    expect(loaded.found, "automatic search finds a parent configuration");
    expect(
        loaded.path ==
            project / simulator::config::kDefaultConfigFilename,
        "nearest configuration file wins"
    );
    expect(
        loaded.document.at("scope") == "project",
        "loaded document preserves json values"
    );

    options.parentSearchLimit = 1;
    const auto candidates =
        simulator::config::configSearchCandidates(options, nested);
    expect(
        candidates.size() == 2,
        "search depth includes the working directory and one parent"
    );
    expect(
        candidates.front().parent_path() == nested,
        "first search candidate is in the working directory"
    );
}

void testSearchLimitAndDefaultFallback(){
    TemporaryDirectory temporary;
    const std::filesystem::path nested = temporary.path() / "nested";
    createDirectory(nested);
    writeFile(
        temporary.path() / simulator::config::kDefaultConfigFilename,
        R"({"scope":"parent"})"
    );

    simulator::config::ConfigSearchOptions options;
    options.parentSearchLimit = 0;
    const simulator::config::LoadedConfig loaded =
        simulator::config::loadConfig(options, nested);

    expect(!loaded.found, "zero parent searches does not inspect the parent");
    expect(loaded.path.empty(), "missing automatic configuration has no path");
    expect(
        loaded.document.is_object() && loaded.document.empty(),
        "missing automatic configuration returns an empty json object"
    );
}

void testExplicitConfiguration(){
    TemporaryDirectory temporary;
    const std::filesystem::path configPath = temporary.path() / "custom.json";
    writeFile(configPath, R"({"enabled":true})");

    simulator::config::ConfigSearchOptions options;
    options.explicitPath = configPath;
    const simulator::config::LoadedConfig loaded =
        simulator::config::loadConfig(options, temporary.path());

    expect(loaded.found, "explicit configuration is loaded");
    expect(
        loaded.path == configPath,
        "explicit configuration reports its absolute path"
    );
    expect(
        loaded.document.at("enabled") == true,
        "explicit configuration preserves json boolean values"
    );

    options.explicitPath = temporary.path() / "missing.json";
    expectRuntimeError(
        [&] {
            static_cast<void>(
                simulator::config::loadConfig(options, temporary.path())
            );
        },
        "missing explicit configuration is an error"
    );
}

void testInvalidConfigurationFiles(){
    TemporaryDirectory temporary;
    simulator::config::ConfigSearchOptions options;

    const std::filesystem::path invalidjson =
        temporary.path() / "invalid.json";
    writeFile(invalidjson, "{");
    options.explicitPath = invalidjson;
    expectRuntimeError(
        [&] {
            static_cast<void>(
                simulator::config::loadConfig(options, temporary.path())
            );
        },
        "invalid json is rejected"
    );

    const std::filesystem::path arrayRoot =
        temporary.path() / "array.json";
    writeFile(arrayRoot, "[]");
    options.explicitPath = arrayRoot;
    expectRuntimeError(
        [&] {
            static_cast<void>(
                simulator::config::loadConfig(options, temporary.path())
            );
        },
        "non-object json root is rejected"
    );

    const std::filesystem::path directoryPath =
        temporary.path() / "directory.json";
    createDirectory(directoryPath);
    options.explicitPath = directoryPath;
    expectRuntimeError(
        [&] {
            static_cast<void>(
                simulator::config::loadConfig(options, temporary.path())
            );
        },
        "configuration path must be a regular file"
    );
}

void testConfigOverrideParsing(){
    const simulator::config::ConfigOverrides missing =
        simulator::config::parseConfigOverrides({});
    expect(
        !missing.schemaVersion && !missing.debug && !missing.pta &&
            !missing.transient,
        "missing configuration produces no overrides"
    );

    const auto overrides = simulator::config::parseConfigOverrides(
        loadedConfigFor(nlohmann::json::parse(R"(
            {
                "schema_version": 1,
                "debug": false,
                "op": {
                    "newton": {
                        "maximum_iterations": 41,
                        "tolerance": "2n",
                        "relative_tolerance": 0.002,
                        "voltage_absolute_tolerance": "3u",
                        "current_absolute_tolerance": "4n",
                        "normalized_update_tolerance": 0.8,
                        "normalized_residual_tolerance": 0.9,
                        "maximum_backtracks": 6,
                        "backtrack_scale": 0.4,
                        "sufficient_decrease": 0.002,
                        "maximum_solution_step": 0.25,
                        "maximum_consecutive_non_monotone_steps": 2,
                        "maximum_non_monotone_residual_growth": 3.5,
                        "trust_region_enabled": false,
                        "trust_region_initial_radius": 0.5,
                        "trust_region_minimum_radius": 0.01,
                        "trust_region_maximum_radius": 100.0,
                        "maximum_trust_region_retries": 6,
                        "trust_region_acceptance_ratio": 0.2,
                        "trust_region_shrink_threshold": 0.3,
                        "trust_region_grow_threshold": 0.8,
                        "trust_region_shrink_factor": 0.2,
                        "trust_region_grow_factor": 1.5,
                        "trust_region_boundary_fraction": 0.9
                    },
                    "source_stepping": {
                        "enabled": false,
                        "initial_step": 0.05,
                        "maximum_step": 0.2,
                        "minimum_step": "10u",
                        "growth_factor": 1.25,
                        "failure_scale": 0.4
                    }
                },
                "pta": {
                    "mode": "FoRcE",
                    "newton": {"maximum_iterations": 53},
                    "initial_step": "2.5n",
                    "maximum_steps": 7,
                    "initial_mos_vgs": 1.25,
                    "initial_bjt_vbe": 0.72,
                    "include_diodes": false
                },
                "tran": {
                    "enabled": true,
                    "output_interval": 2e-9,
                    "stop_time": "10n",
                    "use_initial_conditions": false,
                    "solver": {
                        "newton": {"tolerance": "3n"},
                        "relative_tolerance": 0.001,
                        "maximum_rejects": 0
                    }
                }
            }
        )"))
    );

    expect(
        overrides.schemaVersion && *overrides.schemaVersion == 1,
        "schema version is retained"
    );
    expect(
        overrides.debug && !*overrides.debug,
        "debug report boolean override is retained"
    );
    expect(
        overrides.operatingPoint && overrides.operatingPoint->newton &&
            overrides.operatingPoint->newton->maximumIterations &&
            *overrides.operatingPoint->newton->maximumIterations == 41,
        "OP Newton integer override is retained"
    );
    expect(
        overrides.operatingPoint && overrides.operatingPoint->newton &&
            overrides.operatingPoint->newton->relativeTolerance &&
            *overrides.operatingPoint->newton->relativeTolerance == 0.002 &&
            overrides.operatingPoint->newton->voltageAbsoluteTolerance &&
            *overrides.operatingPoint->newton->voltageAbsoluteTolerance ==
                3.0e-6 &&
            overrides.operatingPoint->newton->currentAbsoluteTolerance &&
            *overrides.operatingPoint->newton->currentAbsoluteTolerance ==
                4.0e-9 &&
            overrides.operatingPoint->newton->normalizedUpdateTolerance &&
            *overrides.operatingPoint->newton->normalizedUpdateTolerance ==
                0.8 &&
            overrides.operatingPoint->newton->normalizedResidualTolerance &&
            *overrides.operatingPoint->newton->normalizedResidualTolerance ==
                0.9 &&
            overrides.operatingPoint->newton->maximumBacktracks &&
            *overrides.operatingPoint->newton->maximumBacktracks == 6 &&
            overrides.operatingPoint->newton->backtrackScale &&
            *overrides.operatingPoint->newton->backtrackScale == 0.4 &&
            overrides.operatingPoint->newton->sufficientDecrease &&
            *overrides.operatingPoint->newton->sufficientDecrease == 0.002 &&
            overrides.operatingPoint->newton
                ->maximumConsecutiveNonMonotoneSteps &&
            *overrides.operatingPoint->newton
                ->maximumConsecutiveNonMonotoneSteps == 2 &&
            overrides.operatingPoint->newton
                ->maximumNonMonotoneResidualGrowth &&
            *overrides.operatingPoint->newton
                ->maximumNonMonotoneResidualGrowth == 3.5 &&
            overrides.operatingPoint->newton->trustRegionEnabled &&
            !*overrides.operatingPoint->newton->trustRegionEnabled &&
            overrides.operatingPoint->newton->trustRegionInitialRadius &&
            *overrides.operatingPoint->newton->trustRegionInitialRadius == 0.5 &&
            overrides.operatingPoint->newton->maximumTrustRegionRetries &&
            *overrides.operatingPoint->newton->maximumTrustRegionRetries == 6 &&
            overrides.operatingPoint->newton->trustRegionGrowFactor &&
            *overrides.operatingPoint->newton->trustRegionGrowFactor == 1.5,
        "OP normalized Newton convergence fields are retained"
    );
    expect(
        overrides.operatingPoint &&
            overrides.operatingPoint->sourceStepping &&
            overrides.operatingPoint->sourceStepping->enabled &&
            !*overrides.operatingPoint->sourceStepping->enabled,
        "OP source-stepping boolean override is retained"
    );
    expect(
        overrides.pta && overrides.pta->mode &&
            *overrides.pta->mode == simulator::config::PtaModeOverride::Force,
        "PTA mode is parsed case-insensitively"
    );
    expect(
        overrides.pta && overrides.pta->initialStep &&
            *overrides.pta->initialStep == 2.5e-9,
        "PTA SPICE numeric string is converted"
    );
    expect(
        overrides.pta && overrides.pta->maximumSteps &&
            *overrides.pta->maximumSteps == 7,
        "PTA integer override is retained"
    );
    expect(
        overrides.pta && overrides.pta->newton &&
            overrides.pta->newton->maximumIterations &&
            *overrides.pta->newton->maximumIterations == 53,
        "PTA Newton override is retained"
    );
    expect(
        overrides.pta && overrides.pta->initialMosVgs.specified &&
            overrides.pta->initialMosVgs.value &&
            *overrides.pta->initialMosVgs.value == 1.25,
        "PTA MOS voltage override is retained"
    );
    expect(
        overrides.pta && overrides.pta->initialBjtVbe.specified &&
            overrides.pta->initialBjtVbe.value &&
            *overrides.pta->initialBjtVbe.value == 0.72,
        "PTA BJT voltage override is retained"
    );
    expect(
        overrides.pta && overrides.pta->includeDiodes &&
            !*overrides.pta->includeDiodes,
        "PTA boolean override is retained"
    );
    expect(
        overrides.transient && overrides.transient->enabled &&
            *overrides.transient->enabled &&
            overrides.transient->stopTime &&
            *overrides.transient->stopTime == 1e-8,
        "transient numeric and boolean overrides are retained"
    );
    expect(
        overrides.transient && overrides.transient->solver &&
            overrides.transient->solver->maximumRejects &&
            *overrides.transient->solver->maximumRejects == 0,
        "transient solver integer override is retained"
    );
    expect(
        overrides.transient && overrides.transient->solver &&
            overrides.transient->solver->newton &&
            overrides.transient->solver->newton->tolerance &&
            std::abs(
                *overrides.transient->solver->newton->tolerance - 3e-9
            ) < 1e-20,
        "transient Newton override is retained"
    );

    const auto nullBjtVbe = simulator::config::parseConfigOverrides(
        loadedConfigFor(nlohmann::json::parse(R"(
            {"schema_version": 1, "pta": {"initial_bjt_vbe": null}}
        )"))
    );
    expect(
        nullBjtVbe.pta && nullBjtVbe.pta->initialBjtVbe.specified &&
            !nullBjtVbe.pta->initialBjtVbe.value,
        "null BJT voltage explicitly clears the override"
    );

    const auto nullMosVgs = simulator::config::parseConfigOverrides(
        loadedConfigFor(nlohmann::json::parse(R"(
            {"schema_version": 1, "pta": {"initial_mos_vgs": null}}
        )"))
    );
    expect(
        nullMosVgs.pta && nullMosVgs.pta->initialMosVgs.specified &&
            !nullMosVgs.pta->initialMosVgs.value,
        "null MOS voltage explicitly clears the override"
    );
}

void testConfigOverrideApplication(){
    const auto overrides = simulator::config::parseConfigOverrides(
        loadedConfigFor(nlohmann::json::parse(R"(
            {
                "schema_version": 1,
                "op": {
                    "newton": {
                        "maximum_iterations": 31,
                        "tolerance": "4n",
                        "relative_tolerance": 0.002,
                        "voltage_absolute_tolerance": "3u",
                        "normalized_residual_tolerance": 0.75,
                        "maximum_backtracks": 3,
                        "backtrack_scale": 0.25,
                        "sufficient_decrease": 0.005,
                        "maximum_solution_step": 0.5,
                        "maximum_consecutive_non_monotone_steps": 2,
                        "maximum_non_monotone_residual_growth": 3.5,
                        "trust_region_enabled": false,
                        "trust_region_initial_radius": 0.5,
                        "trust_region_minimum_radius": 0.01,
                        "trust_region_maximum_radius": 100.0,
                        "maximum_trust_region_retries": 6,
                        "trust_region_acceptance_ratio": 0.2,
                        "trust_region_shrink_threshold": 0.3,
                        "trust_region_grow_threshold": 0.8,
                        "trust_region_shrink_factor": 0.2,
                        "trust_region_grow_factor": 1.5,
                        "trust_region_boundary_fraction": 0.9
                    },
                    "source_stepping": {
                        "enabled": false,
                        "failure_scale": 0.4
                    }
                },
                "pta": {
                    "mode": "fallback",
                    "newton": {"maximum_iterations": 47},
                    "initial_mos_vgs": 1.25,
                    "initial_bjt_vbe": null
                },
                "tran": {
                    "enabled": true,
                    "output_interval": "2n",
                    "stop_time": "20n",
                    "maximum_step": "5n",
                    "solver": {
                        "newton": {"maximum_solution_step": 0.2},
                        "maximum_rejects": 4
                    }
                }
            }
        )"))
    );

    OperatingPointSolverOptions operatingPoint;
    PtaAnalysisConfig pta;
    pta.initialMosVgs = 0.65;
    pta.initialBjtVbe = 0.65;
    std::optional<TransientAnalysisConfig> transient;
    transient.emplace();
    transient->outputInterval = 1e-9;
    transient->stopTime = 10e-9;
    transient->maximumStep = 1e-9;

    simulator::config::applyConfigOverrides(
        overrides,
        operatingPoint,
        pta,
        transient,
        false
    );

    expect(
        operatingPoint.newton.maximumIterations == 31 &&
            operatingPoint.newton.tolerance == 4e-9 &&
            operatingPoint.newton.relativeTolerance == 0.002 &&
            operatingPoint.newton.voltageAbsoluteTolerance == 3e-6 &&
            operatingPoint.newton.currentAbsoluteTolerance == 4e-9 &&
            operatingPoint.newton.normalizedResidualTolerance == 0.75 &&
            operatingPoint.newton.maximumBacktracks == 3 &&
            operatingPoint.newton.backtrackScale == 0.25 &&
            operatingPoint.newton.sufficientDecrease == 0.005 &&
            operatingPoint.newton.maximumSolutionStep == 0.5 &&
            operatingPoint.newton.maximumConsecutiveNonMonotoneSteps == 2 &&
            operatingPoint.newton.maximumNonMonotoneResidualGrowth == 3.5 &&
            !operatingPoint.newton.trustRegionEnabled &&
            operatingPoint.newton.trustRegionInitialRadius == 0.5 &&
            operatingPoint.newton.trustRegionMinimumRadius == 0.01 &&
            operatingPoint.newton.trustRegionMaximumRadius == 100.0 &&
            operatingPoint.newton.maximumTrustRegionRetries == 6 &&
            operatingPoint.newton.trustRegionAcceptanceRatio == 0.2 &&
            operatingPoint.newton.trustRegionShrinkThreshold == 0.3 &&
            operatingPoint.newton.trustRegionGrowThreshold == 0.8 &&
            operatingPoint.newton.trustRegionShrinkFactor == 0.2 &&
            operatingPoint.newton.trustRegionGrowFactor == 1.5 &&
            operatingPoint.newton.trustRegionBoundaryFraction == 0.9,
        "OP mixed-unit Newton overrides are applied"
    );
    expect(
        !operatingPoint.sourceStepping.enabled &&
            operatingPoint.sourceStepping.failureScale == 0.4,
        "OP source-stepping overrides are applied"
    );
    expect(
        pta.mode == PtaMode::Fallback &&
            pta.newtonOptions.maximumIterations == 47 &&
            pta.initialMosVgs && *pta.initialMosVgs == 1.25 &&
            !pta.initialBjtVbe,
        "PTA overrides are applied"
    );
    expect(
        transient && transient->outputInterval == 2e-9 &&
            transient->stopTime == 20e-9 &&
            transient->maximumStep && *transient->maximumStep == 5e-9,
        "transient settings are applied when no netlist field is locked"
    );
    expect(
        transient && transient->solverOptions.maximumRejects == 4 &&
            transient->solverOptions.newtonOptions.maximumSolutionStep == 0.2,
        "transient solver overrides are applied"
    );

    const auto invalidPstranMode = simulator::config::parseConfigOverrides(
        loadedConfigFor(nlohmann::json::parse(R"(
            {"schema_version": 1, "pta": {"mode": "disabled"}}
        )"))
    );
    simulator::config::NetlistAnalysisParameterLocks pstranLocks;
    pstranLocks.pta.mode = true;
    pta.mode = PtaMode::Force;
    simulator::config::applyConfigOverrides(
        invalidPstranMode,
        operatingPoint,
        pta,
        transient,
        true,
        pstranLocks
    );
    expect(
        pta.mode == PtaMode::Force,
        ".pstran mode silently takes priority over configuration"
    );

    const auto invalidOperatingPoint = simulator::config::parseConfigOverrides(
        loadedConfigFor(nlohmann::json::parse(R"(
            {
                "schema_version": 1,
                "op": {"newton": {"tolerance": 0}}
            }
        )"))
    );
    OperatingPointSolverOptions invalidOptions;
    PtaAnalysisConfig defaultPta;
    std::optional<TransientAnalysisConfig> noTransient;
    simulator::config::applyConfigOverrides(
        invalidOperatingPoint,
        invalidOptions,
        defaultPta,
        noTransient,
        false
    );
    expect(
        !invalidOptions.valid(),
        "invalid OP numeric range is deferred to runtime validation"
    );
}

void testCommandLineOverrideApplication(){
    OperatingPointSolverOptions operatingPoint;
    PtaAnalysisConfig pta;
    std::optional<TransientAnalysisConfig> transient;
    std::string key;
    std::string error;

    expect(
        simulator::config::applyOperatingPointOption(
            "newton.maximum_iterations=23",
            operatingPoint,
            key,
            error
        ) && key == "newton.maximum-iterations" &&
            operatingPoint.newton.maximumIterations == 23,
        "OP command-line option accepts JSON-style field names"
    );
    expect(
        simulator::config::applyOperatingPointOption(
            "source_stepping.enabled=false",
            operatingPoint,
            key,
            error
        ) && !operatingPoint.sourceStepping.enabled,
        "OP command-line source-stepping override is applied"
    );
    expect(
        simulator::config::applyOperatingPointOption(
            "newton.current_absolute_tolerance=5n",
            operatingPoint,
            key,
            error
        ) && key == "newton.current-absolute-tolerance" &&
            operatingPoint.newton.currentAbsoluteTolerance == 5e-9,
        "OP command-line normalized Newton tolerance is applied"
    );
    expect(
        simulator::config::applyOperatingPointOption(
            "newton.maximum_backtracks=2",
            operatingPoint,
            key,
            error
        ) && key == "newton.maximum-backtracks" &&
            operatingPoint.newton.maximumBacktracks == 2,
        "OP command-line Newton backtracking limit is applied"
    );
    expect(
        simulator::config::applyOperatingPointOption(
            "newton.trust_region_enabled=false",
            operatingPoint,
            key,
            error
        ) && key == "newton.trust-region-enabled" &&
            !operatingPoint.newton.trustRegionEnabled,
        "OP command-line trust-region switch is applied"
    );
    expect(
        simulator::config::applyPtaOption(
            "newton.trust_region_maximum_radius=2.5k",
            pta,
            key,
            error
        ) && pta.newtonOptions.trustRegionMaximumRadius == 2500.0,
        "PTA command-line trust-region bound is applied"
    );
    expect(
        simulator::config::applyPtaOption(
            "compound_time_constant=5n",
            pta,
            key,
            error
        ) && std::abs(pta.compoundTimeConstant - 5e-9) < 1e-20,
        "PTA command-line extended parameter is applied"
    );
    pta.initialBjtVbe = 0.7;
    expect(
        simulator::config::applyPtaOption(
            "initial_bjt_vbe=null",
            pta,
            key,
            error
        ) && !pta.initialBjtVbe,
        "PTA command-line null clears the BJT initial voltage"
    );
    expect(
        simulator::config::applyPtaOption(
            "initial-mos-vgs=1.25",
            pta,
            key,
            error
        ) && pta.initialMosVgs && *pta.initialMosVgs == 1.25,
        "PTA command-line MOS initial voltage is applied"
    );
    expect(
        simulator::config::applyPtaOption(
            "initial_mos_vgs=null",
            pta,
            key,
            error
        ) && !pta.initialMosVgs,
        "PTA command-line null clears the MOS initial voltage"
    );
    expect(
        simulator::config::applyTransientOption(
            "output_interval=2n",
            transient,
            key,
            error
        ) && transient &&
            std::abs(transient->outputInterval - 2e-9) < 1e-20,
        "TRAN command-line option creates a transient configuration"
    );
    expect(
        simulator::config::applyTransientOption(
            "stop-time=20n",
            transient,
            key,
            error
        ) && transient && std::abs(transient->stopTime - 20e-9) < 1e-19,
        "TRAN command-line stop-time override is applied"
    );
    expect(
        simulator::config::applyTransientOption(
            "solver.newton.maximum_trust_region_retries=3",
            transient,
            key,
            error
        ) && transient &&
            transient->solverOptions.newtonOptions
                .maximumTrustRegionRetries == 3,
        "TRAN command-line trust-region retry limit is applied"
    );
    expect(
        simulator::config::applyTransientOption(
            "solver.maximum-rejects=0",
            transient,
            key,
            error
        ) && transient && transient->solverOptions.maximumRejects == 0,
        "TRAN command-line non-negative integer override is applied"
    );
    expect(
        !simulator::config::applyOperatingPointOption(
            "newton.unknown=1",
            operatingPoint,
            key,
            error
        ) && error == "unknown operating-point option",
        "unknown OP command-line option is rejected"
    );

    const auto configurationOverrides =
        simulator::config::parseConfigOverrides(
            loadedConfigFor(nlohmann::json::parse(R"(
                {
                    "schema_version": 1,
                    "op": {"newton": {"maximum_iterations": 3}},
                    "pta": {"initial_step": "1n"},
                    "tran": {
                        "output_interval": "1n",
                        "stop_time": "10n",
                        "maximum_step": "1n"
                    }
                }
            )"))
        );

    OperatingPointSolverOptions precedenceOp;
    PtaAnalysisConfig precedencePta;
    std::optional<TransientAnalysisConfig> precedenceTran;
    simulator::config::applyConfigOverrides(
        configurationOverrides,
        precedenceOp,
        precedencePta,
        precedenceTran,
        false
    );
    const bool opApplied = simulator::config::applyOperatingPointOption(
        "newton.maximum-iterations=29",
        precedenceOp,
        key,
        error
    );
    const bool ptaApplied = simulator::config::applyPtaOption(
        "initial-step=3n",
        precedencePta,
        key,
        error
    );
    const bool tranApplied = simulator::config::applyTransientOption(
        "output-interval=4n",
        precedenceTran,
        key,
        error
    );
    const bool maximumStepApplied =
        simulator::config::applyTransientOption(
            "maximum-step=3n",
            precedenceTran,
            key,
            error,
            std::nullopt,
            5e-9
        );
    expect(
        opApplied && ptaApplied && tranApplied && maximumStepApplied &&
            precedenceOp.newton.maximumIterations == 29 &&
            std::abs(precedencePta.initialStep - 3e-9) < 1e-20 &&
            precedenceTran &&
            std::abs(precedenceTran->outputInterval - 4e-9) < 1e-20 &&
            std::abs(*precedenceTran->maximumStep - 3e-9) < 1e-20,
        "command-line overrides take precedence over configuration overrides"
    );

    std::optional<TransientAnalysisConfig> restoredTransient;
    TransientAnalysisConfig netlistTransient;
    netlistTransient.outputInterval = 1e-9;
    netlistTransient.stopTime = 10e-9;
    const bool restored = simulator::config::applyTransientOption(
        "enabled=true",
        restoredTransient,
        key,
        error,
        netlistTransient
    );
    expect(
        restored && restoredTransient &&
            std::abs(restoredTransient->outputInterval - 1e-9) < 1e-20 &&
            std::abs(restoredTransient->stopTime - 10e-9) < 1e-19,
        "TRAN command-line enable restores the netlist analysis configuration"
    );
}

void testNetlistParameterLocks(){
    const auto overrides = simulator::config::parseConfigOverrides(
        loadedConfigFor(nlohmann::json::parse(R"(
            {
                "schema_version": 1,
                "pta": {
                    "mode": "disabled",
                    "initial_step": "2n",
                    "derivative_tolerance": 0.4,
                    "compound_time_constant": "5n",
                    "initial_mos_vgs": 1.2
                },
                "tran": {
                    "enabled": false,
                    "output_interval": "2n",
                    "stop_time": "20n",
                    "output_start_time": "3n",
                    "maximum_step": "4n",
                    "use_initial_conditions": false,
                    "solver": {"maximum_rejects": 3}
                }
            }
        )"))
    );

    simulator::config::NetlistAnalysisParameterLocks locks;
    locks.pta.mode = true;
    locks.pta.initialStep = true;
    locks.pta.derivativeTolerance = true;
    locks.pta.compoundTimeConstant = true;
    locks.pta.initialMosVgs = true;
    locks.transient.enabled = true;
    locks.transient.outputInterval = true;
    locks.transient.stopTime = true;
    locks.transient.outputStartTime = true;
    locks.transient.maximumStep = true;
    locks.transient.useInitialConditions = true;

    PtaAnalysisConfig pta;
    pta.mode = PtaMode::Force;
    pta.initialStep = 1e-9;
    pta.derivativeTolerance = 0.8;
    pta.compoundTimeConstant = 1e-9;
    pta.initialMosVgs = 0.8;
    std::optional<TransientAnalysisConfig> transient;
    transient.emplace();
    transient->outputInterval = 1e-9;
    transient->stopTime = 10e-9;
    transient->outputStartTime = 2e-9;
    transient->maximumStep = 3e-9;
    transient->useInitialConditions = true;

    OperatingPointSolverOptions operatingPoint;
    simulator::config::applyConfigOverrides(
        overrides,
        operatingPoint,
        pta,
        transient,
        true,
        locks
    );
    expect(
        pta.mode == PtaMode::Force &&
            std::abs(pta.initialStep - 1e-9) < 1e-20 &&
            pta.derivativeTolerance == 0.8 &&
            std::abs(pta.compoundTimeConstant - 1e-9) < 1e-20 &&
            pta.initialMosVgs && *pta.initialMosVgs == 0.8,
        "locked PTA netlist parameters take priority over configuration"
    );
    expect(
        transient && std::abs(transient->outputInterval - 1e-9) < 1e-20 &&
            std::abs(transient->stopTime - 10e-9) < 1e-19 &&
            std::abs(transient->outputStartTime - 2e-9) < 1e-20 &&
            transient->maximumStep &&
            std::abs(*transient->maximumStep - 3e-9) < 1e-20 &&
            transient->useInitialConditions &&
            transient->solverOptions.maximumRejects == 3,
        "locked TRAN card values win while unset solver values are injected"
    );

    std::string key;
    std::string error;
    const bool ptaApplied = simulator::config::applyPtaOption(
        "initial-step=4n",
        pta,
        key,
        error,
        locks.pta
    );
    const bool tranApplied = simulator::config::applyTransientOption(
        "stop-time=40n",
        transient,
        key,
        error,
        std::nullopt,
        std::nullopt,
        locks.transient
    );
    expect(
        ptaApplied && tranApplied &&
            std::abs(pta.initialStep - 1e-9) < 1e-20 && transient &&
            std::abs(transient->stopTime - 10e-9) < 1e-19,
        "locked netlist parameters silently take priority over CLI values"
    );

    AnalysisPlan pstranPlan;
    pstranPlan.pseudoTransient.emplace();
    pstranPlan.pseudoTransient->kvgs0Specified = true;
    pstranPlan.pseudoTransient->kvgs0 = 1.2;
    const auto parsedLocks = simulator::config::parameterLocksFor(pstranPlan);
    expect(
        parsedLocks.pta.initialMosVgs,
        "an explicit .pstran kvgs0 protects the MOS limiter seed"
    );
}

void testInvalidConfigOverrides(){
    const auto expectInvalid = [](
        const char* document,
        const std::string& description
    ){
        expectRuntimeError(
            [&] {
                static_cast<void>(simulator::config::parseConfigOverrides(
                    loadedConfigFor(nlohmann::json::parse(document))
                ));
            },
            description
        );
    };

    expectInvalid("{}", "configuration schema version is required");
    expectInvalid(
        R"({"schema_version": 2})",
        "unsupported configuration schema version is rejected"
    );
    expectInvalid(
        R"({"schema_version": 1, "unknown": true})",
        "unknown top-level configuration field is rejected"
    );
    expectInvalid(
        R"({"schema_version": 1, "debug": "false"})",
        "non-boolean debug flag is rejected"
    );
    expectInvalid(
        R"({"schema_version": 1, "pta": {"maximum_steps": 1.5}})",
        "fractional PTA integer field is rejected"
    );
    expectInvalid(
        R"({"schema_version": 1, "pta": {"include_diodes": "true"}})",
        "non-boolean PTA flag is rejected"
    );
    expectInvalid(
        R"({"schema_version": 1, "tran": {"solver": []}})",
        "non-object transient solver section is rejected"
    );
    expectInvalid(
        R"({"schema_version": 1, "tran": {"maximum_step": "abc"}})",
        "invalid SPICE numeric string is rejected"
    );
    expectInvalid(
        R"({"schema_version": 1, "op": {"newton": {"typo": 1}}})",
        "unknown OP Newton field is rejected"
    );
    expectInvalid(
        R"({"schema_version": 1, "op": {"source_stepping": true}})",
        "non-object OP source-stepping section is rejected"
    );
}

}  // namespace

int main(){
    try {
        testAutomaticDiscovery();
        testSearchLimitAndDefaultFallback();
        testExplicitConfiguration();
        testInvalidConfigurationFiles();
        testConfigOverrideParsing();
        testConfigOverrideApplication();
        testCommandLineOverrideApplication();
        testNetlistParameterLocks();
        testInvalidConfigOverrides();
    } catch(const std::exception& error) {
        std::cerr << "Unexpected test error: " << error.what() << '\n';
        return 1;
    }

    std::cout << "Configuration tests: "
              << (checkCount - failureCount) << '/' << checkCount
              << " checks passed\n";
    return failureCount == 0 ? 0 : 1;
}
