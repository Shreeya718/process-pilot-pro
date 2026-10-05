# Dependency resolution

Edge `A → B` means "A depends on B". `DependencyGraph::topologicalOrder()` uses **Kahn's algorithm**:

1. `indegree[n]` = number of dependencies of `n`; `adjacency[d]` = dependents of `d`.
2. Queue every service with indegree 0 (registration order keeps the result deterministic).
3. Pop `n`, append to the order, decrement the indegree of each dependent; enqueue those reaching 0.
4. If fewer than V services were emitted, a cycle exists.

Cycle reporting: every leftover node still has an unresolved dependency that is also leftover, so following
"depends on" edges inside the leftover set must revisit a node - that walk is printed, e.g.
`Circular dependency detected: A -> B -> C -> A`. Services that merely depend on a cycle are not reported as part of it.

Missing dependencies and duplicate names are rejected before sorting. A service that is enabled may not depend on a
disabled one. Complexity: `O(V + E)` time, `O(V + E)` space. Shutdown order is the exact reverse.
