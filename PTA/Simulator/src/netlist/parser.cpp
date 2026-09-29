#include <array>
#include <cctype>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "netlist/parser.hpp"
#include "netlist/reader.hpp"
#include "netlist/subcircuit.hpp"
#include "circuit/circuit.hpp"
#include "devices/bjt.hpp"
#include "devices/capacitor.hpp"
#include "devices/current_source.hpp"
#include "devices/diode.hpp"
#include "devices/inductor.hpp"
#include "devices/mosfet.hpp"
#include "devices/resistor.hpp"
#include "devices/voltage_source.hpp"
#include "models/model.hpp"
#include "netlist/spice_syntax.hpp"

namespace {
ModelType parseModelType(const std::string& token){
    const std::string type = to_lower_copy(token);
    if(type == "d") return ModelType::Diode;
    if(type == "npn") return ModelType::NPN;
    if(type == "pnp") return ModelType::PNP;
    if(type == "nmos" || type == "nch") return ModelType::NMOS;
    if(type == "pmos" || type == "pch") return ModelType::PMOS;
    return ModelType::Unknown;
}

bool supportsModelParameter(ModelType type, const std::string& key){
    if(type == ModelType::Diode){
        return key == "is" || key == "n" || key == "vt" ||
               key == "rs" || key == "gmin";
    }
    if(type == ModelType::NPN || type == ModelType::PNP){
        return key == "is" || key == "bf" || key == "beta" ||
               key == "br" || key == "nf" || key == "nr" ||
               key == "vt" || key == "gmin" || key == "rb" ||
               key == "rc" || key == "re" || key == "va" ||
               key == "vaf" || key == "var" || key == "ikf" ||
               key == "ikr" || key == "ise" || key == "isc" ||
               key == "ne" || key == "nc" || key == "rbe" ||
               key == "rce";
    }
    if(type == ModelType::NMOS || type == ModelType::PMOS){
        return key == "level" || key == "vto" || key == "vt0" ||
               key == "kp" || key == "k" || key == "lambda" ||
               key == "lam" || key == "gmin" || key == "rds" ||
               key == "gamma" || key == "phi" || key == "rd" ||
               key == "rs" || key == "rsh" || key == "cbd" ||
               key == "cbs" || key == "is" || key == "js" ||
               key == "pb" || key == "fc" || key == "cj" ||
               key == "mj" || key == "cjsw" || key == "mjsw" ||
               key == "cgso" || key == "cgdo" || key == "cgbo" ||
               key == "tox" || key == "ld" || key == "xl" ||
               key == "xw" || key == "wd" || key == "xj" ||
               key == "nsub" || key == "uo" || key == "u0" ||
               key == "tpg" || key == "nss" || key == "vmax" ||
               key == "nfs" || key == "eta" || key == "delta" ||
               key == "theta" || key == "kappa" || key == "tnom" ||
               key == "kf" || key == "af";
    }
    return false;
}

void validateModelParameter(ModelType type,
                            const std::string& key,
                            double value){
    if(!supportsModelParameter(type, key)){
        throw std::runtime_error("Unsupported model parameter: " + key);
    }
    if(!std::isfinite(value)){
        throw std::runtime_error(
            "Model parameter " + key + " must be finite"
        );
    }
    if(type == ModelType::NMOS || type == ModelType::PMOS){
        if(key == "level"){
            if(value != 1.0 && value != 3.0){
                throw std::runtime_error(
                    "Only MOSFET LEVEL=1 or LEVEL=3 compatibility cards are supported"
                );
            }
            return;
        }
        if(key == "vto" || key == "vt0" || key == "tpg" ||
           key == "xl" || key == "xw" || key == "wd"){
            return;
        }
        if(key == "kp" || key == "k"){
            if(value <= 0.0){
                throw std::runtime_error(
                    "Model parameter " + key + " must be positive"
                );
            }
            return;
        }
        if(value < 0.0){
            throw std::runtime_error(
                "Model parameter " + key + " must be non-negative"
            );
        }
        return;
    }
    if(key == "rs" || key == "lambda" || key == "lam" ||
       key == "rb" || key == "rc" || key == "re" || key == "va" ||
       key == "vaf" || key == "var" || key == "ikf" || key == "ikr" ||
       key == "ise" || key == "isc"){
        if(value < 0.0){
            throw std::runtime_error(
                "Model parameter " + key + " must be non-negative"
            );
        }
        return;
    }
    if(value <= 0.0){
        throw std::runtime_error(
            "Model parameter " + key + " must be positive"
        );
    }
}

std::string canonicalMosModelParameter(std::string key){
    if(key == "vt0") return "vto";
    if(key == "k") return "kp";
    if(key == "u0") return "uo";
    if(key == "lam") return "lambda";
    return key;
}

void skipPrintSeparators(const std::string& text, std::size_t& pos){
    while(pos < text.size() &&
          (std::isspace(static_cast<unsigned char>(text[pos])) ||
           text[pos] == ',')){
        ++pos;
    }
}

bool startsWithWordIgnoreCase(const std::string& text,
                              std::size_t pos,
                              const std::string& word){
    if(pos + word.size() > text.size()){
        return false;
    }
    if(!equal_ignore_case(text.substr(pos, word.size()), word)){
        return false;
    }

    const std::size_t end = pos + word.size();
    return end == text.size() ||
           std::isspace(static_cast<unsigned char>(text[end])) ||
           text[end] == ',';
}

std::vector<std::string> parsePrintArguments(const std::string& expression){
    std::string normalized = expression;
    for(char& c: normalized){
        if(c == ','){
            c = ' ';
        }
    }

    std::istringstream iss(normalized);
    std::vector<std::string> arguments;
    std::string argument;
    while(iss >> argument){
        arguments.push_back(to_lower_copy(argument));
    }
    return arguments;
}

bool samePrintVariable(const PrintVariable& lhs, const PrintVariable& rhs){
    return lhs.quantity == rhs.quantity &&
           lhs.name == rhs.name &&
           lhs.reference == rhs.reference;
}

bool hasNonGroundNode(const std::vector<std::string>& tokens){
    if(tokens.empty() || tokens[0].empty()){
        return false;
    }

    std::size_t nodeCount = 0;
    switch(std::toupper(static_cast<unsigned char>(tokens[0][0]))){
        case 'Q': nodeCount = 3; break;
        case 'M': nodeCount = 4; break;
        case 'R':
        case 'C':
        case 'L':
        case 'V':
        case 'I':
        case 'D': nodeCount = 2; break;
        default: return false;
    }

    for(std::size_t i = 1; i <= nodeCount && i < tokens.size(); ++i){
        if(tokens[i] != "0" && !equal_ignore_case(tokens[i], "gnd")){
            return true;
        }
    }
    return false;
}

void appendUniquePrintVariable(std::vector<PrintVariable>& variables,
                               PrintVariable variable){
    for(const auto& existing: variables){
        if(samePrintVariable(existing, variable)){
            return;
        }
    }
    variables.push_back(std::move(variable));
}

std::string canonicalOutputNode(std::string name){
    if(equal_ignore_case(name, "gnd")){
        return "0";
    }
    return to_lower_copy(std::move(name));
}

std::vector<std::string> elementNodes(const std::vector<std::string>& tokens,
                                      std::size_t first,
                                      std::size_t count){
    std::vector<std::string> nodes;
    nodes.reserve(count);
    for(std::size_t i = 0; i < count; ++i){
        nodes.push_back(canonicalOutputNode(tokens.at(first + i)));
    }
    return nodes;
}

std::size_t sourceValueEnd(const std::vector<std::string>& tokens,
                           std::size_t first){
    if(first >= tokens.size()){
        throw std::runtime_error("Missing source value");
    }
    if(equal_ignore_case(tokens[first], "dc")){
        std::size_t valueIndex = first + 1;
        if(valueIndex < tokens.size() && tokens[valueIndex] == "="){
            ++valueIndex;
        }
        if(valueIndex >= tokens.size()){
            throw std::runtime_error("Missing DC source value");
        }
        return valueIndex + 1;
    }
    if(equal_ignore_case(tokens[first], "dc=")){
        if(first + 1 >= tokens.size()){
            throw std::runtime_error("Missing DC source value");
        }
        return first + 2;
    }
    return first + 1;
}

double positiveElementValue(const std::vector<std::string>& tokens,
                            std::size_t index,
                            const char* elementName){
    const double value = parse_spice_number(tokens.at(index));
    if(value <= 0.0){
        throw std::runtime_error(
            std::string(elementName) + " value must be positive"
        );
    }
    return value;
}

struct InstanceValues {
    double area = 1.0;
    double width = 1.0;
    double length = 1.0;
    std::optional<std::array<double, 2>> initialCondition;
};

InstanceValues parseInstanceValues(const std::vector<std::string>& tokens,
                                   std::size_t first,
                                   bool allowArea,
                                   bool allowGeometry,
                                   bool allowPositionalArea,
                                   bool allowInitialCondition = false){
    InstanceValues values;
    bool areaSeen = false;
    bool widthSeen = false;
    bool lengthSeen = false;
    bool initialConditionSeen = false;

    if(allowPositionalArea && first < tokens.size() &&
       tokens[first].find('=') == std::string::npos &&
       !(first + 1 < tokens.size() && !tokens[first + 1].empty() &&
         tokens[first + 1][0] == '=')){
        values.area = parse_spice_number(tokens[first++]);
        if(values.area <= 0.0){
            throw std::runtime_error("Instance parameter area must be positive");
        }
        areaSeen = true;
    }

    for(std::size_t i = first; i < tokens.size(); ++i){
        std::string key;
        std::string value;
        if(!read_spice_assignment(tokens, i, key, value)){
            throw std::runtime_error(
                "Unsupported or malformed instance parameter: " + tokens[i]
            );
        }

        if(key == "ic" && allowInitialCondition){
            if(initialConditionSeen){
                throw std::runtime_error("Repeated BJT instance parameter: ic");
            }
            if(i + 1 >= tokens.size()){
                throw std::runtime_error("BJT IC requires VBE and VCE values");
            }
            values.initialCondition = std::array<double, 2>{
                parse_spice_number(value),
                parse_spice_number(tokens[++i])
            };
            initialConditionSeen = true;
            continue;
        }

        const double parsed = parse_spice_number(value);
        if(parsed <= 0.0){
            throw std::runtime_error(
                "Instance parameter " + key + " must be positive"
            );
        }

        if(key == "area" && allowArea && !areaSeen){
            values.area = parsed;
            areaSeen = true;
        } else if(key == "w" && allowGeometry && !widthSeen){
            values.width = parsed;
            widthSeen = true;
        } else if(key == "l" && allowGeometry && !lengthSeen){
            values.length = parsed;
            lengthSeen = true;
        } else {
            throw std::runtime_error(
                "Unsupported or repeated instance parameter: " + key
            );
        }
    }
    return values;
}

MOSFET::MosInstanceParams parseMosInstanceParams(
    const std::vector<std::string>& tokens,
    std::size_t first
){
    MOSFET::MosInstanceParams values;
    std::unordered_set<std::string> seen;

    const auto requireFinite = [](double value, const std::string& key){
        if(!std::isfinite(value)){
            throw std::runtime_error(
                "MOSFET instance parameter " + key + " must be finite"
            );
        }
        return value;
    };

    for(std::size_t i = first; i < tokens.size(); ++i){
        if(equal_ignore_case(tokens[i], "off")){
            if(!seen.insert("off").second){
                throw std::runtime_error("Repeated MOSFET instance parameter: off");
            }
            values.off = true;
            continue;
        }

        std::string key;
        std::string value;
        if(!read_spice_assignment(tokens, i, key, value)){
            throw std::runtime_error(
                "Unsupported or malformed MOSFET instance parameter: " +
                tokens[i]
            );
        }
        if(!seen.insert(key).second){
            throw std::runtime_error(
                "Repeated MOSFET instance parameter: " + key
            );
        }

        if(key == "ic"){
            if(i + 2 >= tokens.size()){
                throw std::runtime_error(
                    "MOSFET IC requires VDS, VGS, and VBS values"
                );
            }
            values.ic = std::array<double, 3>{
                requireFinite(parse_spice_number(value), key),
                requireFinite(parse_spice_number(tokens[++i]), key),
                requireFinite(parse_spice_number(tokens[++i]), key)
            };
            continue;
        }

        const double parsed = requireFinite(parse_spice_number(value), key);
        if(key == "w"){
            if(parsed <= 0.0) throw std::runtime_error("MOSFET W must be positive");
            values.w = parsed;
        } else if(key == "l"){
            if(parsed <= 0.0) throw std::runtime_error("MOSFET L must be positive");
            values.l = parsed;
        } else if(key == "m"){
            if(parsed <= 0.0) throw std::runtime_error("MOSFET M must be positive");
            values.m = parsed;
        } else if(key == "ad"){
            if(parsed < 0.0) throw std::runtime_error("MOSFET AD must be non-negative");
            values.ad = parsed;
        } else if(key == "as"){
            if(parsed < 0.0) throw std::runtime_error("MOSFET AS must be non-negative");
            values.as = parsed;
        } else if(key == "pd"){
            if(parsed < 0.0) throw std::runtime_error("MOSFET PD must be non-negative");
            values.pd = parsed;
        } else if(key == "ps"){
            if(parsed < 0.0) throw std::runtime_error("MOSFET PS must be non-negative");
            values.ps = parsed;
        } else if(key == "nrd"){
            if(parsed < 0.0) throw std::runtime_error("MOSFET NRD must be non-negative");
            values.nrd = parsed;
        } else if(key == "nrs"){
            if(parsed < 0.0) throw std::runtime_error("MOSFET NRS must be non-negative");
            values.nrs = parsed;
        } else if(key == "temp"){
            values.temp = parsed;
        } else {
            throw std::runtime_error(
                "Unsupported MOSFET instance parameter: " + key
            );
        }
    }
    return values;
}

std::optional<double> parseDynamicInitialCondition(
    const std::vector<std::string>& tokens,
    std::size_t first,
    const char* elementName
){
    std::optional<double> initialCondition;
    for(std::size_t i = first; i < tokens.size(); ++i){
        std::string key;
        std::string value;
        if(!read_spice_assignment(tokens, i, key, value) || key != "ic"){
            throw std::runtime_error(
                std::string(elementName) +
                " accepts only an optional IC=<initial-condition>"
            );
        }
        if(initialCondition){
            throw std::runtime_error(
                std::string("Repeated ") + elementName + " IC parameter"
            );
        }
        initialCondition = parse_spice_number(value);
    }
    return initialCondition;
}

bool readInitialConditionValue(const std::vector<std::string>& tokens,
                               std::size_t& index,
                               double& value){
    if(index >= tokens.size()){
        return false;
    }
    const std::string& token = tokens[index];
    if(token.empty() || token[0] != '='){
        return false;
    }
    std::string text = token.substr(1);
    if(text.empty()){
        ++index;
        if(index >= tokens.size()){
            return false;
        }
        text = tokens[index];
    }
    value = parse_spice_number(text);
    ++index;
    return true;
}
}

