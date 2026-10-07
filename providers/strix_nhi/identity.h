/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef STRIX_NHI_IDENTITY_H
#define STRIX_NHI_IDENTITY_H
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* UNKNOWN is not a match: require the native service's real sysfs driver. */
static bool strix_identity(const char *name, uint32_t abi, uint32_t driver_id,
                           const char *ibdev_path)
{
	const char *digits;
	char path[PATH_MAX], resolved[PATH_MAX];
	const char suffix[] = "/bus/thunderbolt/drivers/strix_nhi";
	size_t n;
	int len;

	if (abi != 1 || driver_id != 0 || strncmp(name, "strix_nhi", 9))
		return false;
	digits = name + 9;
	if (!*digits) return false;
	for (; *digits; digits++)
		if (*digits < '0' || *digits > '9') return false;
	len = snprintf(path, sizeof(path), "%s/device/driver", ibdev_path);
	if (len < 0 || (size_t)len >= sizeof(path) || !realpath(path, resolved))
		return false;
	n = strlen(resolved);
	return n >= sizeof(suffix) - 1 &&
		!strcmp(resolved + n - (sizeof(suffix) - 1), suffix);
}
#endif
