#include "chwell/service/runtime_plugin.h"
#include "chwell/service/runtime_plugin_abi.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <system_error>
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace chwell { namespace service {
namespace fs = std::filesystem;
namespace {
bool fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}
bool valid_name(const std::string& name) {
    if (name.empty() || name.size() > 128) return false;
    for (unsigned char c : name) {
        if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9') &&
            c != '_' && c != '-' && c != '.') return false;
    }
    return true;
}
bool contained(const fs::path& directory, const fs::path& path) {
    auto root = directory.begin(), part = path.begin();
    for (; root != directory.end(); ++root, ++part) {
        if (part == path.end() || *part != *root) return false;
    }
    return part != path.end();
}
void close_library(void* handle) {
    if (!handle) return;
#if defined(_WIN32)
    FreeLibrary(static_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
}
}

namespace detail {
struct RuntimePluginImage {
    fs::path image_path;
    std::string name, source;
    std::uint64_t version = 0;
    void* handle = nullptr;
    ChwellPluginInvokeV1 invoke = nullptr;
    std::mutex call_mutex;
    ~RuntimePluginImage() {
        close_library(handle); // Static destructors finish before removing the image.
        std::error_code ignored;
        if (!image_path.empty()) fs::remove(image_path, ignored);
    }
};
}

RuntimePluginSnapshot::RuntimePluginSnapshot(std::shared_ptr<detail::RuntimePluginImage> image,
    std::uint64_t generation) : image_(std::move(image)), generation_(generation) {}
const std::string& RuntimePluginSnapshot::name() const { return image_->name; }
std::uint64_t RuntimePluginSnapshot::version() const { return image_->version; }
const std::string& RuntimePluginSnapshot::source_path() const { return image_->source; }
bool RuntimePluginSnapshot::invoke(const std::vector<std::uint8_t>& input,
    std::vector<std::uint8_t>& output, std::string* error, std::size_t capacity) const {
    if (input.size() > 1024 * 1024 || capacity > 1024 * 1024)
        return fail(error, "Plugin request/response limit is 1 MiB");
    // Empty requests still receive valid (non-null) pointers; lengths are authoritative.
    std::vector<std::uint8_t> pending(capacity ? capacity : 1);
    const std::uint8_t empty = 0;
    std::size_t written = 0;
    int status = -1;
    try {
        std::lock_guard<std::mutex> lock(image_->call_mutex);
        status = image_->invoke(input.empty() ? &empty : input.data(), input.size(),
                                pending.data(), capacity, &written);
    } catch (...) { return fail(error, "Plugin violated the no-exception ABI"); }
    if (status != 0) return fail(error, "Plugin invocation failed: " + std::to_string(status));
    if (written > capacity) return fail(error, "Plugin returned an invalid output length");
    pending.resize(written);
    output.swap(pending);
    if (error) error->clear();
    return true;
}

RuntimePluginStore::RuntimePluginStore(std::string name, std::string allowed_directory) : name_(std::move(name)) {
    if (!valid_name(name_)) throw std::invalid_argument("Invalid runtime plugin identity");
    if (allowed_directory.empty()) throw std::invalid_argument("Plugin directory is required");
    auto directory = fs::canonical(fs::u8path(allowed_directory));
    if (!fs::is_directory(directory)) throw std::invalid_argument("Plugin root must be a directory");
    allowed_directory_ = directory.u8string();
}
std::shared_ptr<const RuntimePluginSnapshot> RuntimePluginStore::acquire() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_;
}
std::uint64_t RuntimePluginStore::generation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_;
}