bool Parser::parse(Circuit& circuit){
    analysisPlan_ = {};
    title_.clear();

    NetlistSource source;
    if(!NetlistReader(filename_).read(source)){
        return false;
    }
    title_ = std::move(source.title);

    std::size_t activeLine = 1;
    try {
        SubcircuitLibrary subcircuits;
        std::vector<LogicalNetlistLine> topLevelLines =
            subcircuits.collect(std::move(source.lines));

        for(const auto& logicalLine: topLevelLines){
            activeLine = logicalLine.lineNumber;
            const auto& tokens = logicalLine.tokens;
            if(tokens[0][0] != '.'){
                continue;
            }

            if(equal_ignore_case(tokens[0], ".model")){
                continue;
            }
            if(equal_ignore_case(tokens[0], ".op") ||
               equal_ignore_case(tokens[0], ".tran")){
                parseAnalysisDirective(tokens);
                continue;
            }
            if(equal_ignore_case(tokens[0], ".pstran")){
                parsePstranDirective(tokens);
                continue;
            }
            if(equal_ignore_case(tokens[0], ".nodeset")){
                parseInitialConditionDirective(tokens, true);
                continue;
            }
            if(equal_ignore_case(tokens[0], ".ic")){
                parseInitialConditionDirective(tokens, false);
                continue;
            }
            if(equal_ignore_case(tokens[0], ".option") ||
               equal_ignore_case(tokens[0], ".options")){
                parseOptionsDirective(tokens);
                continue;
            }
            if(equal_ignore_case(tokens[0], ".print")){
                parsePrintDirective(logicalLine.text);
                continue;
            }
            if(equal_ignore_case(tokens[0], ".title")){
                const std::size_t titleStart = logicalLine.text.find_first_of(" \t");
                if(titleStart == std::string::npos ||
                   trim(logicalLine.text.substr(titleStart)).empty()){
                    throw std::runtime_error(".title requires text");
                }
                title_ = trim(logicalLine.text.substr(titleStart));
                continue;
            }

            throw std::runtime_error(
                "Unsupported control directive: " + tokens[0]
            );
        }

        applyStepLimitOptions();

        if(!analysisPlan_.transient &&
           analysisPlan_.transientPrintRequested){
            throw std::runtime_error(".print tran requires a .tran analysis");
        }

        std::unordered_set<std::string> modelNames;
        modelNames.reserve(topLevelLines.size());
        for(const auto& logicalLine: topLevelLines){
            activeLine = logicalLine.lineNumber;
            const auto& tokens = logicalLine.tokens;
            if(equal_ignore_case(tokens[0], ".model")){
                if(tokens.size() < 2 ||
                   !modelNames.insert(to_lower_copy(tokens[1])).second){
                    throw std::runtime_error(
                        "Duplicate or missing model name in .model"
                    );
                }
                if(!parseModel(circuit, tokens)){
                    return false;
                }
            }
        }

        std::unordered_set<std::string> deviceNames;
        deviceNames.reserve(topLevelLines.size());
        bool foundNonGroundNode = false;
        const auto registerPrimitive = [&](const std::vector<std::string>& tokens){
            if(!deviceNames.insert(to_lower_copy(tokens[0])).second){
                throw std::runtime_error(
                    "Duplicate device name: " + tokens[0]
                );
            }
            if(!parseLine(circuit, tokens)){
                throw std::runtime_error("Unable to parse element: " + tokens[0]);
            }
            foundNonGroundNode = foundNonGroundNode || hasNonGroundNode(tokens);
        };

        for(const auto& logicalLine: topLevelLines){
            activeLine = logicalLine.lineNumber;
            const auto& tokens = logicalLine.tokens;
            if(tokens[0][0] == '.'){
                continue;
            }
            if(std::toupper(static_cast<unsigned char>(tokens[0][0])) == 'X'){
                subcircuits.expandInstance(
                    logicalLine,
                    [&](FlattenedNetlistElement&& flattened){
                        activeLine = flattened.lineNumber;
                        registerPrimitive(flattened.tokens);
                    }
                );
            } else {
                registerPrimitive(tokens);
            }
        }
        if(deviceNames.empty()){
            throw std::runtime_error("Netlist requires at least one element");
        }
        if(!foundNonGroundNode){
            throw std::runtime_error(
                "Netlist requires at least one non-ground node"
            );
        }
    } catch(const SubcircuitError& ex){
        std::cerr << filename_ << ':' << ex.lineNumber()
                  << ": parse error: " << ex.what() << '\n';
        return false;
    } catch(const std::exception& ex){
        std::cerr << filename_ << ':' << activeLine
                  << ": parse error: " << ex.what() << '\n';
        return false;
    }

    return true;
}

