/* Pure controller-slot packing shared by both co-op hosts. */
#pragma once

#include <stdint.h>

/**
 * Map a physical/source controller id to its compact id on one host.
 * @p routed_mask has one bit per source controller assigned to that host.
 * Returns -1 when the controller is not routed there.
 */
int coop_gamepad_compact_slot(uint16_t routed_mask, int source_slot);

/** Reverse coop_gamepad_compact_slot(), or -1 for an invalid compact slot. */
int coop_gamepad_source_slot(uint16_t routed_mask, int compact_slot);

/**
 * Filter @p active_mask to controllers routed to one host, then pack those
 * bits into the same stable slot space returned by coop_gamepad_compact_slot().
 */
uint16_t coop_gamepad_compact_mask(uint16_t routed_mask, uint16_t active_mask);
