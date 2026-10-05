#include "dependency/dependency_graph.hpp"

#include <algorithm>
#include <queue>
#include <unordered_set>

namespace pilot {

namespace {
const std::vector<std::string> kEmpty;
}

Result<void> DependencyGraph::addService(const ServiceDefinition& service) {
    if (dependencies_.contains(service.name)) {
        return makeError(ErrorCode::InvalidConfiguration, "duplicate service '" + service.name + "'");
    }
    std::vector<std::string> deps;
    for (const auto& d : service.dependencies) {
        if (std::find(deps.begin(), deps.end(), d) == deps.end()) {
            deps.push_back(d);
        }
    }
    names_.push_back(service.name);
    dependencies_.emplace(service.name, std::move(deps));
    return Result<void>::success();
}

const std::vector<std::string>& DependencyGraph::dependenciesOf(const std::string& name) const {
    const auto it = dependencies_.find(name);
    return it == dependencies_.end() ? kEmpty : it->second;
}

Result<void> DependencyGraph::checkMissing() const {
    for (const auto& name : names_) {
        for (const auto& dep : dependencies_.at(name)) {
            if (!dependencies_.contains(dep)) {
                return makeError(ErrorCode::InvalidConfiguration,
                                 "service '" + name + "' depends on unknown service '" + dep + "'");
            }
        }
    }
    return Result<void>::success();
}

Result<void> DependencyGraph::validate() const {
    auto order = topologicalOrder();
    if (!order) {
        return order.error();
    }
    return Result<void>::success();
}

Result<std::vector<std::string>> DependencyGraph::topologicalOrder() const {
    if (auto r = checkMissing(); !r) {
        return r.error();
    }

    // indegree[n] = number of dependencies of n that are not yet started.
    // adjacency[d] = services that depend on d (edges d -> dependent).
    std::unordered_map<std::string, std::size_t> indegree;
    std::unordered_map<std::string, std::vector<std::string>> adjacency;
    for (const auto& name : names_) {
        const auto& deps = dependencies_.at(name);
        indegree[name] = deps.size();
        for (const auto& d : deps) {
            adjacency[d].push_back(name);
        }
    }

    std::queue<std::string> ready;
    for (const auto& name : names_) {
        if (indegree[name] == 0) {
            ready.push(name);
        }
    }

    std::vector<std::string> order;
    order.reserve(names_.size());
    while (!ready.empty()) {
        std::string current = std::move(ready.front());
        ready.pop();
        const auto it = adjacency.find(current);
        if (it != adjacency.end()) {
            for (const auto& dependent : it->second) {
                if (--indegree[dependent] == 0) {
                    ready.push(dependent);
                }
            }
        }
        order.push_back(std::move(current));
    }

    if (order.size() != names_.size()) {
        const auto cycle = findCycle(indegree);
        std::string path;
        for (std::size_t i = 0; i < cycle.size(); ++i) {
            path += (i == 0 ? "" : " -> ") + cycle[i];
        }
        return makeError(ErrorCode::DependencyCycle, "Circular dependency detected: " + path);
    }
    return order;
}

Result<std::vector<std::string>> DependencyGraph::shutdownOrder() const {
    auto order = topologicalOrder();
    if (!order) {
        return order.error();
    }
    std::reverse(order.value().begin(), order.value().end());
    return order;
}

// Every node left over by Kahn's algorithm still has at least one unresolved
// dependency that is itself left over, so following "depends on" edges inside
// the leftover set must eventually revisit a node: that is a real cycle.
std::vector<std::string> DependencyGraph::findCycle(
    const std::unordered_map<std::string, std::size_t>& indegree) const {
    const auto leftover = [&](const std::string& n) {
        const auto it = indegree.find(n);
        return it != indegree.end() && it->second > 0;
    };

    std::string current;
    for (const auto& n : names_) {
        if (leftover(n)) {
            current = n;
            break;
        }
    }
    if (current.empty()) {
        return {};
    }

    std::vector<std::string> path;
    std::unordered_map<std::string, std::size_t> position;
    while (!position.contains(current)) {
        position.emplace(current, path.size());
        path.push_back(current);
        std::string next;
        for (const auto& dep : dependencies_.at(current)) {
            if (leftover(dep)) {
                next = dep;
                break;
            }
        }
        if (next.empty()) {
            return path;  // defensive: cannot happen for a consistent graph
        }
        current = std::move(next);
    }
    std::vector<std::string> cycle(path.begin() + static_cast<std::ptrdiff_t>(position.at(current)), path.end());
    cycle.push_back(current);
    return cycle;
}

}  // namespace pilot
