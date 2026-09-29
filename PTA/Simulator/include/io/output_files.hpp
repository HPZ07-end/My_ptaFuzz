#pragma once

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>

struct OutputBundlePaths {
    std::filesystem::path directory;
    std::filesystem::path listingPath;
    std::filesystem::path rawPath;
    std::filesystem::path errorPath;
    std::filesystem::path reportPath;
    std::filesystem::path ptaTracePath;

    static OutputBundlePaths derive(
        const std::filesystem::path& inputPath,
        const std::optional<std::filesystem::path>& outputRoot = std::nullopt
    );

    bool validate(const std::filesystem::path& inputPath,
                  std::ostream& error) const;

    bool prepare(const std::filesystem::path& inputPath,
                 std::ostream& error) const;

    bool validateMirrors(
        const std::optional<std::string>& listingPath,
        const std::optional<std::string>& rawPath,
        std::ostream& error
    ) const;
};

class SpiceOutputFiles {
public:
    static bool validatePaths(
        const std::string& inputPath,
        const std::optional<std::string>& listingPath,
        const std::optional<std::string>& rawPath,
        std::ostream& error
    );

    static bool writeAtomically(
        const std::optional<std::string>& listingPath,
        std::string_view listing,
        const std::optional<std::string>& rawPath,
        std::string_view raw,
        std::ostream& error
    );

    static bool writeAtomically(
        const OutputBundlePaths& paths,
        std::string_view listing,
        std::string_view raw,
        std::string_view errorLog,
        std::ostream& error
    );

    static bool writeAtomically(
        const OutputBundlePaths& paths,
        std::string_view listing,
        std::string_view raw,
        std::string_view errorLog,
        const std::optional<std::string_view>& ptaTrace,
        std::ostream& error
    );

    static bool writeAtomically(
        const OutputBundlePaths& paths,
        std::string_view listing,
        std::string_view raw,
        std::string_view errorLog,
        std::string_view report,
        std::ostream& error
    );

    static bool writeAtomically(
        const OutputBundlePaths& paths,
        std::string_view listing,
        std::string_view raw,
        std::string_view errorLog,
        std::string_view report,
        const std::optional<std::string_view>& ptaTrace,
        std::ostream& error
    );

    static bool writeFailureAtomically(
        const OutputBundlePaths& paths,
        std::string_view errorLog,
        std::ostream& error
    );

    static bool writeFailureAtomically(
        const OutputBundlePaths& paths,
        std::string_view errorLog,
        const std::optional<std::string_view>& ptaTrace,
        std::ostream& error
    );

    static bool writeFailureAtomically(
        const OutputBundlePaths& paths,
        std::string_view errorLog,
        std::string_view report,
        std::ostream& error
    );

    static bool writeFailureAtomically(
        const OutputBundlePaths& paths,
        std::string_view errorLog,
        std::string_view report,
        const std::optional<std::string_view>& ptaTrace,
        std::ostream& error
    );
};
