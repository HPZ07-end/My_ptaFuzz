#include "io/output_files.hpp"

#include <array>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

struct StagedOutput {
    std::filesystem::path destination;
    std::filesystem::path temporary;
    std::filesystem::path backup;
    const char* description = nullptr;
    bool removeOnly = false;
    bool committed = false;
};

struct PendingOutput {
    std::filesystem::path destination;
    std::string_view content;
    const char* description = nullptr;
    bool removeOnly = false;
};

std::filesystem::path normalizedPath(const std::filesystem::path& path){
    std::error_code error;
    std::filesystem::path normalized =
        std::filesystem::weakly_canonical(path, error);
    if(!error){
        return normalized;
    }

    error.clear();
    normalized = std::filesystem::absolute(path, error);
    return error
        ? std::filesystem::path(path).lexically_normal()
        : normalized.lexically_normal();
}

bool pathsReferToSameFile(const std::filesystem::path& left,
                          const std::filesystem::path& right){
    if(normalizedPath(left) == normalizedPath(right)){
        return true;
    }

    std::error_code error;
    const bool equivalent = std::filesystem::equivalent(left, right, error);
    return !error && equivalent;
}

std::filesystem::path pathWithSuffix(const std::filesystem::path& stem,
                                     const char* suffix){
    std::filesystem::path result = stem;
    result += suffix;
    return result;
}

