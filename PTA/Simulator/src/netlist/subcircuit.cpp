#include "netlist/subcircuit.hpp"

#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <unordered_set>
#include <utility>

#include "netlist/spice_syntax.hpp"

namespace {
using ParameterLookup = std::function<double(const std::string&)>;

bool isSubcircuitInstance(const std::vector<std::string>& tokens) {
    return !tokens.empty() && !tokens.front().empty() &&
           std::toupper(static_cast<unsigned char>(tokens.front().front())) ==
               'X';
}

bool isParameterMarker(const std::string& token) {
    return equal_ignore_case(token, "params:");
}

bool isAssignmentStart(const std::vector<std::string>& tokens,
                       std::size_t index) {
    std::size_t cursor = index;
    std::string key;
    std::string value;
    return read_spice_assignment(tokens, cursor, key, value);
}

std::size_t elementNodeCount(const std::vector<std::string>& tokens,
                             std::size_t lineNumber) {
    if(tokens.empty() || tokens.front().empty()) {
        throw SubcircuitError(lineNumber, "Empty element statement");
    }

    switch(std::toupper(static_cast<unsigned char>(tokens.front().front()))) {
        case 'Q': return 3;
        case 'M': return 4;
        case 'R':
        case 'C':
        case 'L':
        case 'V':
        case 'I':
        case 'D': return 2;
        default:
            throw SubcircuitError(
                lineNumber,
                "Unsupported element: " + tokens.front()
            );
    }
}

bool isGroundNode(const std::string& canonicalName) {
    return canonicalName == "0" || canonicalName == "gnd";
}

std::string formatParameterValue(double value) {
    if(!std::isfinite(value)) {
        throw std::runtime_error("Parameter expression must be finite");
    }

    std::ostringstream formatted;
    formatted.imbue(std::locale::classic());
    formatted << std::setprecision(std::numeric_limits<double>::max_digits10)
              << value;
    return formatted.str();
}

class ParameterExpressionParser {
public:
    ParameterExpressionParser(std::string expression, ParameterLookup lookup):
        expression_(std::move(expression)),
        lookup_(std::move(lookup)) {}

    double parse() {
        skipWhitespace();
        const double value = parseSum();
        skipWhitespace();
        if(position_ != expression_.size()) {
            throw std::runtime_error(
                "unexpected token at offset " + std::to_string(position_)
            );
        }
        if(!std::isfinite(value)) {
            throw std::runtime_error("expression result must be finite");
        }
        return value;
    }

private:
    double parseSum() {
        double value = parseProduct();
        while(true) {
            skipWhitespace();
            if(consume('+')) {
                value += parseProduct();
            } else if(consume('-')) {
                value -= parseProduct();
            } else {
                return value;
            }
        }
    }

    double parseProduct() {
        double value = parsePower();
        while(true) {
            skipWhitespace();
            if(consume('*')) {
                value *= parsePower();
            } else if(consume('/')) {
                const double divisor = parsePower();
                if(divisor == 0.0) {
                    throw std::runtime_error("division by zero");
                }
                value /= divisor;
            } else {
                return value;
            }
        }
    }

    double parsePower() {
        double value = parseUnary();
        skipWhitespace();
        if(consume('^')) {
            value = std::pow(value, parsePower());
        }
        return value;
    }

    double parseUnary() {
        skipWhitespace();
        if(consume('+')) {
            return parseUnary();
        }
        if(consume('-')) {
            return -parseUnary();
        }
        return parsePrimary();
    }

    double parsePrimary() {
        skipWhitespace();
        if(consume('(')) {
            const double value = parseSum();
            skipWhitespace();
            if(!consume(')')) {
                throw std::runtime_error("missing ')' ");
            }
            return value;
        }
        if(position_ == expression_.size()) {
            throw std::runtime_error("missing operand");
        }

        const unsigned char current =
            static_cast<unsigned char>(expression_[position_]);
        if(std::isdigit(current) || expression_[position_] == '.') {
            return parseNumber();
        }
        if(std::isalpha(current) || expression_[position_] == '_' ||
           expression_[position_] == '$') {
            return lookup_(parseIdentifier());
        }
        throw std::runtime_error(
            "expected number, parameter, or '(' at offset " +
            std::to_string(position_)
        );
    }

