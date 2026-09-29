#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <vector>

#include <nlohmann/json.hpp>

namespace simulator::config {

inline constexpr char kDefaultConfigFilename[] = "config.json";

/**
 * explicitPath 有值时，只读取该文件，不执行自动搜索。
 * explicitPath 为空时，从工作目录开始搜索 config.json，最多向父目录移动
 * parentSearchLimit 次。
 */
struct ConfigSearchOptions {
    std::optional<std::filesystem::path> explicitPath;
    std::size_t parentSearchLimit {8};
};

/**
 * 已加载的配置文件。
 *
 * found == false 表示自动搜索未发现 config.json；此时 path 为空，document
 * 是空 JSON 对象，调用方应保留现有默认值。
 *
 * found == true 时，path 为实际读取文件的规范化绝对路径，document 为已成功
 * 解析且根节点为对象的 JSON 内容。
 */
struct LoadedConfig {
    bool found {false};
    std::filesystem::path path;
    nlohmann::json document {nlohmann::json::object()};
};

/**
 * 返回配置搜索过程中会检查的候选路径。
 *
 * 显式指定配置文件时，结果只包含 explicitPath。自动搜索时，结果从
 * workingDirectory/config.json 开始，依次向父目录查找。
 */
[[nodiscard]] std::vector<std::filesystem::path>
configSearchCandidates(
    const ConfigSearchOptions& options,
    const std::filesystem::path& workingDirectory =
        std::filesystem::current_path()
);

/**
 * 定位、读取并解析 JSON 配置文件。
 *
 * 自动搜索找不到配置文件时，返回 found == false 的 LoadedConfig，不将其视为
 * 错误。显式配置文件不存在、不可读、JSON 非法或根节点不是对象时，抛出
 * std::runtime_error。
 */
[[nodiscard]] LoadedConfig loadConfig(
    const ConfigSearchOptions& options,
    const std::filesystem::path& workingDirectory =
        std::filesystem::current_path()
);

}  // namespace simulator::config
