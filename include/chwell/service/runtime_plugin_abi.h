#ifndef CHWELL_RUNTIME_PLUGIN_ABI_H
#define CHWELL_RUNTIME_PLUGIN_ABI_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define CHWELL_PLUGIN_EXPORT __declspec(dllexport)
#define CHWELL_PLUGIN_CALL __cdecl
#else
#define CHWELL_PLUGIN_EXPORT __attribute__((visibility("default")))
#define CHWELL_PLUGIN_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define CHWELL_RUNTIME_PLUGIN_ABI_V1 1u
// Stateless byte request/response ABI. No C++ objects or plugin-owned buffers
// cross the boundary. No exceptions, retained buffer pointers, background work,
// host callbacks or external side effects. Calls are serialized per image.
// Return 0 on success; set *written <= capacity. Input/output live for this call.
typedef int (CHWELL_PLUGIN_CALL *ChwellPluginInvokeV1)(
    const uint8_t* input, size_t size, uint8_t* output, size_t capacity, size_t* written);

typedef struct ChwellRuntimePluginV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    const char* name; // Stable ASCII identity, 1..128 bytes, [A-Za-z0-9_.-].
    uint64_t version; // Nonzero release number, increasing within a host.
    ChwellPluginInvokeV1 invoke;
    uint64_t reserved; // Must be zero.
} ChwellRuntimePluginV1;

// Export this function; it returns immutable metadata valid until library unload.
typedef const ChwellRuntimePluginV1* (CHWELL_PLUGIN_CALL *ChwellPluginDescriptorV1)(void);
CHWELL_PLUGIN_EXPORT const ChwellRuntimePluginV1* CHWELL_PLUGIN_CALL chwell_runtime_plugin_v1(void);

#ifdef __cplusplus
}
#endif
#endif
