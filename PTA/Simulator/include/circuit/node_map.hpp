#pragma once
#include <vector>
#include <string>
#include <memory>
#include <unordered_map>

class Device;

class NodeMap {
public:
    NodeMap() = default;
    ~NodeMap() = default;

    void build(const std::vector<std::unique_ptr<Device>>& devices);

    int idxOf(const std::string& name) const;

    const std::vector<std::string>& nodeNameByIdx() const;

    int nodeCount() const;

private:
    std::unordered_map<std::string, int> name_to_idx;
    std::vector<std::string> idx_to_name;
};
