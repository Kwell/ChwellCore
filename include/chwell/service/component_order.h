#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace chwell { namespace service {

// A dependency names another node that must precede this node in every forward
// lifecycle phase. Priority/input order break ties only among ready nodes.
struct ComponentSpec {
    std::string name;
    int priority = 100;
    std::vector<std::string> dependencies;
};

// Pure planning step: no component callbacks or platform I/O. On failure order
// stays unchanged; error identifies missing/self dependencies or a cycle path.
// Repeated edges are coalesced (e.g. declared in both code and manifest).
bool resolve_component_order(const std::vector<ComponentSpec>& components,
                             std::vector<std::size_t>& order, std::string* error = nullptr);

} } // namespace chwell::service
