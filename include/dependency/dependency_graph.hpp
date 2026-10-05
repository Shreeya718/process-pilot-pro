#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/result.hpp"
#include "config/service_config.hpp"

namespace pilot {

/// Directed dependency graph: an edge  A -> B  means "A depends on B".
///
/// Responsibility: register services, detect missing dependencies and cycles,
/// compute startup / shutdown order. It never launches processes.
///
/// Complexity (V = services, E = dependency edges):
///   addService          O(deps of the service) average
///   validate / sort     O(V + E)   (Kahn's algorithm, hash-map adjacency)
class DependencyGraph {
public:
    /// Registers a service. Fails on duplicate names.
    [[nodiscard]] Result<void> addService(const ServiceDefinition& service);

    /// Fails with InvalidConfiguration on missing dependencies and with
    /// DependencyCycle on circular dependencies.
    [[nodiscard]] Result<void> validate() const;

    /// Dependencies first. Ties are broken by registration order (deterministic).
    [[nodiscard]] Result<std::vector<std::string>> topologicalOrder() const;

    /// Exact reverse of topologicalOrder(): dependents first.
    [[nodiscard]] Result<std::vector<std::string>> shutdownOrder() const;

    [[nodiscard]] std::size_t size() const noexcept { return names_.size(); }
    [[nodiscard]] bool contains(const std::string& name) const { return dependencies_.contains(name); }
    [[nodiscard]] const std::vector<std::string>& dependenciesOf(const std::string& name) const;

private:
    [[nodiscard]] Result<void> checkMissing() const;
    [[nodiscard]] std::vector<std::string> findCycle(
        const std::unordered_map<std::string, std::size_t>& indegree) const;

    std::vector<std::string> names_;                                      // registration order
    std::unordered_map<std::string, std::vector<std::string>> dependencies_;  // node -> what it needs
};

}  // namespace pilot
