#include "chwell/service/component_order.h"

#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace chwell { namespace service {

bool resolve_component_order(const std::vector<ComponentSpec>& components,
                             std::vector<std::size_t>& order, std::string* error) {
    const auto fail = [error](const std::string& message) {
        if (error) *error = message;
        return false;
    };
    const auto size = components.size();
    std::unordered_map<std::string, std::size_t> names;
    for (std::size_t index = 0; index < size; ++index) {
        if (components[index].name.empty()) return fail("Component name must not be empty");
        if (!names.emplace(components[index].name, index).second)
            return fail("Duplicate component name: " + components[index].name);
    }
    std::vector<std::vector<std::size_t>> required(size), dependents(size);
    std::vector<std::size_t> indegree(size, 0);
    for (std::size_t index = 0; index < size; ++index) {
        std::unordered_set<std::string> seen;
        for (const auto& dependency : components[index].dependencies) {
            const auto& name = components[index].name;
            if (dependency.empty()) return fail("Empty dependency for component " + name);
            const auto target = names.find(dependency);
            if (target == names.end()) return fail("Missing dependency: " + name + " -> " + dependency);
            if (target->second == index) return fail("Self dependency: " + name + " -> " + name);
            if (!seen.insert(dependency).second) continue;
            required[index].push_back(target->second);
            dependents[target->second].push_back(index);
            ++indegree[index];
        }
    }
    const auto later = [&components](std::size_t a, std::size_t b) {
        return components[a].priority != components[b].priority
            ? components[a].priority > components[b].priority : a > b;
    };
    std::priority_queue<std::size_t, std::vector<std::size_t>, decltype(later)> ready(later);
    for (std::size_t index = 0; index < size; ++index) if (!indegree[index]) ready.push(index);
    std::vector<std::size_t> planned;
    planned.reserve(size);
    while (!ready.empty()) {
        const auto index = ready.top();
        ready.pop();
        planned.push_back(index);
        for (auto dependent : dependents[index]) if (!--indegree[dependent]) ready.push(dependent);
    }
    if (planned.size() != size) {
        // Walk unmet prerequisites iteratively, avoiding stack overflow on long
        // chains. The grey back edge gives a real cycle, excluding blocked tails.
        std::vector<unsigned char> state(size, 0);
        std::vector<std::size_t> position(size, 0);
        std::vector<std::pair<std::size_t, std::size_t>> stack;
        for (std::size_t root = 0; root < size; ++root) {
            if (!indegree[root] || state[root]) continue;
            stack.emplace_back(root, 0);
            state[root] = 1;
            position[root] = 0;
            while (!stack.empty()) {
                auto& frame = stack.back();
                if (frame.second == required[frame.first].size()) {
                    state[frame.first] = 2;
                    stack.pop_back();
                    continue;
                }
                const auto next = required[frame.first][frame.second++];
                if (!indegree[next] || state[next] == 2) continue;
                if (state[next] == 1) {
                    std::string message = "Component dependency cycle: ";
                    for (auto index = position[next]; index < stack.size(); ++index)
                        message += components[stack[index].first].name + " -> ";
                    return fail(message + components[next].name);
                }
                state[next] = 1;
                position[next] = stack.size();
                stack.emplace_back(next, 0);
            }
        }
        return fail("Component dependency cycle");
    }
    order = std::move(planned);
    if (error) error->clear();
    return true;
}

} } // namespace chwell::service
