/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef STRIX_NHI_NATIVE_PLATFORM_H
#define STRIX_NHI_NATIVE_PLATFORM_H
#include "native_wire.h"

/* v7.1.5 icm_probe has no case for these Strix Halo device IDs; nhi_select_cm
 * therefore always selects software CM. Firmware CM can disconnect other
 * services' paths, so widening this predicate requires a fresh core audit. */
static inline int sn_supported_nhi(sn_u32 vendor, sn_u32 device)
{
	return vendor == 0x1022 && (device == 0x158d || device == 0x158e);
}
#endif
