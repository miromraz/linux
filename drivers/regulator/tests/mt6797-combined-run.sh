#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Execute only on llumik, inside gpda-combined:trixie, /work mounted.
set -eu
cd /work
python3 src/drivers/regulator/tests/mt6797-combined-dt.py virt.dts combined.dts
dtc -I dts -O dtb combined.dts > combined.dtb 2>dt-compile.log
result=0
args='console=ttyAMA0 kunit.autorun=1'
args="$args kunit.filter_glob=mt6797-combined* kunit_shutdown=poweroff"
for pair in 0:0 1:0 0:1 0:2 0:3 0:4 0:5; do
  order=${pair%:*}
  scenario=${pair#*:}
  cp combined.dtb matrix-$order-$scenario.dtb
  fdtput -t i matrix-$order-$scenario.dtb / test-order "$order"
  fdtput -t i matrix-$order-$scenario.dtb / test-scenario "$scenario"
  timeout 45 qemu-system-aarch64 -machine virt -cpu cortex-a53 -smp 8 -m 512 \
    -nographic -kernel "${IMAGE:-arm64/arch/arm64/boot/Image}" \
    -dtb matrix-$order-$scenario.dtb \
    -append "$args" \
    > matrix-$order-$scenario.log 2>&1
  grep 'ok 1 mt6797-combined' matrix-$order-$scenario.log
  if grep -E 'not ok|WARNING:|BUG:|possible circular|inconsistent lock' \
    matrix-$order-$scenario.log; then
    result=1
  fi
done
exit "$result"