bool Parser::parseAnalysisDirective(const std::vector<std::string>& tokens){
    if(equal_ignore_case(tokens[0], ".op")){
        if(tokens.size() != 1){
            throw std::runtime_error(".op does not accept arguments");
        }
        analysisPlan_.operatingPointRequested = true;
        return true;
    }

    if(!equal_ignore_case(tokens[0], ".tran")){
        return true;
    }

    if(analysisPlan_.transient){
        throw std::runtime_error("Multiple .tran directives are not supported");
    }
    if(tokens.size() < 3){
        throw std::runtime_error(".tran requires TSTEP and TSTOP");
    }

    TransientAnalysisConfig config;
    TransientNetlistParameterPresence presence;
    config.outputInterval = parse_spice_number(tokens[1]);
    config.stopTime = parse_spice_number(tokens[2]);

    if(config.outputInterval <= 0.0){
        throw std::runtime_error(".tran TSTEP must be positive");
    }
    if(config.stopTime <= 0.0){
        throw std::runtime_error(".tran TSTOP must be positive");
    }

    std::vector<double> optionalTimes;
    for(std::size_t i = 3; i < tokens.size(); ++i){
        if(equal_ignore_case(tokens[i], "uic")){
            if(config.useInitialConditions){
                throw std::runtime_error(".tran specifies UIC more than once");
            }
            config.useInitialConditions = true;
            presence.useInitialConditions = true;
            continue;
        }
        optionalTimes.push_back(parse_spice_number(tokens[i]));
    }

    if(optionalTimes.size() > 2){
        throw std::runtime_error(".tran accepts at most TSTART and TMAX after TSTEP and TSTOP");
    }
    if(!optionalTimes.empty()){
        config.outputStartTime = optionalTimes[0];
        presence.outputStartTime = true;
        if(config.outputStartTime < 0.0 ||
           config.outputStartTime >= config.stopTime){
            throw std::runtime_error(".tran TSTART must be non-negative and smaller than TSTOP");
        }
    }
    if(optionalTimes.size() == 2){
        config.maximumStep = optionalTimes[1];
        presence.maximumStep = true;
        if(*config.maximumStep <= 0.0){
            throw std::runtime_error(".tran TMAX must be positive");
        }
    }

    analysisPlan_.transient = config;
    analysisPlan_.transientNetlistParameters = presence;
    return true;
}

