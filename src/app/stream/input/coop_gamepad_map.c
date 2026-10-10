#include "coop_gamepad_map.h"

int coop_gamepad_compact_slot(uint16_t routed_mask, int source_slot) {
    if (source_slot < 0 || source_slot >= 16 ||
        (routed_mask & (uint16_t) (1u << source_slot)) == 0) {
        return -1;
    }

    int compact = 0;
    for (int source = 0; source < source_slot; ++source) {
        if ((routed_mask & (uint16_t) (1u << source)) != 0) {
            compact++;
        }
    }
    return compact;
}

int coop_gamepad_source_slot(uint16_t routed_mask, int compact_slot) {
    if (compact_slot < 0 || compact_slot >= 16) {
        return -1;
    }

    int compact = 0;
    for (int source = 0; source < 16; ++source) {
        if ((routed_mask & (uint16_t) (1u << source)) == 0) {
            continue;
        }
        if (compact == compact_slot) {
            return source;
        }
        compact++;
    }
    return -1;
}

uint16_t coop_gamepad_compact_mask(uint16_t routed_mask, uint16_t active_mask) {
    uint16_t compact_mask = 0;
    int compact = 0;
    for (int source = 0; source < 16; ++source) {
        const uint16_t source_bit = (uint16_t) (1u << source);
        if ((routed_mask & source_bit) == 0) {
            continue;
        }
        if ((active_mask & source_bit) != 0) {
            compact_mask |= (uint16_t) (1u << compact);
        }
        compact++;
    }
    return compact_mask;
}
