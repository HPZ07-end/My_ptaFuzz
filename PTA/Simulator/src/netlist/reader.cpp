#include "netlist/reader.hpp"

#include <fstream>
#include <iostream>
#include <utility>

#include "netlist/spice_syntax.hpp"

NetlistReader::NetlistReader(std::string filename):
    filename_(std::move(filename)) {}

bool NetlistReader::read(NetlistSource& source) const {
    std::ifstream file(filename_);
    if(!file) {
        std::cerr << "Cannot open input netlist <" << filename_ << ">\n";
        return false;
    }

    source = {};
    std::string line;
    if(!std::getline(file, line)) {
        std::cerr << "Input netlist is empty <" << filename_ << ">\n";
        return false;
    }
    source.title = trim(line);
    if(source.title.empty()) {
        source.title = "Untitled circuit";
    }

    std::size_t lineNumber = 1;
    bool endFound = false;
    while(std::getline(file, line)) {
        ++lineNumber;

        line = trim(strip_spice_comment(line));
        if(line.empty() || line.front() == '*') {
            continue;
        }

        if(endFound) {
            std::cerr << filename_ << ':' << lineNumber
                      << ": .end must be the last netlist statement\n";
            return false;
        }

        if(line.front() == '+') {
            if(source.lines.empty()) {
                std::cerr << filename_ << ':' << lineNumber
                          << ": continuation line has no previous statement\n";
                return false;
            }
            source.lines.back().text += ' ' + trim(line.substr(1));
            source.lines.back().tokens =
                tokenize_spice_line(source.lines.back().text);
            continue;
        }

        std::vector<std::string> tokens = tokenize_spice_line(line);
        if(tokens.empty()) {
            continue;
        }
        if(equal_ignore_case(tokens.front(), ".end")) {
            if(tokens.size() != 1) {
                std::cerr << filename_ << ':' << lineNumber
                          << ": .end does not accept arguments\n";
                return false;
            }
            endFound = true;
            continue;
        }
        source.lines.push_back({lineNumber, line, std::move(tokens)});
    }

    if(file.bad()) {
        std::cerr << "Failed while reading input netlist <" << filename_ << ">\n";
        return false;
    }
    if(!endFound) {
        std::cerr << filename_ << ": missing required .end directive\n";
        return false;
    }

    for(auto& logicalLine: source.lines) {
        const std::string& directive = logicalLine.tokens.front();
        if(!equal_ignore_case(directive, ".print") &&
           !equal_ignore_case(directive, ".title")) {
            logicalLine.text.clear();
        }
    }

    return true;
}
