// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <unistd.h>
#include "identity.h"
int main(int argc, char **argv)
{
	char driver[PATH_MAX];
	assert(argc == 2);
	assert(strix_identity("strix_nhi0", 1, 0, argv[1]));
	assert(snprintf(driver, sizeof(driver), "%s/device/driver", argv[1]) < (int)sizeof(driver));
	assert(!unlink(driver));
	assert(!symlink("/tmp", driver));
	assert(!strix_identity("strix_nhi0", 1, 0, argv[1]));
	puts("ok - UNKNOWN/name/ABI alone cannot bind an unrelated parent driver");
	return 0;
}