void Parser::parseInitialConditionDirective(
    const std::vector<std::string>& tokens,
    bool nodeSet
){
    if(tokens.size() < 4){
        throw std::runtime_error(
            std::string(nodeSet ? ".nodeset" : ".ic") +
            " requires at least one V(node)=value expression"
        );
    }

    std::vector<NodeVoltageConstraint>& constraints = nodeSet
        ? analysisPlan_.nodeSets
        : analysisPlan_.initialConditions;
    std::size_t index = 1;
    while(index < tokens.size()){
        if(!equal_ignore_case(tokens[index], "v")){
            throw std::runtime_error(
                std::string(nodeSet ? ".nodeset" : ".ic") +
                " accepts only V(node)=value expressions"
            );
        }
        ++index;
        if(index >= tokens.size() || tokens[index].empty() ||
           tokens[index][0] == '='){
            throw std::runtime_error("Initial-condition voltage is missing a node");
        }

        NodeVoltageConstraint constraint;
        constraint.positiveNode = tokens[index++];
        if(index < tokens.size() && !tokens[index].empty() &&
           tokens[index][0] != '='){
            constraint.negativeNode = tokens[index++];
        }
        if(!readInitialConditionValue(tokens, index, constraint.voltage)){
            throw std::runtime_error(
                "Initial-condition voltage requires =value"
            );
        }
        constraints.push_back(std::move(constraint));
    }
}

