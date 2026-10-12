#include "chwell/service/runtime_plugin_abi.h"
#include <limits.h>

#ifndef RULE_VERSION
#define RULE_VERSION 1
#endif
#ifndef RULE_MULTIPLIER
#define RULE_MULTIPLIER 2
#endif
// Example: request/response is one uint32 in network byte order. Stateless rule
// evaluation owns no player data, callbacks, timers or external resources.
static int CHWELL_PLUGIN_CALL evaluate(const uint8_t* input, size_t size,
    uint8_t* output, size_t capacity, size_t* written) {
    uint32_t value;
    if (size != 4 || capacity < 4) return 1;
    value = ((uint32_t)input[0] << 24) | ((uint32_t)input[1] << 16) |
            ((uint32_t)input[2] << 8) | (uint32_t)input[3];
    if (value > UINT32_MAX / RULE_MULTIPLIER) return 2;
    value *= RULE_MULTIPLIER;
    output[0] = (uint8_t)(value >> 24); output[1] = (uint8_t)(value >> 16);
    output[2] = (uint8_t)(value >> 8); output[3] = (uint8_t)value;
    *written = 4;
    return 0;
}
static const ChwellRuntimePluginV1 descriptor = {CHWELL_RUNTIME_PLUGIN_ABI_V1,
    sizeof(ChwellRuntimePluginV1), "example.damage", RULE_VERSION, evaluate, 0};
CHWELL_PLUGIN_EXPORT const ChwellRuntimePluginV1* CHWELL_PLUGIN_CALL chwell_runtime_plugin_v1(void) {
    return &descriptor;
}
