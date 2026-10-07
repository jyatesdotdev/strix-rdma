#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Emit the opt-in follow-on patch, without a Git index or commits.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cat <<'PATCH'
From: strix-rdma contributors <devnull@localhost>
Subject: [PATCH 2/2] RDMA: add opt-in experimental native NHI software verbs

Apply after the v7.1 backport, zero-copy series20, and the opt-in native
path-lifetime core patch. This patch only adds the new native driver. Default off; no module-device alias; enable=1 is required.
This implementation is experimental and has not passed hardware/DS4 gates.
Canonical source and validation boundaries are in kernel/verbs/README.md.

---
diff --git a/drivers/infiniband/Kconfig b/drivers/infiniband/Kconfig
--- a/drivers/infiniband/Kconfig
+++ b/drivers/infiniband/Kconfig
@@ -112,3 +112,5 @@
 source "drivers/infiniband/ulp/rtrs/Kconfig"
 
+source "drivers/infiniband/hw/strix_nhi/Kconfig"
+
 endif # INFINIBAND
diff --git a/drivers/infiniband/Makefile b/drivers/infiniband/Makefile
--- a/drivers/infiniband/Makefile
+++ b/drivers/infiniband/Makefile
@@ -3,3 +3,4 @@
 obj-$(CONFIG_INFINIBAND)		+= hw/
 obj-$(CONFIG_INFINIBAND)		+= ulp/
 obj-$(CONFIG_INFINIBAND)		+= sw/
+obj-$(CONFIG_INFINIBAND_STRIX_NHI) += hw/strix_nhi/
PATCH
for file in Kconfig Makefile native_wire.h native_order.h native_platform.h strix_nhi.h main.c ring.c verbs.c mr.c protocol.c; do
    dest=drivers/infiniband/hw/strix_nhi/$file
    lines=$(wc -l < "$here/$file" | tr -d ' ')
    printf 'diff --git a/%s b/%s\nnew file mode 100644\n--- /dev/null\n+++ b/%s\n@@ -0,0 +1,%s @@\n' "$dest" "$dest" "$dest" "$lines"
    sed 's/^/+/' "$here/$file"
done
