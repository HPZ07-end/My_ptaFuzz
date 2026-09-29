#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>

#include "app/command_line.hpp"
#include "circuit/circuit.hpp"
#include "config/config_loader.hpp"
#include "config/overrides.hpp"
#include "io/output_files.hpp"
#include "io/pta_trace.hpp"
#include "io/solver_report.hpp"
#include "io/spice_output.hpp"
#include "netlist/parser.hpp"

namespace {

class TeeStreamBuffer final: public std::streambuf {
public:
    TeeStreamBuffer(std::streambuf* first, std::streambuf* second):
        first_(first), second_(second) {}

protected:
    int overflow(int character) override {
        if(character == traits_type::eof()){
            return traits_type::not_eof(character);
        }
        const char value = static_cast<char>(character);
        const bool firstOk = first_->sputc(value) != traits_type::eof();
        const bool secondOk = second_->sputc(value) != traits_type::eof();
        return firstOk && secondOk ? character : traits_type::eof();
    }

    std::streamsize xsputn(const char* data, std::streamsize size) override {
        const std::streamsize firstWritten = first_->sputn(data, size);
        const std::streamsize secondWritten = second_->sputn(data, size);
        return std::min(firstWritten, secondWritten);
    }

    int sync() override {
        return first_->pubsync() == 0 && second_->pubsync() == 0 ? 0 : -1;
    }

private:
    std::streambuf* first_;
    std::streambuf* second_;
};

class ErrorCapture {
public:
    explicit ErrorCapture(std::ostream& stream):
        stream_(stream),
        original_(stream.rdbuf()),
        tee_(original_, captured_.rdbuf())
    {
        stream_.rdbuf(&tee_);
    }

    ~ErrorCapture(){
        stop();
    }

    void stop(){
        if(active_){
            stream_.flush();
            stream_.rdbuf(original_);
            active_ = false;
        }
    }

    std::string str() const {
        return captured_.str();
    }

private:
    std::ostream& stream_;
    std::streambuf* original_;
    std::ostringstream captured_;
    TeeStreamBuffer tee_;
    bool active_ = true;
};

double elapsedSeconds(std::chrono::steady_clock::time_point start){
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start
    ).count();
}

std::string configurationSource(
    const simulator::config::LoadedConfig& config
){
    return config.found ? config.path.string() : "built-in defaults";
}

}  // namespace

