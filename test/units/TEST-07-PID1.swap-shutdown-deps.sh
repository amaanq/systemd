#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
set -eux
set -o pipefail

# Swap files are left to systemd-shutdown instead of being stopped by umount.target, see #2930.

if systemd-detect-virt --container; then
    echo "Swap units get no default dependencies in containers, skipping."
    exit 0
fi

at_exit() {
    set +e

    rm -f /run/systemd/system/{var-tmp-testswap,dev-testswap,var-tmp-netswap}.swap
    systemctl daemon-reload
}

trap at_exit EXIT

mkdir -p /run/systemd/system

cat >/run/systemd/system/var-tmp-testswap.swap <<EOF
[Swap]
What=/var/tmp/testswap
EOF

cat >/run/systemd/system/dev-testswap.swap <<EOF
[Swap]
What=/dev/testswap
EOF

cat >/run/systemd/system/var-tmp-netswap.swap <<EOF
[Swap]
What=/var/tmp/netswap
Options=_netdev
EOF

systemctl daemon-reload

has_dep() {
    [[ " $(systemctl show -P "$2" "$1") " == *" $3 "* ]]
}

has_dep var-tmp-testswap.swap Before umount.target
(! has_dep var-tmp-testswap.swap Conflicts umount.target)
has_dep var-tmp-testswap.swap Conflicts soft-reboot.target

has_dep dev-testswap.swap Before umount.target
has_dep dev-testswap.swap Conflicts umount.target
(! has_dep dev-testswap.swap Conflicts soft-reboot.target)

has_dep var-tmp-netswap.swap Before umount.target
has_dep var-tmp-netswap.swap Conflicts umount.target
