#include "chwell/service/runtime_plugin_abi.h"
#include <atomic>
#include <cstring>
#include <thread>

#ifndef PLUGIN_VERSION
#define PLUGIN_VERSION 1
#endif
#ifndef PLUGIN_ABI
#define PLUGIN_ABI CHWELL_RUNTIME_PLUGIN_ABI_V1
#endif
#ifndef PLUGIN_NAME
#define PLUGIN_NAME "test.logic"
#endif
#ifndef PLUGIN_STRUCT_SIZE
#define PLUGIN_STRUCT_SIZE sizeof(ChwellRuntimePluginV1)
#endif
namespace {
std::atomic<bool> entered{false}, released{false};
int CHWELL_PLUGIN_CALL invoke(const uint8_t* input, size_t size, uint8_t* output, size_t capacity, size_t* written) {
    if (size && input[0] == 'B') {
        entered = true;
        while (!released.load()) std::this_thread::yield(); // Test-only deterministic in-flight call.
    }
    if (size && input[0] == 'E') return 7;
    if (size && input[0] == 'L') { *written = capacity + 1; return 0; }
    if (capacity < size + 1) return 8;
    output[0] = PLUGIN_VERSION;
    if (size) std::memcpy(output + 1, input, size);
    *written = size + 1;
    return 0;
}
const ChwellRuntimePluginV1 descriptor{PLUGIN_ABI, PLUGIN_STRUCT_SIZE, PLUGIN_NAME,
    PLUGIN_VERSION, invoke, 0};
}
extern "C" CHWELL_PLUGIN_EXPORT const ChwellRuntimePluginV1* CHWELL_PLUGIN_CALL chwell_runtime_plugin_v1() {
    return &descriptor;
}
// Private test-only gates, never part of the runtime plugin ABI.
extern "C" CHWELL_PLUGIN_EXPORT int CHWELL_PLUGIN_CALL chwell_test_entered() { return entered.load() ? 1 : 0; }
extern "C" CHWELL_PLUGIN_EXPORT void CHWELL_PLUGIN_CALL chwell_test_release() { released = true; }