int main(int argc, char* argv[]){
    simulator::app::CommandLineOptions options;
    if(!simulator::app::parseCommandLine(
           argc,
           argv,
           options,
           std::cout,
           std::cerr)){
        if(argc <= 1){
            simulator::app::printUsage(std::cerr, argv[0]);
        }
        return options.helpRequested ? 0 : 2;
    }
    if(!SpiceOutputFiles::validatePaths(
           options.inputPath,
           options.listingPath,
           options.rawPath,
           std::cerr)){
        return 2;
    }

    std::optional<OutputBundlePaths> outputPaths;
    std::unique_ptr<ErrorCapture> errorCapture;
    const auto simulationStart = std::chrono::steady_clock::now();
    if(!options.parseOnly){
        outputPaths = OutputBundlePaths::derive(
            options.inputPath,
            options.outputRoot
        );
        if(!outputPaths->validateMirrors(
               options.listingPath,
               options.rawPath,
               std::cerr)){
            return 2;
        }
        if(!outputPaths->prepare(options.inputPath, std::cerr)){
            return 2;
        }
        errorCapture = std::make_unique<ErrorCapture>(std::cerr);
    }

    simulator::config::ConfigSearchOptions configOptions;
    configOptions.explicitPath = options.configPath;
    configOptions.parentSearchLimit = options.configSearchDepth;

    simulator::config::LoadedConfig loadedConfig;
    simulator::config::ConfigOverrides configOverrides;
    Parser parser(options.inputPath);
    Circuit circuit;
    AnalysisPlan plan;
    bool planAvailable = false;
    bool circuitAvailable = false;
    OperatingPointSolverOptions operatingPointConfig;
    PtaAnalysisConfig ptaConfig;
    bool effectiveConfigurationAvailable = false;
    bool writeSolveReport = true;
    SimulationReport report;
    report.inputPath = options.inputPath;
    report.configurationSource = options.configPath
        ? options.configPath->string() + " (load failed)"
        : "automatic config search (load failed)";

    std::ostringstream listing;
    std::ostringstream raw;

    const auto finish = [&](int requestedExitCode,
                            std::string statusDetail) {
        if(options.parseOnly){
            return requestedExitCode;
        }

        int exitCode = requestedExitCode;
        report.status = exitCode == 0 ? "succeeded" : "failed";
        report.statusDetail = std::move(statusDetail);
        const std::string listingOutput = listing.str();
        const std::string rawOutput = raw.str();

        // The old explicit file paths remain compatibility mirrors.  A mirror
        // error participates in the run result but never partially replaces
        // its listing/raw pair.
        if(exitCode == 0 && (options.listingPath || options.rawPath) &&
           !SpiceOutputFiles::writeAtomically(
               options.listingPath,
               listingOutput,
               options.rawPath,
               rawOutput,
               std::cerr)){
            exitCode = 1;
            report.status = "failed";
            report.statusDetail =
                "simulation converged, but a legacy output mirror failed";
        }

        report.totalWallSeconds = elapsedSeconds(simulationStart);
        std::optional<std::string> ptaTrace;
        if(circuitAvailable && effectiveConfigurationAvailable &&
           circuit.ptaDiagnostics().attempted){
            ptaTrace = PtaTraceWriter::write(
                circuit,
                ptaConfig,
                report.inputPath,
                report.configurationSource,
                report.status,
                report.statusDetail
            );
        }
        const std::optional<std::string_view> ptaTraceView = ptaTrace
            ? std::optional<std::string_view>(*ptaTrace)
            : std::nullopt;
        errorCapture->stop();
        const std::string errorLog = errorCapture->str();

        bool wroteArtifacts = false;
        if(writeSolveReport){
            std::ostringstream reportOutput;
            SolverReportWriter::write(
                reportOutput,
                report,
                circuitAvailable ? &circuit : nullptr,
                planAvailable ? &plan : nullptr,
                effectiveConfigurationAvailable ? &operatingPointConfig : nullptr,
                effectiveConfigurationAvailable ? &ptaConfig : nullptr
            );
            const std::string solverReport = reportOutput.str();
            wroteArtifacts = exitCode == 0
                ? SpiceOutputFiles::writeAtomically(
                    *outputPaths,
                    listingOutput,
                    rawOutput,
                    errorLog,
                    solverReport,
                    ptaTraceView,
                    std::cerr
                )
                : SpiceOutputFiles::writeFailureAtomically(
                    *outputPaths,
                    errorLog,
                    solverReport,
                    ptaTraceView,
                    std::cerr
                );
        } else {
            wroteArtifacts = exitCode == 0
                ? SpiceOutputFiles::writeAtomically(
                    *outputPaths,
                    listingOutput,
                    rawOutput,
                    errorLog,
                    ptaTraceView,
                    std::cerr
                )
                : SpiceOutputFiles::writeFailureAtomically(
                    *outputPaths,
                    errorLog,
                    ptaTraceView,
                    std::cerr
                );
        }
        if(!wroteArtifacts){
            return 1;
        }

        if(exitCode == 0 && !options.batchMode && !options.listingPath){
            std::cout << listingOutput;
            std::cout.flush();
            if(!std::cout){
                std::cerr << "Failed while writing listing output to stdout\n";
                return 1;
            }
        }
        return exitCode;
    };

    try {
        loadedConfig = simulator::config::loadConfig(configOptions);
        configOverrides =
            simulator::config::parseConfigOverrides(loadedConfig);
        report.configurationSource = configurationSource(loadedConfig);
        if(configOverrides.debug){
            writeSolveReport = *configOverrides.debug;
        }
        if(options.debug){
            writeSolveReport = *options.debug;
        }
    } catch(const std::runtime_error& error) {
        std::cerr << "Invalid configuration: " << error.what() << '\n';
        return finish(2, "configuration loading or validation failed");
    }

    if(options.printConfigPath){
        if(loadedConfig.found){
            std::cerr << "Configuration file: <"
                      << loadedConfig.path.string() << ">\n";
        } else {
            std::cerr
                << "Configuration file: none (using built-in defaults)\n";
        }
    }

    if(!parser.parse(circuit)){
        return finish(1, "netlist parsing failed");
    }
    circuitAvailable = true;
    report.title = parser.title();

    plan = parser.analysisPlan();
    planAvailable = true;
    const auto netlistLocks = simulator::config::parameterLocksFor(plan);

    const std::optional<TransientAnalysisConfig> netlistTransient =
        plan.transient;
    const std::optional<double> netlistMaximumTransientStep =
        netlistTransient ? netlistTransient->maximumStep : std::nullopt;

    ptaConfig = plan.pseudoTransient
        ? plan.pseudoTransient->makePtaConfig()
        : PtaAnalysisConfig{};

    try {
        simulator::config::applyConfigOverrides(
            configOverrides,
            operatingPointConfig,
            ptaConfig,
            plan.transient,
            plan.pseudoTransient.has_value(),
            netlistLocks
        );

        for(const std::string& assignment:
            options.operatingPointOptionAssignments)
        {
            std::string key;
            std::string error;
            if(!simulator::config::applyOperatingPointOption(
                   assignment,
                   operatingPointConfig,
                   key,
                   error)){
                throw std::invalid_argument(
                    "invalid command-line OP option <" + assignment +
                    ">: " + error
                );
            }
        }

        if(options.ptaModeSpecified && !netlistLocks.pta.mode){
            ptaConfig.mode = options.ptaMode;
        }
        for(const std::string& assignment: options.ptaOptionAssignments){
            std::string key;
            std::string error;
            if(!simulator::config::applyPtaOption(
                   assignment,
                   ptaConfig,
                   key,
                   error,
                   netlistLocks.pta)){
                throw std::invalid_argument(
                    "invalid command-line PTA option <" + assignment +
                    ">: " + error
                );
            }
        }
        for(const std::string& assignment: options.transientOptionAssignments){
            std::string key;
            std::string error;
            if(!simulator::config::applyTransientOption(
                   assignment,
                   plan.transient,
                   key,
                   error,
                   netlistTransient,
                   netlistMaximumTransientStep,
                   netlistLocks.transient)){
                throw std::invalid_argument(
                    "invalid command-line TRAN option <" + assignment +
                    ">: " + error
                );
            }
        }

        if((!options.ptaOptionAssignments.empty() || options.ptaDiagnostics) &&
           ptaConfig.mode == PtaMode::Disabled){
            throw std::invalid_argument(
                "PTA options and diagnostics require force or fallback mode"
            );
        }

        if(!operatingPointConfig.valid()){
            throw std::invalid_argument(
                "operating-point solver configuration is invalid"
            );
        }
        if(plan.transient && !plan.transient->valid()){
            throw std::invalid_argument(
                "transient solver configuration is invalid"
            );
        }
    } catch(const std::invalid_argument& error) {
        std::cerr << "Invalid analysis configuration: " << error.what() << '\n';
        return finish(2, "analysis configuration validation failed");
    }

    try {
        ptaConfig.validate();
    } catch(const std::invalid_argument& error) {
        std::cerr << "Invalid PTA configuration: " << error.what() << '\n';
        return finish(2, "PTA configuration validation failed");
    }
    effectiveConfigurationAvailable = true;

    if(options.parseOnly){
        return 0;
    }

    if(!circuit.build(ptaConfig)){
        std::cerr << "Failed to build circuit <" << options.inputPath << ">\n";
        return finish(1, "circuit matrix construction failed");
    }
    try {
        circuit.configureInitialConditions(plan);
    } catch(const std::exception& error) {
        std::cerr << "Invalid initial condition: " << error.what() << '\n';
        return finish(2, "netlist initial-condition validation failed");
    }

    bool wroteAnalysis = false;
    bool wrotePtaDiagnostics = false;

    try {
        if(plan.operatingPointRequested || !plan.transient){
            bool solved = false;
            std::string attemptLabel;

            switch (ptaConfig.mode){
                case PtaMode::Disabled:
                    attemptLabel = "ordinary operating point";
                    solved = circuit.solveOperatingPoint(operatingPointConfig);
                    break;
                case PtaMode::Force:
                    attemptLabel = "forced adaptive PTA operating point";
                    solved = circuit.solveAdaptivePta(ptaConfig);
                    break;
                case PtaMode::Fallback:
                    attemptLabel = "ordinary operating point with PTA fallback";
                    solved = circuit.solveOperatingPoint(operatingPointConfig);
                    if(!solved){
                        solved = circuit.solveAdaptivePta(ptaConfig);
                    }
                    break;
            }

            report.operatingPointAttempts.push_back({
                attemptLabel,
                circuit.operatingPointDiagnostics()
            });

            if(!solved){
                std::cerr << "Operating point analysis failed <"
                          << options.inputPath << ">\n";
                return finish(1, "operating-point analysis did not converge");
            }

            if(options.ptaDiagnostics){
                SpiceOutputWriter::writePtaDiagnostics(std::cerr, circuit);
                wrotePtaDiagnostics = true;
            }

            SpiceOutputWriter::writeOperatingPoint(
                listing,
                circuit,
                parser.title(),
                plan
            );
            SpiceRawWriter::writeOperatingPoint(
                raw,
                circuit,
                parser.title()
            );
            wroteAnalysis = true;
        }

        if(plan.transient){
            const bool solved = circuit.solveTransient(
                *plan.transient,
                operatingPointConfig
            );
            report.transient = circuit.transientDiagnostics();
            if(!plan.transient->useInitialConditions &&
               circuit.operatingPointDiagnostics().attempted){
                report.operatingPointAttempts.push_back({
                    "transient operating-point initialization",
                    circuit.operatingPointDiagnostics()
                });
            }

            if(!solved){
                std::cerr << "Transient analysis failed <"
                          << options.inputPath << ">\n";
                return finish(1, "transient analysis did not converge");
            }
            if(wroteAnalysis){
                listing << '\n';
                raw << '\n';
            }
            SpiceOutputWriter::writeTransient(
                listing,
                circuit,
                parser.title(),
                plan
            );
            SpiceRawWriter::writeTransient(
                raw,
                circuit,
                parser.title()
            );
        }

        if(options.ptaDiagnostics && !wrotePtaDiagnostics){
            SpiceOutputWriter::writePtaDiagnostics(std::cerr, circuit);
        }
    } catch(const std::exception& exception){
        std::cerr << "Output error: " << exception.what() << '\n';
        return finish(1, "analysis result serialization failed");
    }

    return finish(0, "all requested analyses converged and artifacts were written");
}
