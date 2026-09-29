#include "app/command_line.hpp"
#include "netlist/spice_syntax.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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

void expectNear(double actual,
                double expected,
                const std::string& description){
    const double tolerance = 1.0e-12 *
        std::max({1.0, std::abs(actual), std::abs(expected)});
    expect(
        std::isfinite(actual) && std::abs(actual - expected) <= tolerance,
        description
    );
}

struct CommandLineResult {
    bool success = false;
    simulator::app::CommandLineOptions options;
    std::string output;
    std::string error;
};

CommandLineResult parseCommandLine(std::initializer_list<const char*> arguments){
    std::vector<std::string> storage;
    storage.reserve(arguments.size());
    for(const char* argument: arguments){
        storage.emplace_back(argument);
    }

    std::vector<char*> argv;
    argv.reserve(storage.size());
    for(std::string& argument: storage){
        argv.push_back(argument.data());
    }

    CommandLineResult result;
    std::ostringstream output;
    std::ostringstream error;
    result.success = simulator::app::parseCommandLine(
        static_cast<int>(argv.size()),
        argv.data(),
        result.options,
        output,
        error
    );
    result.output = output.str();
    result.error = error.str();
    return result;
}

void testBasicCommandLine(){
    const CommandLineResult basic = parseCommandLine({"spice", "input.cir"});
    expect(basic.success, "single input netlist is accepted");
    expect(basic.options.inputPath == "input.cir", "input path is retained");
    expect(!basic.options.outputRoot, "output root defaults beside the netlist");
    expect(!basic.options.listingPath, "legacy listing mirror is disabled by default");
    expect(!basic.options.debug, "debug report setting defaults to the configuration layer");
    expect(basic.options.configSearchDepth == 8, "config search depth defaults to eight");

    const CommandLineResult positional = parseCommandLine(
        {"spice", "input.cir", "result.out"}
    );
    expect(positional.success, "positional listing path is accepted");
    expect(
        positional.options.listingPath == std::optional<std::string>("result.out"),
        "positional listing path is retained"
    );
}

void testCompleteCommandLine(){
    const CommandLineResult result = parseCommandLine({
        "spice",
        "-b",
        "--output-root", "artifacts",
        "--config", "settings.json",
        "--config-search-depth", "3",
        "--print-config-path",
        "--debug", "false",
        "--pta", "fallback",
        "--pta-diagnostics",
        "--op-option", "newton.tolerance=1n",
        "--pta-option", "maximum-steps=42",
        "--tran-option", "solver.maximum-rejects=3",
        "-o", "result.out",
        "-r", "result.raw",
        "input.cir"
    });

    expect(result.success, "complete command line is accepted");
    expect(result.options.batchMode, "batch mode is retained");
    expect(
        result.options.outputRoot == std::filesystem::path("artifacts"),
        "structured output root is retained"
    );
    expect(
        result.options.configPath == std::filesystem::path("settings.json"),
        "explicit config path is retained"
    );
    expect(result.options.configSearchDepth == 3, "explicit search depth is retained");
    expect(result.options.printConfigPath, "config diagnostics flag is retained");
    expect(
        result.options.debug == std::optional<bool>(false),
        "debug report option is retained"
    );
    expect(
        result.options.ptaModeSpecified &&
            result.options.ptaMode == PtaMode::Fallback,
        "PTA fallback mode is retained"
    );
    expect(result.options.ptaDiagnostics, "PTA diagnostics flag is retained");
    expect(
        result.options.operatingPointOptionAssignments.size() == 1 &&
            result.options.ptaOptionAssignments.size() == 1 &&
            result.options.transientOptionAssignments.size() == 1,
        "analysis overrides are retained by layer"
    );
    expect(
        result.options.listingPath == std::optional<std::string>("result.out") &&
            result.options.rawPath == std::optional<std::string>("result.raw"),
        "explicit output paths are retained"
    );
}