    double parseNumber() {
        const std::size_t start = position_;
        while(position_ < expression_.size() &&
              (std::isdigit(static_cast<unsigned char>(expression_[position_])) ||
               expression_[position_] == '.')) {
            ++position_;
        }
        if(position_ < expression_.size() &&
           (expression_[position_] == 'e' || expression_[position_] == 'E')) {
            ++position_;
            if(position_ < expression_.size() &&
               (expression_[position_] == '+' || expression_[position_] == '-')) {
                ++position_;
            }
            const std::size_t exponentStart = position_;
            while(position_ < expression_.size() &&
                  std::isdigit(static_cast<unsigned char>(expression_[position_]))) {
                ++position_;
            }
            if(exponentStart == position_) {
                throw std::runtime_error("malformed exponent");
            }
        }
        while(position_ < expression_.size() &&
              std::isalpha(static_cast<unsigned char>(expression_[position_]))) {
            ++position_;
        }
        return parse_spice_number(expression_.substr(start, position_ - start));
    }

    std::string parseIdentifier() {
        const std::size_t start = position_;
        while(position_ < expression_.size()) {
            const unsigned char current =
                static_cast<unsigned char>(expression_[position_]);
            if(!std::isalnum(current) && expression_[position_] != '_' &&
               expression_[position_] != '$' && expression_[position_] != '.') {
                break;
            }
            ++position_;
        }
        return to_lower_copy(expression_.substr(start, position_ - start));
    }

