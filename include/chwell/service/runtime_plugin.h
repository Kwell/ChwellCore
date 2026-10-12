#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace chwell { namespace service {
namespace detail { struct RuntimePluginImage; }

// A retained snapshot pins its library, including while invoke is executing.
// Holding it across multiple calls keeps one release for a logical operation.
class RuntimePluginSnapshot {
public:
    const std::string& name() const;
    std::uint64_t version() const;
    std::uint64_t generation() const { return generation_; }
    const std::string& source_path() const;
    bool invoke(const std::vector<std::uint8_t>& input, std::vector<std::uint8_t>& output,
                std::string* error = nullptr, std::size_t capacity = 1024 * 1024) const;
private:
    friend class RuntimePluginStore;
    RuntimePluginSnapshot(std::shared_ptr<detail::RuntimePluginImage> image, std::uint64_t generation);
    std::shared_ptr<detail::RuntimePluginImage> image_;
    std::uint64_t generation_;
};

// One plugin identity per store. Candidates use unique library copies; replacing
// the source path cannot make dlopen/LoadLibrary reuse the current image.
class RuntimePluginStore {
public:
    RuntimePluginStore(std::string name, std::string allowed_directory);
    std::shared_ptr<const RuntimePluginSnapshot> acquire() const;
    std::uint64_t generation() const;
    bool publish(const std::string& path, std::uint64_t expected_generation, std::string* error = nullptr);
    bool rollback(std::uint64_t expected_generation, std::string* error = nullptr);
    bool clear(std::uint64_t expected_generation, std::string* error = nullptr);
private:
    std::string name_, allowed_directory_;
    mutable std::mutex mutex_;
    std::shared_ptr<const RuntimePluginSnapshot> current_, previous_;
    std::uint64_t generation_ = 0, high_water_ = 0;
};

} }
