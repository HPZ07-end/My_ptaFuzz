#include "app/command_line.hpp"

#include <cctype>
#include <limits>
#include <optional>
#include <ostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "analysis/solver_options.hpp"
#include "analysis/transient_analysis.hpp"
#include "config/overrides.hpp"
#include "utils/string.hpp"

namespace simulator::app {
namespace {

bool parseNonNegativeSize(const std::string& text, std::size_t& value){
    if(text.empty()){
        return false;
    }

    for(unsigned char character: text){
        if(!std::isdigit(character)){
            return false;
        }
    }

    try {
        std::size_t parsedLength = 0;
        const unsigned long long parsed =
            std::stoull(text, &parsedLength, 10);
        if(parsedLength != text.size() ||
           parsed > std::numeric_limits<std::size_t>::max()){
            return false;
        }
        value = static_cast<std::size_t>(parsed);
        return true;
    } catch(const std::exception&) {
        return false;
    }
}

bool parsePtaMode(const std::string& text, PtaMode& mode){
    if(text == "disabled"){
        mode = PtaMode::Disabled;
        return true;
    }
    if(text == "force"){
        mode = PtaMode::Force;
        return true;
    }
    if(text == "fallback"){
        mode = PtaMode::Fallback;
        return true;
    }
    return false;
}

bool parseBoolean(const std::string& text, bool& value){
    const std::string normalized = to_lower_copy(text);
    if(normalized == "true" || normalized == "1"){
        value = true;
        return true;
    }
    if(normalized == "false" || normalized == "0"){
        value = false;
        return true;
    }
    return false;
}

}  // namespace

void printUsage(std::ostream& output, const char* program){
    output << "Usage:\n"
           << "  " << program << " <input.cir> [listing-copy.out]\n"
           << "  " << program << " --parse-only <input.cir>\n"
           << "  " << program
           << " [-b] [--pta disabled|force|fallback]"
           << " [--pta-diagnostics]"
           << " [--op-option name=value]"
           << " [--pta-option name=value]"
           << " [--tran-option name=value]"
           << " [--config path] [--config-search-depth N]"
           << " [--print-config-path]"
           << " [--debug true|false]"
           << " [--output-root directory]"
           << " [-o listing-copy.out] [-r raw-copy.raw] <input.cir>\n";
    output << "\nOutput options:\n"
           << "  --output-root directory    Place the <netlist-name>/ result folder here\n"
           << "  -o, --output path          Also copy the listing to this legacy path\n"
           << "  -r, --rawfile path         Also copy the rawfile to this legacy path\n"
           << "  -b, --batch                Write artifacts without echoing the listing\n"
           << "  --debug true|false         Enable or disable the .solve.txt report\n";
    output << "\nConfiguration options:\n"
           << "  --config path              Read this configuration file only\n"
           << "  --config-search-depth N    Search at most N parent directories\n"
           << "  --print-config-path         Report the selected configuration file\n";
    output << "\nAnalysis overrides (repeat as needed; netlist controls win):\n"
           << "  --op-option name=value    OP fields: newton.* or source-stepping.*\n"
           << "  --tran-option name=value  TRAN fields: top-level or solver.*\n"
           << "  --pta-option name=value   PTA fields below; --pta selects its mode\n";
    output << "\nNewton option fields (under newton.*):\n"
           << "  maximum-iterations, relative-tolerance,\n"
           << "  voltage-absolute-tolerance, current-absolute-tolerance,\n"
           << "  normalized-update-tolerance, normalized-residual-tolerance,\n"
           << "  maximum-backtracks, backtrack-scale, sufficient-decrease,\n"
           << "  maximum-solution-step (tolerance is a legacy absolute alias)\n";
    output << "\nPTA option fields:\n"
           << "  initial-step, minimum-step, maximum-step, maximum-steps\n"
           << "  derivative-tolerance, derivative-relative-tolerance,\n"
           << "  derivative-voltage-absolute-tolerance,\n"
           << "  derivative-current-absolute-tolerance, dc-residual-tolerance\n"
           << "  dc-residual-relative-tolerance, dc-voltage-absolute-tolerance,\n"
           << "  dc-current-absolute-tolerance\n"
           << "  initial-node-capacitance, minimum-node-capacitance,\n"
           << "  maximum-node-capacitance, current-source-capacitance,\n"
           << "  voltage-source-inductance\n"
           << "  failed-step-scale, successful-step-scale,\n"
           << "  capacitance-grow-scale,\n"
           << "  small-oscillation-scale, medium-oscillation-scale,\n"
           << "  heavy-oscillation-scale, medium-oscillation-ratio,\n"
           << "  heavy-oscillation-ratio, compound-time-constant,\n"
           << "  compound-initial-resistance, compound-initial-conductance,\n"
           << "  source-ramp-time, initial-mos-vgs, initial-bjt-vbe (or null)\n"
           << "  include-mos-bulk, include-diodes (true or false)\n";
}

bool parseCommandLine(int argc,
                      char* argv[],
                      CommandLineOptions& options,
                      std::ostream& output,
                      std::ostream& error){
    std::vector<std::string> positional;
    std::set<std::string> operatingPointOptionKeys;
    std::set<std::string> ptaOptionKeys;
    std::set<std::string> transientOptionKeys;
    bool configSearchDepthSpecified = false;

    for(int i = 1; i < argc; ++i){
        const std::string argument = argv[i];
        if(argument == "-h" || argument == "--help"){
            printUsage(output, argv[0]);
            options.helpRequested = true;
            return false;
        }
        if(argument == "-b" || argument == "--batch"){
            if(options.batchMode){
                error << "Repeated batch option\n";
                return false;
            }
            options.batchMode = true;
            continue;
        }
        if(argument == "--output-root"){
            if(++i >= argc || options.outputRoot ||
               std::string(argv[i]).empty() || argv[i][0] == '-'){
                error << "Invalid or repeated output root option\n";
                return false;
            }
            options.outputRoot = std::filesystem::path(argv[i]);
            continue;
        }
        if(argument == "--config"){
            if(++i >= argc || options.configPath ||
               std::string(argv[i]).empty() || argv[i][0] == '-'){
                error << "Invalid or repeated configuration file option\n";
                return false;
            }
            options.configPath = std::filesystem::path(argv[i]);
            continue;
        }
        if(argument == "--config-search-depth"){
            if(++i >= argc || configSearchDepthSpecified ||
               !parseNonNegativeSize(argv[i], options.configSearchDepth)){
                error
                    << "Invalid or repeated configuration search depth; "
                    << "expected a non-negative integer\n";
                return false;
            }
            configSearchDepthSpecified = true;
            continue;
        }
        if(argument == "--print-config-path"){
            if(options.printConfigPath){
                error << "Repeated print-config-path option\n";
                return false;
            }
            options.printConfigPath = true;
            continue;
        }
        if(argument == "--parse-only"){
            if(options.parseOnly){
                error << "Repeated parse-only option\n";
                return false;
            }
            options.parseOnly = true;
            continue;
        }
        if(argument == "--pta-diagnostics"){
            if(options.ptaDiagnostics){
                error << "Repeated PTA diagnostics option\n";
                return false;
            }
            options.ptaDiagnostics = true;
            continue;
        }
        if(argument == "--debug"){
            bool debug = false;
            if(++i >= argc || options.debug ||
               !parseBoolean(argv[i], debug)){
                error
                    << "Invalid or repeated debug option; expected true or false\n";
                return false;
            }
            options.debug = debug;
            continue;
        }
        if(argument == "--pta"){
            if(++i >= argc || options.ptaModeSpecified ||
               !parsePtaMode(argv[i], options.ptaMode)){
                error
                    << "Invalid or repeated PTA mode; expected "
                    << "disabled, force, or fallback\n";
                return false;
            }
            options.ptaModeSpecified = true;
            continue;
        }
        if(argument == "--op-option"){
            if(++i >= argc){
                error << "Missing OP option; expected name=value\n";
                return false;
            }

            OperatingPointSolverOptions validationOptions;
            std::string key;
            std::string validationError;
            if(!config::applyOperatingPointOption(
                   argv[i], validationOptions, key, validationError)){
                error << "Invalid OP option <" << argv[i]
                      << ">: " << validationError << '\n';
                return false;
            }
            if(!operatingPointOptionKeys.insert(key).second){
                error << "Repeated OP option: " << key << '\n';
                return false;
            }
            options.operatingPointOptionAssignments.emplace_back(argv[i]);
            continue;
        }
        if(argument == "--pta-option"){
            if(++i >= argc){
                error << "Missing PTA option; expected name=value\n";
                return false;
            }

            std::string key;
            std::string validationError;
            PtaAnalysisConfig validationOptions;
            if(!config::applyPtaOption(
                   argv[i], validationOptions, key, validationError)){
                error << "Invalid PTA option <" << argv[i]
                      << ">: " << validationError << '\n';
                return false;
            }
            if(!ptaOptionKeys.insert(key).second){
                error << "Repeated PTA option: " << key << '\n';
                return false;
            }
            options.ptaOptionAssignments.emplace_back(argv[i]);
            continue;
        }
        if(argument == "--tran-option"){
            if(++i >= argc){
                error << "Missing TRAN option; expected name=value\n";
                return false;
            }

            std::optional<TransientAnalysisConfig> validationOptions;
            std::string key;
            std::string validationError;
            if(!config::applyTransientOption(
                   argv[i], validationOptions, key, validationError)){
                error << "Invalid TRAN option <" << argv[i]
                      << ">: " << validationError << '\n';
                return false;
            }
            if(!transientOptionKeys.insert(key).second){
                error << "Repeated TRAN option: " << key << '\n';
                return false;
            }
            options.transientOptionAssignments.emplace_back(argv[i]);
            continue;
        }
        if(argument == "-o" || argument == "--output"){
            if(++i >= argc || options.listingPath ||
               std::string(argv[i]).empty() || argv[i][0] == '-'){
                error << "Invalid or repeated listing output option\n";
                return false;
            }
            options.listingPath = argv[i];
            continue;
        }
        if(argument == "-r" || argument == "--rawfile"){
            if(++i >= argc || options.rawPath ||
               std::string(argv[i]).empty() || argv[i][0] == '-'){
                error << "Invalid or repeated rawfile option\n";
                return false;
            }
            options.rawPath = argv[i];
            continue;
        }
        if(!argument.empty() && argument[0] == '-'){
            error << "Unknown option: " << argument << '\n';
            return false;
        }
        positional.push_back(argument);
    }

    if(positional.empty() || positional.size() > 2){
        error << "Exactly one input netlist is required\n";
        return false;
    }

    options.inputPath = positional[0];
    if(positional.size() == 2){
        if(options.listingPath){
            error << "Listing output was specified twice\n";
            return false;
        }
        options.listingPath = positional[1];
    }
    if(options.parseOnly &&
       (options.outputRoot || options.listingPath || options.rawPath ||
        options.debug ||
        options.ptaModeSpecified ||
        options.ptaDiagnostics || !operatingPointOptionKeys.empty() ||
        !ptaOptionKeys.empty() || !transientOptionKeys.empty())){
        error
            << "--parse-only cannot be combined with output or analysis options\n";
        return false;
    }
    return true;
}

}  // namespace simulator::app
