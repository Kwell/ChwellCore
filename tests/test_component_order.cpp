#include <gtest/gtest.h>
#include "chwell/service/component_order.h"
#include <algorithm>
#include <limits>
#include <random>

using chwell::service::ComponentSpec;
using chwell::service::resolve_component_order;

TEST(ComponentOrderTest, DependenciesOverridePriorityAndNewlyReadyNodesCompete) {
    std::vector<ComponentSpec> components{
        {"Players", -100, {"Storage"}}, {"Metrics", 10, {}},
        {"Storage", 20, {}}, {"Gateway", -200, {"Players"}}};
    std::vector<std::size_t> order;
    ASSERT_TRUE(resolve_component_order(components, order));
    EXPECT_EQ(order, (std::vector<std::size_t>{1, 2, 0, 3}));
}

TEST(ComponentOrderTest, NoDependenciesPreservesStablePriorityOrder) {
    std::vector<ComponentSpec> components{{"z", 100, {}}, {"a", 100, {}},
        {"min", std::numeric_limits<int>::min(), {}}, {"max", std::numeric_limits<int>::max(), {}}};
    std::vector<std::size_t> order;
    std::string error = "old error";
    ASSERT_TRUE(resolve_component_order(components, order, &error));
    EXPECT_EQ(order, (std::vector<std::size_t>{2, 0, 1, 3}));
    EXPECT_TRUE(error.empty());
    ASSERT_TRUE(resolve_component_order({}, order));
    EXPECT_TRUE(order.empty());
}

TEST(ComponentOrderTest, DiamondAndRepeatedEdgesInitializeEachNodeOnce) {
    std::vector<ComponentSpec> components{{"root", 20, {}}, {"left", 10, {"root", "root"}},
        {"right", 0, {"root"}}, {"leaf", -10, {"left", "right", "left"}}};
    std::vector<std::size_t> order;
    ASSERT_TRUE(resolve_component_order(components, order));
    EXPECT_EQ(order, (std::vector<std::size_t>{0, 2, 1, 3}));
}

TEST(ComponentOrderTest, InvalidGraphsPreserveOutputAndNameTheProblem) {
    const std::vector<std::pair<std::vector<ComponentSpec>, std::string>> cases{
        {{{"", 100, {}}}, "name must not be empty"},
        {{{"a", 100, {}}, {"a", 100, {}}}, "Duplicate component name: a"},
        {{{"a", 100, {""}}}, "Empty dependency for component a"},
        {{{"a", 100, {"absent"}}}, "Missing dependency: a -> absent"},
        {{{"a", 100, {"a"}}}, "Self dependency: a -> a"}};
    for (const auto& item : cases) {
        SCOPED_TRACE(item.second);
        std::vector<std::size_t> order{42};
        std::string error;
        EXPECT_FALSE(resolve_component_order(item.first, order, &error));
        EXPECT_EQ(order, (std::vector<std::size_t>{42}));
        EXPECT_NE(error.find(item.second), std::string::npos);
    }
}

TEST(ComponentOrderTest, CycleDiagnosticExcludesBlockedTailAndUnrelatedNodes) {
    std::vector<ComponentSpec> components{{"tail", 0, {"a"}}, {"ok", -1, {}},
        {"a", 0, {"b"}}, {"b", 0, {"c"}}, {"c", 0, {"a"}}};
    std::vector<std::size_t> order{42};
    std::string error;
    EXPECT_FALSE(resolve_component_order(components, order, &error));
    EXPECT_EQ(error, "Component dependency cycle: a -> b -> c -> a");
    EXPECT_EQ(order, (std::vector<std::size_t>{42}));
}

TEST(ComponentOrderTest, RandomDagsRespectEveryEdgeAndAreRepeatable) {
    std::mt19937 random(20261010);
    for (int round = 0; round < 40; ++round) {
        std::vector<ComponentSpec> components;
        for (int index = 0; index < 100; ++index) {
            ComponentSpec spec{"node" + std::to_string(index), static_cast<int>(random() % 20), {}};
            for (int prerequisite = 0; prerequisite < index; ++prerequisite)
                if (random() % 20 == 0) spec.dependencies.push_back("node" + std::to_string(prerequisite));
            components.push_back(std::move(spec));
        }
        std::shuffle(components.begin(), components.end(), random);
        std::vector<std::size_t> order, repeat;
        ASSERT_TRUE(resolve_component_order(components, order));
        ASSERT_TRUE(resolve_component_order(components, repeat));
        EXPECT_EQ(order, repeat);
        ASSERT_EQ(order.size(), components.size());
        std::vector<std::string> visited;
        for (auto index : order) {
            for (const auto& dependency : components[index].dependencies)
                EXPECT_NE(std::find(visited.begin(), visited.end(), dependency), visited.end());
            visited.push_back(components[index].name);
        }
    }
}

TEST(ComponentOrderTest, DeepGraphsDoNotRequireRecursiveTraversal) {
    std::vector<ComponentSpec> components;
    for (int index = 0; index < 20000; ++index)
        components.push_back({"node" + std::to_string(index), 100,
            index ? std::vector<std::string>{"node" + std::to_string(index - 1)} : std::vector<std::string>{}});
    std::vector<std::size_t> order;
    ASSERT_TRUE(resolve_component_order(components, order));
    EXPECT_EQ(order.front(), 0u);
    EXPECT_EQ(order.back(), 19999u);
    components[0].dependencies = {"node19999"};
    std::string error;
    EXPECT_FALSE(resolve_component_order(components, order, &error));
    EXPECT_EQ(order.size(), 20000u);
    EXPECT_EQ(error.find("Component dependency cycle: node0"), 0u);
}