std::filesystem::path temporaryOutputPath(
    const std::filesystem::path& destination)
{
    std::filesystem::path directory = destination.parent_path();
    if(directory.empty()){
        directory = ".";
    }

    static unsigned long long counter = 0;
    for(int attempt = 0; attempt < 100; ++attempt){
        const auto ticks = std::chrono::steady_clock::now()
            .time_since_epoch().count();
        const auto candidate = directory /
            (".spice-tmp-" + std::to_string(ticks) +
             "-" + std::to_string(++counter));
        std::error_code error;
        if(!std::filesystem::exists(candidate, error) && !error){
            return candidate;
        }
    }
    throw std::runtime_error(
        "Cannot allocate a temporary output beside <" +
        destination.string() + ">"
    );
}
void removeTemporaryOutput(const std::filesystem::path& path){
    if(path.empty()){
        return;
    }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void discardStagedOutputs(std::vector<StagedOutput>& outputs){
    for(auto& output: outputs){
        removeTemporaryOutput(output.temporary);
        removeTemporaryOutput(output.backup);
        output.temporary.clear();
        output.backup.clear();
    }
}

bool stageFile(const std::filesystem::path& path,
               std::string_view content,
               const char* description,
               StagedOutput& staged,
               std::ostream& error){
    staged.destination = path;
    staged.description = description;

    std::error_code statusError;
    if(std::filesystem::exists(staged.destination, statusError) &&
       !std::filesystem::is_regular_file(staged.destination, statusError)){
        error << "Cannot replace non-regular " << description << " <"
              << path.string() << ">\n";
        return false;
    }
    if(statusError){
        error << "Cannot inspect " << description << " <" << path.string()
              << ">: " << statusError.message() << '\n';
        return false;
    }

    try {
        staged.temporary = temporaryOutputPath(staged.destination);
    } catch(const std::exception& exception){
        error << exception.what() << '\n';
        return false;
    }

    std::ofstream output(
        staged.temporary,
        std::ios::out | std::ios::trunc | std::ios::binary
    );
    if(!output){
        error << "Cannot open " << description << " <" << path.string()
              << ">\n";
        removeTemporaryOutput(staged.temporary);
        staged.temporary.clear();
        return false;
    }

    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    output.flush();
    output.close();
    if(!output){
        error << "Failed while writing " << description
              << " <" << path.string() << ">\n";
        removeTemporaryOutput(staged.temporary);
        staged.temporary.clear();
        return false;
    }
    return true;
}

bool stageRemoval(const std::filesystem::path& path,
                  const char* description,
                  StagedOutput& staged){
    staged.destination = path;
    staged.description = description;
    staged.removeOnly = true;
    return true;
}

bool commitStagedOutputs(std::vector<StagedOutput>& outputs,
                         std::ostream& error){
    const auto rollback = [&outputs, &error](){
        bool restored = true;
        for(auto& output: outputs){
            if(output.committed){
                std::error_code removeError;
                std::filesystem::remove(output.destination, removeError);
                if(removeError){
                    error << "Cannot remove partially committed "
                          << output.description << " <"
                          << output.destination.string() << ">: "
                          << removeError.message() << '\n';
                    restored = false;
                }
                output.committed = false;
            }
        }
        for(auto& output: outputs){
            if(output.backup.empty()){
                continue;
            }
            std::error_code restoreError;
            std::filesystem::rename(
                output.backup,
                output.destination,
                restoreError
            );
            if(restoreError){
                error << "Cannot restore previous " << output.description
                      << " <" << output.destination.string() << ">: "
                      << restoreError.message() << '\n';
                restored = false;
            } else {
                output.backup.clear();
            }
        }
        for(auto& output: outputs){
            removeTemporaryOutput(output.temporary);
            output.temporary.clear();
        }
        return restored;
    };

    for(auto& output: outputs){
        std::error_code existsError;
        const bool exists = std::filesystem::exists(
            output.destination,
            existsError
        );
        if(existsError){
            error << "Cannot inspect " << output.description << " <"
                  << output.destination.string() << ">: "
                  << existsError.message() << '\n';
            rollback();
            return false;
        }
        if(!exists){
            continue;
        }

        std::error_code typeError;
        if(!std::filesystem::is_regular_file(output.destination, typeError) ||
           typeError){
            error << "Cannot replace non-regular " << output.description
                  << " <" << output.destination.string() << ">\n";
            rollback();
            return false;
        }

        try {
            output.backup = temporaryOutputPath(output.destination);
        } catch(const std::exception& exception){
            error << exception.what() << '\n';
            rollback();
            return false;
        }

        std::error_code backupError;
        std::filesystem::rename(
            output.destination,
            output.backup,
            backupError
        );
        if(backupError){
            error << "Cannot preserve previous " << output.description
                  << " <" << output.destination.string() << ">: "
                  << backupError.message() << '\n';
            output.backup.clear();
            rollback();
            return false;
        }
    }

    for(auto& output: outputs){
        if(output.removeOnly){
            continue;
        }
        std::error_code collisionError;
        if(std::filesystem::exists(output.destination, collisionError)){
            error << "Output destination appeared during commit <"
                  << output.destination.string()
                  << ">; possible path alias or concurrent writer\n";
            rollback();
            return false;
        }
        if(collisionError){
            error << "Cannot inspect output destination <"
                  << output.destination.string() << ">: "
                  << collisionError.message() << '\n';
            rollback();
            return false;
        }

        std::error_code commitError;
        std::filesystem::rename(
            output.temporary,
            output.destination,
            commitError
        );
        if(commitError){
            error << "Cannot replace " << output.description << " <"
                  << output.destination.string() << ">: "
                  << commitError.message() << '\n';
            rollback();
            return false;
        }
        output.temporary.clear();
        output.committed = true;
    }

    // Remove stale artifacts before deleting ordinary backups.  If removal
    // fails, rollback can still restore every pre-run file, including the
    // stale artifact that was moved aside for removal.
    for(auto& output: outputs){
        if(!output.removeOnly || output.backup.empty()){
            continue;
        }

        std::error_code removalError;
        const bool removed = std::filesystem::remove(
            output.backup,
            removalError
        );
        if(removalError || !removed){
            error << "Cannot remove stale " << output.description << " <"
                  << output.destination.string() << ">";
            if(removalError){
                error << ": " << removalError.message();
            }
            error << '\n';
            rollback();
            return false;
        }
        output.backup.clear();
    }

    for(auto& output: outputs){
        removeTemporaryOutput(output.backup);
        output.backup.clear();
        output.committed = false;
    }
    return true;
}

bool writeOutputsAtomically(const std::vector<PendingOutput>& pendingOutputs,
                            std::ostream& error){
    std::vector<StagedOutput> stagedOutputs;
    stagedOutputs.reserve(pendingOutputs.size());

    for(const auto& pending: pendingOutputs){
        StagedOutput staged;
        const bool stagedSuccessfully = pending.removeOnly
            ? stageRemoval(
                pending.destination,
                pending.description,
                staged
            )
            : stageFile(
                pending.destination,
                pending.content,
                pending.description,
                staged,
                error
            );
        if(!stagedSuccessfully){
            discardStagedOutputs(stagedOutputs);
            return false;
        }
        stagedOutputs.push_back(std::move(staged));
    }

    return commitStagedOutputs(stagedOutputs, error);
}

}  // namespace