    bool consume(char expected) {
        if(position_ < expression_.size() && expression_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    void skipWhitespace() {
        while(position_ < expression_.size() &&
              std::isspace(static_cast<unsigned char>(expression_[position_]))) {
            ++position_;
        }
    }

    std::string expression_;
    ParameterLookup lookup_;
    std::size_t position_ = 0;
};

double evaluateParameterExpression(const std::string& expression,
                                   const ParameterLookup& lookup,
                                   std::size_t lineNumber) {
    std::string normalized = trim(expression);
    if(normalized.size() >= 2 && normalized.front() == '{' &&
       normalized.back() == '}') {
        normalized = trim(normalized.substr(1, normalized.size() - 2));
    }
    if(normalized.empty()) {
        throw SubcircuitError(lineNumber, "Empty subcircuit parameter expression");
    }

    try {
        return ParameterExpressionParser(normalized, lookup).parse();
    } catch(const SubcircuitError&) {
        throw;
    } catch(const std::exception& error) {
        throw SubcircuitError(
            lineNumber,
            "Invalid subcircuit parameter expression <" + expression +
            ">: " + error.what()
        );
    }
}

std::string materializeParameterReferences(
    const std::string& token,
    const ParameterLookup& lookup,
    std::size_t lineNumber)
{
    const std::size_t firstOpen = token.find('{');
    if(firstOpen == std::string::npos) {
        const std::string canonical = to_lower_copy(token);
        try {
            return formatParameterValue(lookup(canonical));
        } catch(const std::runtime_error&) {
            return token;
        }
    }

    std::string materialized;
    materialized.reserve(token.size());
    std::size_t start = 0;
    while(start < token.size()) {
        const std::size_t open = token.find('{', start);
        if(open == std::string::npos) {
            materialized.append(token, start, std::string::npos);
            break;
        }
        materialized.append(token, start, open - start);
        const std::size_t close = token.find('}', open + 1);
        if(close == std::string::npos) {
            throw SubcircuitError(
                lineNumber,
                "Unclosed '{' in subcircuit parameter token: " + token
            );
        }
        materialized += formatParameterValue(evaluateParameterExpression(
            token.substr(open + 1, close - open - 1),
            lookup,
            lineNumber
        ));
        start = close + 1;
    }
    return materialized;
}
}

SubcircuitError::SubcircuitError(std::size_t lineNumber, std::string message):
    std::runtime_error(std::move(message)),
    lineNumber_(lineNumber) {}

std::vector<LogicalNetlistLine> SubcircuitLibrary::collect(
    std::vector<LogicalNetlistLine> lines)
{
    definitions_.clear();
    definitions_.reserve(lines.size());

    std::vector<LogicalNetlistLine> topLevelLines;
    topLevelLines.reserve(lines.size());

    Definition currentDefinition;
    bool insideDefinition = false;

    for(auto& logicalLine: lines) {
        const auto& tokens = logicalLine.tokens;
        if(tokens.empty()) {
            continue;
        }

        if(equal_ignore_case(tokens.front(), ".subckt")) {
            if(insideDefinition) {
                throw SubcircuitError(
                    logicalLine.lineNumber,
                    "Nested .subckt definitions are not supported"
                );
            }
            if(tokens.size() < 2) {
                throw SubcircuitError(
                    logicalLine.lineNumber,
                    ".subckt requires a name"
                );
            }

            currentDefinition = {};
            currentDefinition.lineNumber = logicalLine.lineNumber;
            currentDefinition.name = to_lower_copy(tokens[1]);
            currentDefinition.pins.reserve(tokens.size() - 2);
            currentDefinition.pinIndex.reserve(tokens.size() - 2);

            std::size_t parameterStart = tokens.size();
            for(std::size_t i = 2; i < tokens.size(); ++i) {
                if(isParameterMarker(tokens[i]) || isAssignmentStart(tokens, i)) {
                    parameterStart = i;
                    break;
                }

                const std::string pin = to_lower_copy(tokens[i]);
                if(isGroundNode(pin) ||
                   !currentDefinition.pinIndex.emplace(
                       pin,
                       currentDefinition.pins.size()
                   ).second) {
                    throw SubcircuitError(
                        logicalLine.lineNumber,
                        ".subckt pins must be unique non-ground nodes"
                    );
                }
                currentDefinition.pins.push_back(pin);
            }

            if(parameterStart < tokens.size()) {
                if(isParameterMarker(tokens[parameterStart])) {
                    ++parameterStart;
                }
                while(parameterStart < tokens.size()) {
                    std::size_t cursor = parameterStart;
                    std::string key;
                    std::string value;
                    if(!read_spice_assignment(tokens, cursor, key, value)) {
                        throw SubcircuitError(
                            logicalLine.lineNumber,
                            ".subckt parameters must use name=value syntax"
                        );
                    }
                    if(!currentDefinition.defaultParameterExpressions.emplace(
                        std::move(key),
                        std::move(value)
                    ).second) {
                        throw SubcircuitError(
                            logicalLine.lineNumber,
                            "Repeated .subckt parameter"
                        );
                    }
                    parameterStart = cursor + 1;
                }
            }

            insideDefinition = true;
            continue;
        }

        if(equal_ignore_case(tokens.front(), ".ends")) {
            if(!insideDefinition) {
                throw SubcircuitError(
                    logicalLine.lineNumber,
                    ".ends has no matching .subckt"
                );
            }
            if(tokens.size() > 2 ||
               (tokens.size() == 2 &&
                !equal_ignore_case(tokens[1], currentDefinition.name))) {
                throw SubcircuitError(
                    logicalLine.lineNumber,
                    ".ends name does not match its .subckt definition"
                );
            }

            const std::string name = currentDefinition.name;
            if(!definitions_.emplace(name, std::move(currentDefinition)).second) {
                throw SubcircuitError(
                    logicalLine.lineNumber,
                    "Duplicate .subckt definition: " + name
                );
            }
            insideDefinition = false;
            continue;
        }

        if(insideDefinition) {
            currentDefinition.body.push_back(std::move(logicalLine));
        } else {
            topLevelLines.push_back(std::move(logicalLine));
        }
    }

    if(insideDefinition) {
        throw SubcircuitError(
            currentDefinition.lineNumber,
            ".subckt is missing a matching .ends"
        );
    }

    const ParameterValues noOuterParameters;
    const ParameterExpressions noOverrides;
    for(auto& definitionEntry: definitions_) {
        Definition& definition = definitionEntry.second;
        definition.defaultParameterValues = bindParameters(
            definition,
            noOverrides,
            noOuterParameters,
            definition.lineNumber
        );
    }

    return topLevelLines;
}

SubcircuitLibrary::InstanceInvocation SubcircuitLibrary::parseInvocation(
    const std::vector<std::string>& tokens,
    std::size_t sourceLine) const
{
    if(tokens.size() < 2) {
        throw SubcircuitError(
            sourceLine,
            "Subcircuit instance requires a subcircuit name"
        );
    }

    std::size_t parameterStart = tokens.size();
    for(std::size_t i = 1; i < tokens.size(); ++i) {
        if(isParameterMarker(tokens[i]) || isAssignmentStart(tokens, i)) {
            parameterStart = i;
            break;
        }
    }
    if(parameterStart == 1) {
        throw SubcircuitError(
            sourceLine,
            "Subcircuit instance requires a subcircuit name before parameters"
        );
    }

    InstanceInvocation invocation;
    invocation.subcircuitNameIndex = parameterStart - 1;
    invocation.subcircuitName = to_lower_copy(
        tokens[invocation.subcircuitNameIndex]
    );

    if(parameterStart < tokens.size() && isParameterMarker(tokens[parameterStart])) {
        ++parameterStart;
    }
    while(parameterStart < tokens.size()) {
        std::size_t cursor = parameterStart;
        std::string key;
        std::string value;
        if(!read_spice_assignment(tokens, cursor, key, value)) {
            throw SubcircuitError(
                sourceLine,
                "Subcircuit instance parameters must use name=value syntax"
            );
        }
        if(!invocation.parameterExpressions.emplace(
            std::move(key),
            std::move(value)
        ).second) {
            throw SubcircuitError(
                sourceLine,
                "Repeated subcircuit instance parameter"
            );
        }
        parameterStart = cursor + 1;
    }

    return invocation;
}

SubcircuitLibrary::ParameterValues SubcircuitLibrary::bindParameters(
    const Definition& definition,
    const ParameterExpressions& overrides,
    const ParameterValues& outerParameters,
    std::size_t sourceLine) const
{
    for(const auto& overrideEntry: overrides) {
        if(definition.defaultParameterExpressions.find(overrideEntry.first) ==
           definition.defaultParameterExpressions.end()) {
            throw SubcircuitError(
                sourceLine,
                "Unknown subcircuit parameter " + overrideEntry.first +
                " for " + definition.name
            );
        }
    }
    if(overrides.empty() &&
       definition.defaultParameterValues.size() ==
           definition.defaultParameterExpressions.size()) {
        return definition.defaultParameterValues;
    }

    ParameterValues values;
    values.reserve(definition.defaultParameterExpressions.size());
    std::unordered_set<std::string> resolving;

    std::function<double(const std::string&)> resolve;
    resolve = [&](const std::string& name) {
        const auto existing = values.find(name);
        if(existing != values.end()) {
            return existing->second;
        }

        const auto override = overrides.find(name);
        const auto defaultExpression =
            definition.defaultParameterExpressions.find(name);
        if(override == overrides.end() &&
           defaultExpression == definition.defaultParameterExpressions.end()) {
            const auto outer = outerParameters.find(name);
            if(outer != outerParameters.end()) {
                return outer->second;
            }
            throw std::runtime_error("Unknown parameter: " + name);
        }
        if(!resolving.insert(name).second) {
            throw std::runtime_error("Circular parameter dependency: " + name);
        }

        const std::string& expression = override != overrides.end()
            ? override->second
            : defaultExpression->second;
        const double value = evaluateParameterExpression(
            expression,
            resolve,
            sourceLine
        );
        resolving.erase(name);
        values.emplace(name, value);
        return value;
    };

    for(const auto& defaultEntry: definition.defaultParameterExpressions) {
        resolve(defaultEntry.first);
    }
    return values;
}

void SubcircuitLibrary::expandInstance(const LogicalNetlistLine& instance,
                                       const ElementVisitor& visitor) const {
    if(!isSubcircuitInstance(instance.tokens)) {
        throw SubcircuitError(
            instance.lineNumber,
            "Expected a subcircuit instance"
        );
    }
    const ParameterValues noOuterParameters;
    expand(
        instance.tokens,
        to_lower_copy(instance.tokens.front()),
        instance.lineNumber,
        noOuterParameters,
        visitor
    );
}

void SubcircuitLibrary::expand(const std::vector<std::string>& instanceTokens,
                               const std::string& instancePath,
                               std::size_t sourceLine,
                               const ParameterValues& outerParameters,
                               const ElementVisitor& visitor) const {
    const InstanceInvocation invocation = parseInvocation(
        instanceTokens,
        sourceLine
    );
    const auto definitionIt = definitions_.find(invocation.subcircuitName);
    if(definitionIt == definitions_.end()) {
        throw SubcircuitError(
            sourceLine,
            "Unknown subcircuit: " +
            instanceTokens[invocation.subcircuitNameIndex]
        );
    }

    const Definition& definition = definitionIt->second;
    const std::size_t actualNodeCount = invocation.subcircuitNameIndex - 1;
    if(actualNodeCount != definition.pins.size()) {
        throw SubcircuitError(
            sourceLine,
            "Subcircuit " +
            instanceTokens[invocation.subcircuitNameIndex] + " expects " +
            std::to_string(definition.pins.size()) + " nodes but received " +
            std::to_string(actualNodeCount)
        );
    }

    const ParameterValues parameters = bindParameters(
        definition,
        invocation.parameterExpressions,
        outerParameters,
        sourceLine
    );
    const ParameterLookup lookup = [&](const std::string& name) {
        const auto parameter = parameters.find(name);
        if(parameter == parameters.end()) {
            throw std::runtime_error("Unknown parameter: " + name);
        }
        return parameter->second;
    };

    for(const auto& bodyLine: definition.body) {
        if(bodyLine.tokens.empty() || bodyLine.tokens.front().empty()) {
            continue;
        }
        if(bodyLine.tokens.front().front() == '.') {
            throw SubcircuitError(
                bodyLine.lineNumber,
                "Control directives inside .subckt are not supported: " +
                bodyLine.tokens.front()
            );
        }

        std::vector<std::string> flattened = bodyLine.tokens;
        const bool nestedInstance = isSubcircuitInstance(flattened);
        const std::size_t nodeCount = nestedInstance
            ? parseInvocation(flattened, bodyLine.lineNumber)
                  .subcircuitNameIndex - 1
            : elementNodeCount(flattened, bodyLine.lineNumber);
        if(flattened.size() <= nodeCount) {
            throw SubcircuitError(
                bodyLine.lineNumber,
                "Malformed element in .subckt: " + flattened.front()
            );
        }

        for(std::size_t i = 1; i <= nodeCount; ++i) {
            const std::string node = to_lower_copy(flattened[i]);
            if(isGroundNode(node)) {
                flattened[i] = "0";
                continue;
            }

            const auto pin = definition.pinIndex.find(node);
            flattened[i] = pin == definition.pinIndex.end()
                ? instancePath + ":" + node
                : instanceTokens[pin->second + 1];
        }

        if(nestedInstance) {
            expand(
                flattened,
                instancePath + "/" + to_lower_copy(flattened.front()),
                bodyLine.lineNumber,
                parameters,
                visitor
            );
        } else {
            for(std::size_t i = nodeCount + 1; i < flattened.size(); ++i) {
                flattened[i] = materializeParameterReferences(
                    flattened[i],
                    lookup,
                    bodyLine.lineNumber
                );
            }
            // Keep the designator first because primitive parsing dispatches on
            // it; the hierarchy suffix makes flattened instance names unique.
            flattened.front() += "@" + instancePath;
            visitor({bodyLine.lineNumber, std::move(flattened)});
        }
    }
}
