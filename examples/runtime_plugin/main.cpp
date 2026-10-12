#include "chwell/service/runtime_plugin.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace chwell::service;
namespace {
int evaluate(const std::shared_ptr<const RuntimePluginSnapshot>& snapshot) {
    std::vector<std::uint8_t> result;
    std::string error;
    if (!snapshot->invoke({0, 0, 0, 10}, result, &error, 4) || result.size() != 4)
        throw std::runtime_error(error);
    return (int(result[0]) << 24) | (int(result[1]) << 16) | (int(result[2]) << 8) | result[3];
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 3) throw std::runtime_error("Usage: runtime_plugin_demo rule_v1_library rule_v2_library");
        const auto first = std::filesystem::canonical(std::filesystem::u8path(argv[1]));
        RuntimePluginStore store("example.damage", first.parent_path().u8string());
        std::string error;
        if (!store.publish(first.u8string(), 0, &error)) throw std::runtime_error(error);
        auto old = store.acquire();
        if (evaluate(old) != 20) throw std::runtime_error("Wrong v1 rule");
        if (!store.publish(argv[2], 1, &error)) throw std::runtime_error(error);
        if (evaluate(store.acquire()) != 30 || evaluate(old) != 20) throw std::runtime_error("Mixed plugin releases");
        if (!store.rollback(2, &error) || evaluate(store.acquire()) != 20) throw std::runtime_error(error);
        std::cout << "PASS: independent C ABI rules, v1=20, v2=30, retained v1=20 and rollback=20\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