OutputBundlePaths OutputBundlePaths::derive(
    const std::filesystem::path& inputPath,
    const std::optional<std::filesystem::path>& outputRoot
){
    OutputBundlePaths paths;
    const std::filesystem::path stem = inputPath.filename().stem();
    paths.directory = outputRoot
        ? *outputRoot / stem
        : inputPath.parent_path() / stem;
    paths.listingPath = paths.directory / pathWithSuffix(stem, ".out");
    paths.rawPath = paths.directory / pathWithSuffix(stem, ".raw");
    paths.errorPath = paths.directory / pathWithSuffix(stem, ".err");
    paths.reportPath = paths.directory / pathWithSuffix(stem, ".solve.txt");
    paths.ptaTracePath = paths.directory / pathWithSuffix(stem, ".pta.jsonl");
    return paths;
}

bool OutputBundlePaths::validate(const std::filesystem::path& inputPath,
                                 std::ostream& error) const{
    if(inputPath.empty() || inputPath.filename().empty()){
        error << "Cannot derive output bundle from an empty input filename\n";
        return false;
    }
    if(directory.empty()){
        error << "Output bundle directory must not be empty\n";
        return false;
    }
    if(pathsReferToSameFile(directory, inputPath)){
        error << "Input netlist and output bundle directory must be different\n";
        return false;
    }

    std::error_code directoryError;
    const bool directoryExists = std::filesystem::exists(
        directory,
        directoryError
    );
    if(directoryError){
        error << "Cannot inspect output bundle directory <"
              << directory.string() << ">: "
              << directoryError.message() << '\n';
        return false;
    }
    if(directoryExists){
        std::error_code typeError;
        if(!std::filesystem::is_directory(directory, typeError) || typeError){
            error << "Output bundle path is not a directory <"
                  << directory.string() << ">\n";
            return false;
        }
    }

    const std::array<std::pair<const std::filesystem::path*, const char*>, 5>
        outputs = {{
            {&listingPath, "listing output"},
            {&rawPath, "raw output"},
            {&errorPath, "error log"},
            {&reportPath, "solve report"},
            {&ptaTracePath, "PTA trace"}
        }};
    const std::filesystem::path normalizedDirectory = normalizedPath(directory);

    for(const auto& output: outputs){
        const std::filesystem::path& path = *output.first;
        const std::filesystem::path filename = path.filename();
        if(path.empty() || filename.empty() || filename == "." ||
           filename == ".."){
            error << "Invalid " << output.second << " path <"
                  << path.string() << ">\n";
            return false;
        }
        if(normalizedPath(path.parent_path()) != normalizedDirectory){
            error << "The " << output.second
                  << " must be a direct child of output bundle directory <"
                  << directory.string() << ">\n";
            return false;
        }
        if(pathsReferToSameFile(path, inputPath)){
            error << "Input netlist and " << output.second
                  << " must be different files\n";
            return false;
        }

        std::error_code statusError;
        const bool exists = std::filesystem::exists(path, statusError);
        if(statusError){
            error << "Cannot inspect " << output.second << " <"
                  << path.string() << ">: " << statusError.message() << '\n';
            return false;
        }
        if(exists){
            std::error_code typeError;
            if(!std::filesystem::is_regular_file(path, typeError) || typeError){
                error << "Cannot replace non-regular " << output.second << " <"
                      << path.string() << ">\n";
                return false;
            }
        }
    }

    for(std::size_t left = 0; left < outputs.size(); ++left){
        for(std::size_t right = left + 1; right < outputs.size(); ++right){
            if(pathsReferToSameFile(*outputs[left].first, *outputs[right].first)){
                error << "Output bundle paths for " << outputs[left].second
                      << " and " << outputs[right].second
                      << " must be different files\n";
                return false;
            }
        }
    }
    return true;
}