void Parser::parseOptionsDirective(const std::vector<std::string>& tokens){
    if(tokens.size() == 1){
        throw std::runtime_error(".options requires at least one name=value option");
    }

    for(std::size_t i = 1; i < tokens.size(); ++i){
        std::string key;
        std::string value;
        if(!read_spice_assignment(tokens, i, key, value)){
            throw std::runtime_error(
                ".options parameters must use name=value syntax"
            );
        }
        if(key != "delmax"){
            throw std::runtime_error("Unsupported .options parameter: " + key);
        }

        const double parsed = parse_spice_number(value);
        if(!std::isfinite(parsed) || parsed <= 0.0){
            throw std::runtime_error(".options DELMAX must be positive");
        }
        // SPICE option cards are mutable global configuration: as in ngspice
        // control-mode `option`, a later assignment supersedes an earlier one.
        analysisPlan_.delmax = parsed;
    }
}

void Parser::applyStepLimitOptions(){
    if(!analysisPlan_.delmax){
        return;
    }

    if(analysisPlan_.transient){
        auto& config = *analysisPlan_.transient;
        if(config.maximumStep){
            // TMAX and DELMAX are independently specified upper bounds.  The
            // smaller one must win so no internal integration interval can
            // exceed either limit.
            config.maximumStep = std::min(
                *config.maximumStep,
                *analysisPlan_.delmax
            );
        } else {
            config.maximumStep = *analysisPlan_.delmax;
        }
    }

    if(analysisPlan_.pseudoTransient){
        // Pseudo-transient analysis advances in pseudo-time, but DELMAX has
        // the same role: it is a global upper bound on every internal step.
        analysisPlan_.pseudoTransient->maximumStep = std::min(
            analysisPlan_.pseudoTransient->maximumStep,
            *analysisPlan_.delmax
        );
        analysisPlan_.pseudoTransient->initialStep = std::min(
            analysisPlan_.pseudoTransient->initialStep,
            analysisPlan_.pseudoTransient->maximumStep
        );
    }
}

