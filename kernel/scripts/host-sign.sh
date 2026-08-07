#!/usr/bin/env bash
# Sign the built modules with a local MOK for Secure Boot hosts (max2).
# Generates the key on first run and stages MOK enrollment; the enrollment
# itself must be confirmed at the physical console on next reboot
# (MokManager prompts for the password chosen here).
set -euo pipefail

KREL=$(uname -r)
SRC=${SRC:-$HOME/src/linux-stable}
MOKDIR=$HOME/mok

if [ ! -f "$MOKDIR/mok.key" ]; then
    mkdir -p "$MOKDIR"; chmod 700 "$MOKDIR"
    openssl req -new -x509 -newkey rsa:2048 -nodes -days 36500 \
        -subj "/CN=strix-rdma module signing/" \
        -keyout "$MOKDIR/mok.key" -out "$MOKDIR/mok.crt"
    openssl x509 -in "$MOKDIR/mok.crt" -outform DER -out "$MOKDIR/mok.der"
    echo "== new MOK generated at $MOKDIR"
    echo "== enroll it (choose a one-time password, then confirm at console on reboot):"
    echo "   sudo mokutil --import $MOKDIR/mok.der"
fi

cd "$SRC"
for ko in drivers/thunderbolt/thunderbolt.ko \
          drivers/thunderbolt/thunderbolt_stream.ko \
          drivers/net/thunderbolt/thunderbolt_net.ko; do
    ./scripts/sign-file sha256 "$MOKDIR/mok.key" "$MOKDIR/mok.crt" "$ko"
    echo "signed: $ko"
done
echo "== note: sign BEFORE host-install.sh (modules_install compresses; it"
echo "   installs whatever is in the build dir at that moment)"