bool OutputBundlePaths::prepare(const std::filesystem::path& inputPath,
                                std::ostream& error) const{
    if(!validate(inputPath, error)){
        return false;
    }

    std::error_code createError;
    std::filesystem::create_directories(directory, createError);
    if(createError){
        error << "Cannot create output bundle directory <"
              << directory.string() << ">: " << createError.message() << '\n';
        return false;
    }

    // Revalidate after creation to catch aliases or concurrent path changes.
    return validate(inputPath, error);
}

bool OutputBundlePaths::validateMirrors(
    const std::optional<std::string>& listingMirror,
    const std::optional<std::string>& rawMirror,
    std::ostream& error
) const{
    const std::array<std::pair<const std::filesystem::path*, const char*>, 5>
        outputs = {{
            {&listingPath, "listing output"},
            {&rawPath, "raw output"},
            {&errorPath, "error log"},
            {&reportPath, "solve report"},
            {&ptaTracePath, "PTA trace"}
        }};

    const auto validateMirror = [&outputs, &error, this](
        const std::optional<std::string>& mirror,
        const char* description
    ) {
        if(!mirror){
            return true;
        }
        if(pathsReferToSameFile(*mirror, directory)){
            error << "The " << description
                  << " must not be the output bundle directory\n";
            return false;
        }
        for(const auto& output: outputs){
            if(pathsReferToSameFile(*mirror, *output.first)){
                error << "The " << description << " and canonical "
                      << output.second << " must be different files\n";
                return false;
            }
        }
        return true;
    };

    return validateMirror(listingMirror, "listing mirror") &&
           validateMirror(rawMirror, "raw mirror");
}

bool SpiceOutputFiles::validatePaths(
    const std::string& inputPath,
    const std::optional<std::string>& listingPath,
    const std::optional<std::string>& rawPath,
    std::ostream& error
){
    if(listingPath && pathsReferToSameFile(*listingPath, inputPath)){
        error << "Input netlist and listing output must be different files\n";
        return false;
    }
    if(rawPath && pathsReferToSameFile(*rawPath, inputPath)){
        error << "Input netlist and raw output must be different files\n";
        return false;
    }
    if(listingPath && rawPath &&
       pathsReferToSameFile(*listingPath, *rawPath)){
        error << "Listing output and raw output must be different files\n";
        return false;
    }
    return true;
}

bool SpiceOutputFiles::writeAtomically(
    const std::optional<std::string>& listingPath,
    std::string_view listing,
    const std::optional<std::string>& rawPath,
    std::string_view raw,
    std::ostream& error
){
    std::vector<PendingOutput> pendingOutputs;
    pendingOutputs.reserve(
        static_cast<std::size_t>(listingPath.has_value()) +
        static_cast<std::size_t>(rawPath.has_value())
    );

    if(listingPath){
        pendingOutputs.push_back({*listingPath, listing, "listing output"});
    }
    if(rawPath){
        pendingOutputs.push_back({*rawPath, raw, "raw output"});
    }
    return writeOutputsAtomically(pendingOutputs, error);
}

bool SpiceOutputFiles::writeAtomically(
    const OutputBundlePaths& paths,
    std::string_view listing,
    std::string_view raw,
    std::string_view errorLog,
    std::ostream& error
){
    return writeOutputsAtomically(
        {
            {paths.listingPath, listing, "listing output"},
            {paths.rawPath, raw, "raw output"},
            {paths.errorPath, errorLog, "error log"},
            {paths.reportPath, {}, "solve report", true},
            {paths.ptaTracePath, {}, "PTA trace", true}
        },
        error
    );
}