void Parser::parsePstranDirective(const std::vector<std::string>& tokens){
    if(analysisPlan_.pseudoTransient){
        throw std::runtime_error("Multiple .pstran directives are not supported");
    }

    PstranAnalysisConfig config;
    std::unordered_set<std::string> seen;
    for(std::size_t i = 1; i < tokens.size(); ++i){
        std::string key;
        std::string value;
        if(!read_spice_assignment(tokens, i, key, value)){
            throw std::runtime_error(
                ".pstran parameters must use name=value syntax"
            );
        }
        if(!seen.insert(key).second){
            throw std::runtime_error("Repeated .pstran parameter: " + key);
        }

        const double parsed = parse_spice_number(value);
        if(key == "convval"){
            config.convergenceValue = parsed;
            config.convergenceValueSpecified = true;
        } else if(key == "initstep"){
            config.initialStep = parsed;
            config.initialStepSpecified = true;
        } else if(key == "minstep"){
            config.minimumStep = parsed;
            config.minimumStepSpecified = true;
        } else if(key == "maxstep"){
            config.maximumStep = parsed;
            config.maximumStepSpecified = true;
        } else if(key == "tau"){
            config.tau = parsed;
            config.tauSpecified = true;
        } else if(key == "vbe0"){
            config.vbe0 = parsed;
            config.vbe0Specified = true;
        } else if(key == "kvgs0"){
            config.kvgs0 = parsed;
            config.kvgs0Specified = true;
        } else if(key == "tauramp"){
            config.tauRamp = parsed;
            config.tauRampSpecified = true;
        } else {
            throw std::runtime_error("Unsupported .pstran parameter: " + key);
        }
    }

    if(!std::isfinite(config.tau) || config.tau < 0.0 ||
       !std::isfinite(config.vbe0) || !std::isfinite(config.kvgs0) ||
       !std::isfinite(config.tauRamp) || config.tauRamp < 0.0){
        throw std::runtime_error(
            ".pstran tau and tauramp must be non-negative; vbe0 and kvgs0 must be finite"
        );
    }

    try {
        config.makePtaConfig().validate();
    } catch(const std::invalid_argument& error) {
        throw std::runtime_error(
            std::string("Invalid .pstran configuration: ") + error.what()
        );
    }

    analysisPlan_.pseudoTransient = config;
}

