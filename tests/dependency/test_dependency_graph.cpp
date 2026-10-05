#include <algorithm>

#include "dependency/dependency_graph.hpp"
#include "test_framework.hpp"

using namespace pilot;

namespace {
ServiceDefinition svc(const std::string& name, std::vector<std::string> deps = {}) {
    ServiceDefinition s;
    s.name = name;
    s.executable = "/bin/true";
    s.dependencies = std::move(deps);
    return s;
}

std::size_t indexOf(const std::vector<std::string>& v, const std::string& n) {
    return static_cast<std::size_t>(std::find(v.begin(), v.end(), n) - v.begin());
}
}  // namespace

PP_TEST(linear_chain_orders_dependencies_first) {
    DependencyGraph g;
    CHECK(g.addService(svc("WebApp", {"Database"})).ok());
    CHECK(g.addService(svc("Database", {"Logger"})).ok());
    CHECK(g.addService(svc("Logger")).ok());  // declared last on purpose
    auto order = g.topologicalOrder();
    CHECK(order.ok());
    if (!order) return;
    CHECK((order.value() == std::vector<std::string>{"Logger", "Database", "WebApp"}));
    auto down = g.shutdownOrder();
    CHECK(down.ok() && (down.value() == std::vector<std::string>{"WebApp", "Database", "Logger"}));
}

PP_TEST(diamond_respects_all_edges) {
    DependencyGraph g;
    CHECK(g.addService(svc("D", {"B", "C"})).ok());
    CHECK(g.addService(svc("B", {"A"})).ok());
    CHECK(g.addService(svc("C", {"A"})).ok());
    CHECK(g.addService(svc("A")).ok());
    auto order = g.topologicalOrder();
    CHECK(order.ok());
    if (!order) return;
    const auto& o = order.value();
    CHECK(o.size() == 4u);
    CHECK(indexOf(o, "A") < indexOf(o, "B"));
    CHECK(indexOf(o, "A") < indexOf(o, "C"));
    CHECK(indexOf(o, "B") < indexOf(o, "D"));
    CHECK(indexOf(o, "C") < indexOf(o, "D"));
}

PP_TEST(independent_services_keep_registration_order) {
    DependencyGraph g;
    CHECK(g.addService(svc("Z")).ok());
    CHECK(g.addService(svc("M")).ok());
    CHECK(g.addService(svc("A")).ok());
    auto order = g.topologicalOrder();
    CHECK(order.ok() && (order.value() == std::vector<std::string>{"Z", "M", "A"}));
}

PP_TEST(detects_three_node_cycle) {
    DependencyGraph g;
    CHECK(g.addService(svc("A", {"B"})).ok());
    CHECK(g.addService(svc("B", {"C"})).ok());
    CHECK(g.addService(svc("C", {"A"})).ok());
    auto r = g.validate();
    CHECK(!r.ok());
    if (r) return;
    CHECK(r.error().code == ErrorCode::DependencyCycle);
    CHECK(r.error().message.find("Circular dependency detected") != std::string::npos);
    CHECK(r.error().message.find("A -> B -> C -> A") != std::string::npos);
    CHECK(!g.topologicalOrder().ok());
}

PP_TEST(cycle_report_ignores_innocent_dependents) {
    DependencyGraph g;
    CHECK(g.addService(svc("Web", {"A"})).ok());  // depends on the cycle but is not part of it
    CHECK(g.addService(svc("A", {"B"})).ok());
    CHECK(g.addService(svc("B", {"A"})).ok());
    auto r = g.validate();
    CHECK(!r.ok());
    if (r) return;
    CHECK(r.error().code == ErrorCode::DependencyCycle);
    CHECK(r.error().message.find("Web") == std::string::npos);
}

PP_TEST(detects_self_dependency) {
    DependencyGraph g;
    CHECK(g.addService(svc("A", {"A"})).ok());
    auto r = g.validate();
    CHECK(!r.ok());
    if (!r) CHECK(r.error().code == ErrorCode::DependencyCycle);
}

PP_TEST(detects_missing_dependency) {
    DependencyGraph g;
    CHECK(g.addService(svc("A", {"Ghost"})).ok());
    auto r = g.validate();
    CHECK(!r.ok());
    if (!r) {
        CHECK(r.error().code == ErrorCode::InvalidConfiguration);
        CHECK(r.error().message.find("Ghost") != std::string::npos);
    }
}

PP_TEST(rejects_duplicate_service) {
    DependencyGraph g;
    CHECK(g.addService(svc("A")).ok());
    CHECK(!g.addService(svc("A")).ok());
    CHECK(g.size() == 1u);
}

PP_TEST(handles_larger_graph) {
    DependencyGraph g;
    constexpr int n = 2000;
    for (int i = n - 1; i >= 0; --i) {  // each depends on its predecessor, registered in reverse
        std::vector<std::string> deps;
        if (i > 0) deps.push_back("S" + std::to_string(i - 1));
        CHECK(g.addService(svc("S" + std::to_string(i), deps)).ok());
    }
    auto order = g.topologicalOrder();
    CHECK(order.ok() && order.value().size() == static_cast<std::size_t>(n));
    if (order) CHECK(order.value().front() == "S0" && order.value().back() == "S1999");
}

PP_TEST_MAIN()