bool SpiceOutputFiles::writeAtomically(
    const OutputBundlePaths& paths,
    std::string_view listing,
    std::string_view raw,
    std::string_view errorLog,
    const std::optional<std::string_view>& ptaTrace,
    std::ostream& error
){
    return writeOutputsAtomically(
        {
            {paths.listingPath, listing, "listing output"},
            {paths.rawPath, raw, "raw output"},
            {paths.errorPath, errorLog, "error log"},
            {paths.reportPath, {}, "solve report", true},
            ptaTrace
                ? PendingOutput{paths.ptaTracePath, *ptaTrace, "PTA trace"}
                : PendingOutput{paths.ptaTracePath, {}, "PTA trace", true}
        },
        error
    );
}

bool SpiceOutputFiles::writeAtomically(
    const OutputBundlePaths& paths,
    std::string_view listing,
    std::string_view raw,
    std::string_view errorLog,
    std::string_view report,
    std::ostream& error
){
    return writeOutputsAtomically(
        {
            {paths.listingPath, listing, "listing output"},
            {paths.rawPath, raw, "raw output"},
            {paths.errorPath, errorLog, "error log"},
            {paths.reportPath, report, "solve report"},
            {paths.ptaTracePath, {}, "PTA trace", true}
        },
        error
    );
}

bool SpiceOutputFiles::writeAtomically(
    const OutputBundlePaths& paths,
    std::string_view listing,
    std::string_view raw,
    std::string_view errorLog,
    std::string_view report,
    const std::optional<std::string_view>& ptaTrace,
    std::ostream& error
){
    return writeOutputsAtomically(
        {
            {paths.listingPath, listing, "listing output"},
            {paths.rawPath, raw, "raw output"},
            {paths.errorPath, errorLog, "error log"},
            {paths.reportPath, report, "solve report"},
            ptaTrace
                ? PendingOutput{paths.ptaTracePath, *ptaTrace, "PTA trace"}
                : PendingOutput{paths.ptaTracePath, {}, "PTA trace", true}
        },
        error
    );
}

bool SpiceOutputFiles::writeFailureAtomically(
    const OutputBundlePaths& paths,
    std::string_view errorLog,
    std::ostream& error
){
    return writeOutputsAtomically(
        {
            {paths.errorPath, errorLog, "error log"},
            {paths.reportPath, {}, "solve report", true},
            {paths.ptaTracePath, {}, "PTA trace", true}
        },
        error
    );
}

bool SpiceOutputFiles::writeFailureAtomically(
    const OutputBundlePaths& paths,
    std::string_view errorLog,
    const std::optional<std::string_view>& ptaTrace,
    std::ostream& error
){
    return writeOutputsAtomically(
        {
            {paths.errorPath, errorLog, "error log"},
            {paths.reportPath, {}, "solve report", true},
            ptaTrace
                ? PendingOutput{paths.ptaTracePath, *ptaTrace, "PTA trace"}
                : PendingOutput{paths.ptaTracePath, {}, "PTA trace", true}
        },
        error
    );
}

bool SpiceOutputFiles::writeFailureAtomically(
    const OutputBundlePaths& paths,
    std::string_view errorLog,
    std::string_view report,
    std::ostream& error
){
    return writeOutputsAtomically(
        {
            {paths.errorPath, errorLog, "error log"},
            {paths.reportPath, report, "solve report"},
            {paths.ptaTracePath, {}, "PTA trace", true}
        },
        error
    );
}

bool SpiceOutputFiles::writeFailureAtomically(
    const OutputBundlePaths& paths,
    std::string_view errorLog,
    std::string_view report,
    const std::optional<std::string_view>& ptaTrace,
    std::ostream& error
){
    return writeOutputsAtomically(
        {
            {paths.errorPath, errorLog, "error log"},
            {paths.reportPath, report, "solve report"},
            ptaTrace
                ? PendingOutput{paths.ptaTracePath, *ptaTrace, "PTA trace"}
                : PendingOutput{paths.ptaTracePath, {}, "PTA trace", true}
        },
        error
    );
}