void Parser::parsePrintDirective(const std::string& line){
    std::istringstream iss(line);
    std::string directive;
    std::string analysis;
    iss >> directive >> analysis;

    if(analysis.empty()){
        throw std::runtime_error(".print requires an analysis type");
    }

    std::vector<PrintVariable>* variables = nullptr;
    if(equal_ignore_case(analysis, "op")){
        analysisPlan_.operatingPointRequested = true;
        analysisPlan_.operatingPointPrintRequested = true;
        variables = &analysisPlan_.operatingPointPrints;
    } else if(equal_ignore_case(analysis, "tran")){
        analysisPlan_.transientPrintRequested = true;
        variables = &analysisPlan_.transientPrints;
    } else {
        throw std::runtime_error(
            "Unsupported .print analysis type: " + analysis
        );
    }

    std::string expressions;
    std::getline(iss, expressions);
    std::size_t pos = 0;
    bool foundVariable = false;

    while(true){
        skipPrintSeparators(expressions, pos);
        if(pos >= expressions.size()){
            break;
        }

        if(startsWithWordIgnoreCase(expressions, pos, "time")){
            if(!equal_ignore_case(analysis, "tran")){
                throw std::runtime_error("time is only valid for .print tran");
            }
            pos += 4;
            foundVariable = true;
            continue;
        }

        const char function = static_cast<char>(
            std::tolower(static_cast<unsigned char>(expressions[pos]))
        );
        if(function != 'v' && function != 'i'){
            throw std::runtime_error(
                "Expected v(node), v(node1,node2), or i(device) in .print"
            );
        }
        ++pos;
        while(pos < expressions.size() &&
              std::isspace(static_cast<unsigned char>(expressions[pos]))){
            ++pos;
        }
        if(pos >= expressions.size() || expressions[pos] != '('){
            throw std::runtime_error("Missing '(' in .print expression");
        }

        const std::size_t close = expressions.find(')', pos + 1);
        if(close == std::string::npos){
            throw std::runtime_error("Missing ')' in .print expression");
        }

        const auto arguments = parsePrintArguments(
            expressions.substr(pos + 1, close - pos - 1)
        );
        PrintVariable variable;
        if(function == 'v'){
            if(arguments.empty() || arguments.size() > 2){
                throw std::runtime_error(
                    "Voltage output requires one or two node names"
                );
            }
            variable.quantity = PrintQuantity::Voltage;
            variable.name = canonicalOutputNode(arguments[0]);
            variable.reference = arguments.size() == 2
                ? canonicalOutputNode(arguments[1])
                : "0";
        } else {
            if(arguments.size() != 1){
                throw std::runtime_error(
                    "Current output requires exactly one device name"
                );
            }
            variable.quantity = PrintQuantity::BranchCurrent;
            variable.name = arguments[0];
            variable.reference.clear();
        }

        appendUniquePrintVariable(*variables, std::move(variable));
        foundVariable = true;
        pos = close + 1;
    }

    if(!foundVariable){
        throw std::runtime_error(".print requires at least one output expression");
    }
}

bool Parser::parseModel(Circuit& circuit, const std::vector<std::string>& tokens){
    if(tokens.size() < 3){
        throw std::runtime_error(".model requires name and type");
    }

    const ModelType type = parseModelType(tokens[2]);
    if(type == ModelType::Unknown){
        throw std::runtime_error("Unsupported model type: " + tokens[2]);
    }

    Model::Parameters parameters;
    parameters.reserve(tokens.size() - 3);

    for(std::size_t i = 3; i < tokens.size(); ++i){
        std::string key;
        std::string value;
        if(!read_spice_assignment(tokens, i, key, value)){
            throw std::runtime_error(
                "Malformed model parameter: " + tokens[i]
            );
        }
        const double parsed = parse_spice_number(value);
        if(type == ModelType::NMOS || type == ModelType::PMOS){
            key = canonicalMosModelParameter(std::move(key));
        }
        parameters.insert_or_assign(std::move(key), parsed);
    }

    for(const auto& parameter: parameters){
        validateModelParameter(type, parameter.first, parameter.second);
    }

    if((type == ModelType::NMOS || type == ModelType::PMOS) &&
       parameters.find("level") != parameters.end() &&
       parameters.at("level") == 3.0 &&
       (parameters.find("gmin") != parameters.end() ||
        parameters.find("rds") != parameters.end())){
        throw std::runtime_error(
            "LEVEL=3 MOSFET models do not accept simulator-specific GMIN or RDS"
        );
    }

    auto model = std::make_unique<Model>(
        to_lower_copy(tokens[1]),
        type,
        std::move(parameters)
    );
    circuit.addModel(std::move(model));
    return true;
}