bool RuntimePluginStore::publish(const std::string& path, std::uint64_t expected, std::string* error) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (expected != generation_) return fail(error, "Stale plugin generation");
    }
    auto image = std::make_shared<detail::RuntimePluginImage>();
    try {
        const auto source = fs::canonical(fs::u8path(path));
        if (!contained(fs::u8path(allowed_directory_), source) || !fs::is_regular_file(source))
            return fail(error, "Plugin is outside the allowed directory or not a regular file");
        const auto size = fs::file_size(source);
        const auto modified = fs::last_write_time(source);
        if (!size || size > 128 * 1024 * 1024) return fail(error, "Plugin image limit is 128 MiB");
        static std::atomic<std::uint64_t> sequence{0};
#if defined(_WIN32)
        const auto pid = GetCurrentProcessId();
#else
        const auto pid = getpid();
#endif
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto copy = source.parent_path() / (".chwell-plugin-" + std::to_string(pid) + "-" +
            std::to_string(nonce) + "-" + std::to_string(++sequence) + source.extension().u8string());
        // No overwrite; the image lives beside its source to preserve $ORIGIN dependencies.
        if (!fs::copy_file(source, copy, fs::copy_options::none)) return fail(error, "Cannot stage plugin image");
        image->image_path = copy;
        if (fs::file_size(source) != size || fs::file_size(copy) != size || fs::last_write_time(source) != modified)
            return fail(error, "Plugin changed while staging; publish immutable artifacts atomically");
        image->source = source.u8string();
#if defined(_WIN32)
        image->handle = LoadLibraryExW(copy.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        auto descriptor = image->handle ? reinterpret_cast<ChwellPluginDescriptorV1>(
            GetProcAddress(static_cast<HMODULE>(image->handle), "chwell_runtime_plugin_v1")) : nullptr;
#else
        image->handle = dlopen(copy.c_str(), RTLD_NOW | RTLD_LOCAL);
        auto descriptor = image->handle ? reinterpret_cast<ChwellPluginDescriptorV1>(
            dlsym(image->handle, "chwell_runtime_plugin_v1")) : nullptr;
#endif
        if (!image->handle || !descriptor) return fail(error, "Plugin library or ABI descriptor unavailable");
        const auto* abi = descriptor();
        if (!abi || abi->abi_version != CHWELL_RUNTIME_PLUGIN_ABI_V1 ||
            abi->struct_size != sizeof(ChwellRuntimePluginV1) || !abi->name || !abi->invoke || !abi->version || abi->reserved)
            return fail(error, "Unsupported runtime plugin ABI");
        std::size_t length = 0;
        while (length <= 128 && abi->name[length]) ++length;
        if (length > 128) return fail(error, "Invalid plugin name length");
        image->name.assign(abi->name, length);
        if (!valid_name(image->name) || image->name != name_) return fail(error, "Plugin identity mismatch");
        image->version = abi->version;
        image->invoke = abi->invoke;
    } catch (const std::exception&) { return fail(error, "Cannot stage or validate runtime plugin"); }
      catch (...) { return fail(error, "Plugin descriptor violated the no-exception ABI"); }

    // Destroy retired snapshots outside the store mutex: dlclose can run plugin destructors.
    std::shared_ptr<const RuntimePluginSnapshot> retired;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (expected != generation_) return fail(error, "Stale plugin generation");
        if (generation_ == std::numeric_limits<std::uint64_t>::max()) return fail(error, "Plugin generation exhausted");
        if (image->version <= high_water_) return fail(error, "Plugin version must exceed the release high-water mark");
        auto next = std::shared_ptr<const RuntimePluginSnapshot>(new RuntimePluginSnapshot(image, generation_ + 1));
        retired = std::move(previous_);
        previous_ = std::move(current_);
        current_ = std::move(next);
        high_water_ = image->version;
        ++generation_;
    }
    if (error) error->clear();
    return true;
}
bool RuntimePluginStore::rollback(std::uint64_t expected, std::string* error) {
    std::shared_ptr<const RuntimePluginSnapshot> retired;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (expected != generation_) return fail(error, "Stale plugin generation");
        if (!previous_) return fail(error, "No previous plugin release");
        if (generation_ == std::numeric_limits<std::uint64_t>::max()) return fail(error, "Plugin generation exhausted");
        auto restored = std::shared_ptr<const RuntimePluginSnapshot>(new RuntimePluginSnapshot(previous_->image_, generation_ + 1));
        retired = std::move(current_);
        current_ = std::move(restored);
        previous_.reset(); // current_ retains its image, so this cannot unload it.
        ++generation_;
    }
    if (error) error->clear();
    return true;
}
bool RuntimePluginStore::clear(std::uint64_t expected, std::string* error) {
    std::shared_ptr<const RuntimePluginSnapshot> current, previous;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (expected != generation_) return fail(error, "Stale plugin generation");
        if (!current_) return fail(error, "No active plugin");
        if (generation_ == std::numeric_limits<std::uint64_t>::max()) return fail(error, "Plugin generation exhausted");
        current = std::move(current_);
        previous = std::move(previous_);
        ++generation_;
    }
    if (error) error->clear();
    return true;
}

} }
