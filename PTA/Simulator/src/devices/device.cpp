#include "devices/device.hpp"

#include "circuit/node_map.hpp"

void Device::bindNodes(const NodeMap& nodeMap){
    nodeIds.resize(nodes.size());
    for(std::size_t i = 0; i < nodes.size(); ++i){
        nodeIds[i] = nodeMap.idxOf(nodes[i]);
    }
}
