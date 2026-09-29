#include "config/config_loader.hpp"

#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace simulator::config {
namespace {

std::filesystem::path makeAbsolutePath(
    const std::filesystem::path& path
){
    std::error_code error;
    const std::filesystem::path absolute =
        std::filesystem::absolute(path, error);

    if(error){
        throw std::runtime_error(
            "Cannot resolve configuration path <" + path.string() +
            ">: " + error.message()
        );
    }

    return absolute.lexically_normal();
}

std::string describePath(const std::filesystem::path& path){
    return "<" + path.string() + ">";
}

LoadedConfig readConfigFile(const std::filesystem::path& path){
    std::ifstream input(path);
    if(!input.is_open()){
        throw std::runtime_error(
            "Cannot read configuration file " + describePath(path)
        );
    }

    nlohmann::json document;
    try {
        input >> document;
    } catch(const nlohmann::json::exception& error) {
        throw std::runtime_error(
            "Invalid json in configuration file " + describePath(path) +
            ": " + error.what()
        );
    }

    if(input.bad()){
        throw std::runtime_error(
            "I/O error while reading configuration file " +
            describePath(path)
        );
    }

    if(!document.is_object()){
        throw std::runtime_error(
            "Configuration root must be a json object: " +
            describePath(path)
        );
    }

    return LoadedConfig{
        true,
        path,
        std::move(document)
    };
}
}  // namespace

std::vector<std::filesystem::path>
configSearchCandidates(
    const ConfigSearchOptions& options,
    const std::filesystem::path& workingDirectory
){
    const std::filesystem::path absoluteWorkingDirectory =
        makeAbsolutePath(workingDirectory);

    if(options.explicitPath){
        if(options.explicitPath->empty()){
            throw std::runtime_error(
                "Explicit configuration path must not be empty"
            );
        }

        std::filesystem::path candidate = *options.explicitPath;
        if(candidate.is_relative()){
            candidate = absoluteWorkingDirectory / candidate;
        }

        return {makeAbsolutePath(candidate)};
    }

    std::error_code error;
    if(!std::filesystem::is_directory(absoluteWorkingDirectory, error)){
        const std::string reason = error ?
            ": " + error.message() : "";

        throw std::runtime_error(
            "Configuration search directory is invalid " +
            describePath(absoluteWorkingDirectory) + reason
        );
    }

    std::vector<std::filesystem::path> candidates;
    std::filesystem::path directory = absoluteWorkingDirectory;

    for(std::size_t level = 0; ; ++level){
        candidates.push_back(directory / kDefaultConfigFilename);

        if(level >= options.parentSearchLimit){
            break;
        }

        const std::filesystem::path parent = directory.parent_path();
        if(parent == directory){
            break;
        }

        directory = parent;
    }

    return candidates;
}

LoadedConfig loadConfig(
    const ConfigSearchOptions& options,
    const std::filesystem::path& workingDirectory
){
    const std::vector<std::filesystem::path> candidates =
        configSearchCandidates(options, workingDirectory);

    for(const auto& candidate: candidates){
        std::error_code error;
        const bool exists = std::filesystem::exists(candidate, error);

        if(error){
            throw std::runtime_error(
                "Cannot access configuration file " +
                describePath(candidate) + ": " + error.message()
            );
        }

        if(!exists){
            if(options.explicitPath){
                throw std::runtime_error(
                    "Configuration file not found: " +
                    describePath(candidate)
                );
            }
            continue;
        }

        error.clear();
        const bool isRegularFile =
            std::filesystem::is_regular_file(candidate, error);

        if(error){
            throw std::runtime_error(
                "Cannot inspect configuration file " +
                describePath(candidate) + ": " + error.message()
            );
        }

        if(!isRegularFile){
            throw std::runtime_error(
                "Configuration path is not a regular file: " +
                describePath(candidate)
            );
        }

        return readConfigFile(candidate);
    }

    // 自动查找失败，保持现有默认参数。
    return LoadedConfig{
        false,
        {},
        nlohmann::json::object()
    };
}

}  // namespace simulator::config