void testCommandLineFailures(){
    const CommandLineResult help = parseCommandLine({"spice", "--help"});
    expect(!help.success && help.options.helpRequested, "help exits parsing deliberately");
    expect(help.output.find("Usage:") != std::string::npos, "help writes usage text");
    expect(help.error.empty(), "help does not write an error");

    const std::vector<std::pair<CommandLineResult, std::string>> failures = {
        {parseCommandLine({"spice"}), "missing input is rejected"},
        {parseCommandLine({"spice", "--unknown", "input.cir"}),
         "unknown option is rejected"},
        {parseCommandLine({"spice", "--config-search-depth", "-1", "input.cir"}),
         "negative search depth is rejected"},
        {parseCommandLine({"spice", "--parse-only", "-o", "out", "input.cir"}),
         "parse-only output conflict is rejected"},
        {parseCommandLine({
             "spice", "--parse-only", "--output-root", "results", "input.cir"
         }), "parse-only structured output conflict is rejected"},
        {parseCommandLine({
             "spice", "--parse-only", "--debug", "false", "input.cir"
         }), "parse-only debug output conflict is rejected"},
        {parseCommandLine({"spice", "--debug", "enabled", "input.cir"}),
         "invalid debug boolean is rejected"},
        {parseCommandLine({
             "spice", "--debug", "true", "--debug", "false", "input.cir"
         }), "repeated debug option is rejected"},
        {parseCommandLine({
             "spice", "--output-root", "one", "--output-root", "two", "input.cir"
         }), "duplicate output root is rejected"},
        {parseCommandLine({"spice", "-o", "one", "input.cir", "two"}),
         "duplicate positional listing is rejected"},
        {parseCommandLine({
             "spice",
             "--op-option", "newton.maximum_iterations=10",
             "--op-option", "newton.maximum-iterations=20",
             "input.cir"
         }), "normalized duplicate OP option is rejected"},
        {parseCommandLine({
             "spice", "--pta-option", "maximum-steps=zero", "input.cir"
         }), "invalid PTA assignment is rejected"},
        {parseCommandLine({"spice", "--pta", "unknown", "input.cir"}),
         "unknown PTA mode is rejected"}
    };

    for(const auto& failure: failures){
        expect(!failure.first.success, failure.second);
        expect(!failure.first.error.empty(), failure.second + " with a diagnostic");
    }
}

void testSpiceNumbers(){
    const std::vector<std::pair<std::string, double>> valid = {
        {"0", 0.0},
        {"+.5", 0.5},
        {"-2.", -2.0},
        {"1e3", 1.0e3},
        {"2K", 2.0e3},
        {"3meg", 3.0e6},
        {"4MIL", 4.0 * 25.4e-6},
        {"5m", 5.0e-3},
        {"6u", 6.0e-6},
        {"7n", 7.0e-9},
        {"8p", 8.0e-12}
    };
    for(const auto& number: valid){
        expectNear(
            parse_spice_number(number.first),
            number.second,
            "valid SPICE number " + number.first
        );
    }

    const std::vector<std::string> invalid = {
        "", "+", ".", "1e", "1e+", "1..0", "1k=2", "1_0",
        "nan", "inf", "1e999"
    };
    for(const std::string& number: invalid){
        bool threw = false;
        try {
            static_cast<void>(parse_spice_number(number));
        } catch(const std::runtime_error&) {
            threw = true;
        }
        expect(threw, "invalid SPICE number is rejected: " + number);
    }
}

void testSpiceTokenUtilities(){
    const auto tokens = tokenize_spice_line(
        "R1 in out {base * 2 + 1}, model"
    );
    expect(tokens.size() == 5, "tokenizer preserves expected token count");
    expect(tokens[3] == "{base * 2 + 1}", "tokenizer preserves brace expression");
    expect(tokens[4] == "model", "tokenizer handles comma separator");

    expect(
        strip_spice_comment("R1 1 0 1k // comment") == "R1 1 0 1k ",
        "double-slash comment is stripped"
    );
    expect(
        strip_spice_comment("R1 1 0 1k; comment") == "R1 1 0 1k",
        "semicolon comment is stripped"
    );

    std::string key;
    std::string value;
    std::size_t index = 0;
    expect(
        read_spice_assignment({"W=2u"}, index, key, value) &&
            index == 0 && key == "w" && value == "2u",
        "inline assignment is parsed"
    );

    index = 0;
    expect(
        read_spice_assignment({"L", "=", "3u"}, index, key, value) &&
            index == 2 && key == "l" && value == "3u",
        "separated assignment is parsed"
    );

    index = 0;
    expect(
        read_spice_assignment({"AREA", "=4"}, index, key, value) &&
            index == 1 && key == "area" && value == "4",
        "prefixed-equals assignment is parsed"
    );

    expectNear(
        parse_spice_value_token({"DC", "=", "5m"}, 0),
        5.0e-3,
        "separated DC source value is parsed"
    );
    expectNear(
        parse_spice_named_value({"r=1k", "c=2u"}, "C", 0.0),
        2.0e-6,
        "named value lookup is case-insensitive"
    );
    expectNear(
        parse_spice_named_value({"r=1k"}, "missing", 3.0),
        3.0,
        "named value lookup preserves fallback"
    );
}

}  // namespace

int main(){
    testBasicCommandLine();
    testCompleteCommandLine();
    testCommandLineFailures();
    testSpiceNumbers();
    testSpiceTokenUtilities();

    if(failureCount != 0){
        std::cerr << failureCount << " of " << checkCount
                  << " core checks failed\n";
        return 1;
    }

    std::cout << "Core unit tests: " << checkCount
              << "/" << checkCount << " checks passed\n";
    return 0;
}
