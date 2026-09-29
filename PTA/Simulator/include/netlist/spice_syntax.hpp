#pragma once

#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "utils/string.hpp"

inline std::string strip_spice_comment(const std::string& line){
    std::size_t pos = line.find_first_of(";$");
    const std::size_t slashPos = line.find("//");
    if(slashPos != std::string::npos &&
       (pos == std::string::npos || slashPos < pos)){
        pos = slashPos;
    }
    return pos == std::string::npos ? line : line.substr(0, pos);
}

inline std::vector<std::string> tokenize_spice_line(const std::string& line){
    std::vector<std::string> tokens;
    std::string token;
    token.reserve(line.size());

    bool insideBraces = false;
    const auto flushToken = [&] {
        if(!token.empty()){
            tokens.push_back(std::move(token));
            token.clear();
        }
    };

    for(char c: line){
        if(c == '{'){
            insideBraces = true;
            token.push_back(c);
            continue;
        }
        if(c == '}'){
            token.push_back(c);
            insideBraces = false;
            continue;
        }
        if(!insideBraces &&
           (std::isspace(static_cast<unsigned char>(c)) ||
            c == '(' || c == ')' || c == ',')){
            flushToken();
            continue;
        }
        token.push_back(c);
    }
    flushToken();
    return tokens;
}

inline double parse_spice_number(const std::string& text){
    std::size_t position = 0;
    if(position < text.size() &&
       (text[position] == '+' || text[position] == '-')){
        ++position;
    }

    const auto consumeDigits = [&text, &position] {
        const std::size_t begin = position;
        while(position < text.size() &&
              text[position] >= '0' && text[position] <= '9'){
            ++position;
        }
        return position != begin;
    };

    const bool hasIntegerDigits = consumeDigits();
    bool hasFractionDigits = false;
    if(position < text.size() && text[position] == '.'){
        ++position;
        hasFractionDigits = consumeDigits();
    }
    if(!hasIntegerDigits && !hasFractionDigits){
        throw std::runtime_error("Invalid SPICE number: " + text);
    }

    if(position < text.size() &&
       (text[position] == 'e' || text[position] == 'E')){
        ++position;
        if(position < text.size() &&
           (text[position] == '+' || text[position] == '-')){
            ++position;
        }
        if(!consumeDigits()){
            throw std::runtime_error("Invalid SPICE number: " + text);
        }
    }

    const std::size_t suffixStart = position;
    while(position < text.size() &&
          ((text[position] >= 'a' && text[position] <= 'z') ||
           (text[position] >= 'A' && text[position] <= 'Z'))){
        ++position;
    }
    if(position != text.size()){
        throw std::runtime_error("Invalid SPICE number: " + text);
    }

    errno = 0;
    char* parsedEnd = nullptr;
    const double value = std::strtod(text.c_str(), &parsedEnd);
    if(parsedEnd != text.c_str() + suffixStart || errno == ERANGE ||
       !std::isfinite(value)){
        throw std::runtime_error("SPICE number must be finite");
    }

    const auto lowerSuffixCharacter = [&text, suffixStart](std::size_t index){
        return static_cast<char>(std::tolower(
            static_cast<unsigned char>(text[suffixStart + index])
        ));
    };
    const std::size_t suffixLength = text.size() - suffixStart;
    const auto suffixStartsWith = [&](const char* prefix){
        std::size_t index = 0;
        while(prefix[index] != '\0'){
            if(index >= suffixLength ||
               lowerSuffixCharacter(index) != prefix[index]){
                return false;
            }
            ++index;
        }
        return true;
    };

    auto scaled = [value](double multiplier){
        const double result = value * multiplier;
        if(!std::isfinite(result)){
            throw std::runtime_error("SPICE number must be finite");
        }
        return result;
    };

    if(suffixLength == 0) return scaled(1.0);

    if(suffixStartsWith("meg")) return scaled(1e6);
    if(suffixStartsWith("mil")) return scaled(25.4e-6);

    switch(lowerSuffixCharacter(0)){
        case 'a': return scaled(1e-18);
        case 'f': return scaled(1e-15);
        case 'p': return scaled(1e-12);
        case 'n': return scaled(1e-9);
        case 'u': return scaled(1e-6);
        case 'm': return scaled(1e-3);
        case 'k': return scaled(1e3);
        case 'g': return scaled(1e9);
        case 't': return scaled(1e12);
        default: return scaled(1.0);
    }
}

inline bool read_spice_assignment(const std::vector<std::string>& tokens,
                                  std::size_t& i,
                                  std::string& key,
                                  std::string& value){
    const std::string& token = tokens[i];
    const std::size_t eq = token.find('=');

    if(eq != std::string::npos){
        key = to_lower_copy(token.substr(0, eq));
        value = token.substr(eq + 1);
        if(value.empty() && i + 1 < tokens.size()){
            value = tokens[++i];
        }
        return !key.empty() && !value.empty();
    }

    if(i + 1 < tokens.size() && tokens[i + 1] == "="){
        if(i + 2 >= tokens.size()){
            return false;
        }
        key = to_lower_copy(tokens[i]);
        value = tokens[i + 2];
        i += 2;
        return !key.empty() && !value.empty();
    }

    if(i + 1 < tokens.size() && tokens[i + 1].size() > 1 &&
       tokens[i + 1][0] == '='){
        key = to_lower_copy(tokens[i]);
        value = tokens[i + 1].substr(1);
        ++i;
        return !key.empty() && !value.empty();
    }

    return false;
}

inline double parse_spice_value_token(const std::vector<std::string>& tokens,
                                      std::size_t first){
    if(first >= tokens.size()){
        throw std::runtime_error("Missing value");
    }

    if(equal_ignore_case(tokens[first], "dc")){
        std::size_t valueIndex = first + 1;
        bool separatedEquals = false;
        if(valueIndex < tokens.size() && tokens[valueIndex] == "="){
            separatedEquals = true;
            ++valueIndex;
        }
        if(valueIndex >= tokens.size()){
            throw std::runtime_error("Missing DC value");
        }
        const std::string& value = tokens[valueIndex];
        return parse_spice_number(
            !separatedEquals && value.size() > 1 && value[0] == '='
                ? value.substr(1)
                : value
        );
    }

    const std::string dcPrefix = "dc=";
    const std::string token = to_lower_copy(tokens[first]);
    if(token == dcPrefix){
        if(first + 1 >= tokens.size()){
            throw std::runtime_error("Missing DC value");
        }
        return parse_spice_number(tokens[first + 1]);
    }
    if(token.rfind(dcPrefix, 0) == 0){
        return parse_spice_number(token.substr(dcPrefix.size()));
    }

    return parse_spice_number(tokens[first]);
}

inline double parse_spice_named_value(const std::vector<std::string>& tokens,
                                      const std::string& key,
                                      double fallback){
    const std::string wanted = to_lower_copy(key);
    for(std::size_t i = 0; i < tokens.size(); ++i){
        std::string found;
        std::string value;
        if(read_spice_assignment(tokens, i, found, value) && found == wanted){
            return parse_spice_number(value);
        }
    }
    return fallback;
}
