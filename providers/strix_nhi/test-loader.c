// SPDX-License-Identifier: GPL-2.0-only
#include <dlfcn.h>
#include <stdio.h>
int main(int argc, char **argv)
{
	if (argc != 2) return 2;
	if (!dlopen(argv[1], RTLD_NOW | RTLD_LOCAL)) {
		fprintf(stderr, "provider dlopen failed: %s\n", dlerror());
		return 1;
	}
	/* Registered providers live for the process lifetime. No device is opened. */
	puts("ok - real provider plugin dlopen/registration and symbol resolution; no device discovery claim");
	return 0;
}