bool Parser::parseLine(Circuit& circuit, const std::vector<std::string>& tokens){
    if(tokens[0].empty()){
        return true;
    }

    const char type = static_cast<char>(std::toupper(static_cast<unsigned char>(tokens[0][0])));
    switch(type){
        case 'R':
            if(tokens.size() != 4) throw std::runtime_error("Resistor requires exactly two nodes and one value");
            circuit.addDevice<Resistor>(tokens[0], elementNodes(tokens, 1, 2), positiveElementValue(tokens, 3, "Resistor"));
            return true;
        case 'C':
            if(tokens.size() < 4) throw std::runtime_error("Capacitor requires exactly two nodes and one value");
            circuit.addDevice<Capacitor>(
                tokens[0], elementNodes(tokens, 1, 2),
                positiveElementValue(tokens, 3, "Capacitor"),
                parseDynamicInitialCondition(tokens, 4, "Capacitor")
            );
            return true;
        case 'L':
            if(tokens.size() < 4) throw std::runtime_error("Inductor requires exactly two nodes and one value");
            circuit.addDevice<Inductor>(
                tokens[0], elementNodes(tokens, 1, 2),
                positiveElementValue(tokens, 3, "Inductor"),
                parseDynamicInitialCondition(tokens, 4, "Inductor")
            );
            return true;
        case 'V':
            if(tokens.size() < 4) throw std::runtime_error("Bad voltage source line");
            if(sourceValueEnd(tokens, 3) != tokens.size()) throw std::runtime_error("Only a single DC voltage-source value is supported");
            circuit.addDevice<VoltageSource>(tokens[0], elementNodes(tokens, 1, 2), parse_spice_value_token(tokens, 3));
            return true;
        case 'I':
            if(tokens.size() < 4) throw std::runtime_error("Bad current source line");
            if(sourceValueEnd(tokens, 3) != tokens.size()) throw std::runtime_error("Only a single DC current-source value is supported");
            circuit.addDevice<CurrentSource>(tokens[0], elementNodes(tokens, 1, 2), parse_spice_value_token(tokens, 3));
            return true;
        case 'D': {
            if(tokens.size() < 4) throw std::runtime_error("Bad diode line");
            const Model* model = circuit.findModel(to_lower_copy(tokens[3]));
            if(!model) throw std::runtime_error("Unknown diode model: " + tokens[3]);
            if(!model->isDiode()) throw std::runtime_error("Model is not diode type: " + tokens[3]);
            const InstanceValues values = parseInstanceValues(
                tokens,
                4,
                true,
                false,
                true
            );
            circuit.addDevice<Diode>(tokens[0], elementNodes(tokens, 1, 2), model, values.area);
            return true;
        }
        case 'Q': {
            if(tokens.size() < 5) throw std::runtime_error("Bad BJT line");
            const Model* model = circuit.findModel(to_lower_copy(tokens[4]));
            if(!model) throw std::runtime_error("Unknown BJT model: " + tokens[4]);
            if(!model->isBjt()) throw std::runtime_error("Model is not BJT type: " + tokens[4]);
            const InstanceValues values = parseInstanceValues(
                tokens,
                5,
                true,
                false,
                true,
                true
            );
            circuit.addDevice<BJT>(
                tokens[0], elementNodes(tokens, 1, 3), model,
                values.area, values.initialCondition
            );
            return true;
        }
        case 'M': {
            if(tokens.size() < 6) throw std::runtime_error("Bad MOSFET line");
            const Model* model = circuit.findModel(to_lower_copy(tokens[5]));
            if(!model) throw std::runtime_error("Unknown MOSFET model: " + tokens[5]);
            if(!model->isMosfet()) throw std::runtime_error("Model is not MOSFET type: " + tokens[5]);
            const MOSFET::MosInstanceParams values = parseMosInstanceParams(tokens, 6);
            circuit.addDevice<MOSFET>(
                tokens[0],
                elementNodes(tokens, 1, 4),
                model,
                values
            );
            return true;
        }
        default:
            throw std::runtime_error("Unsupported element: " + tokens[0]);
    }
}
