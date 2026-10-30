#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# llumik only: gpda-combined:trixie with the phase3 directory at /work.
set -eu
cd /work
mkdir -p terminal-matrix
errors='not ok|WARNING:|BUG:|possible circular|inconsistent lock|Internal error:'
args='console=ttyAMA0 kunit.autorun=1 kunit_shutdown=poweroff'
timeout 60 qemu-system-aarch64 -machine virt -cpu cortex-a53 -smp 8 -m 512 \
 -nographic -kernel arm64/arch/arm64/boot/Image \
 -append "$args kunit.filter_glob=mt6797-provider*" >terminal-qemu.log 2>&1
grep 'ok 1 mt6797-provider' terminal-qemu.log
if grep -E "$errors" terminal-qemu.log; then
 exit 1
fi
for pair in 0:0 1:0 0:1 0:2 0:3 0:4 0:5; do
 order=${pair%:*}
 scenario=${pair#*:}
 log=terminal-matrix/$order-$scenario.log
 timeout 45 qemu-system-aarch64 -machine virt -cpu cortex-a53 -smp 8 -m 512 \
  -nographic -kernel arm64/arch/arm64/boot/Image \
  -dtb matrix/matrix-$order-$scenario.dtb \
  -append "$args kunit.filter_glob=mt6797-combined*" >"$log" 2>&1
 grep 'ok 1 mt6797-combined' "$log"
 if grep -E "$errors" "$log"; then
  exit 1
 fi
done
