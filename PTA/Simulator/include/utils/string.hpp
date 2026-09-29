#pragma once

#include <cctype>
#include <string>

inline std::string trim(const std::string& text){
    const auto first = text.find_first_not_of(" \t\r\n");
    if(first == std::string::npos) return {};

    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

inline std::string to_lower_copy(std::string text){
    for(char& character: text){
        character = static_cast<char>(
            std::tolower(static_cast<unsigned char>(character))
        );
    }
    return text;
}

inline bool equal_ignore_case(const std::string& left,
                              const std::string& right){
    if(left.size() != right.size()) return false;

    for(std::size_t i = 0; i < left.size(); ++i){
        if(std::tolower(static_cast<unsigned char>(left[i])) !=
           std::tolower(static_cast<unsigned char>(right[i]))){
            return false;
        }
    }
    return true;
}
