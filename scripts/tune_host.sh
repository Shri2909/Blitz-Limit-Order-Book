#!/usr/bin/env bash
# Runtime host tuning for benchmarks. Run with sudo. Settings reset on reboot.
# Usage: sudo scripts/tune_host.sh [pinned cores, default "4,5"]
set -u
CORES="${1:-4,5}"
NCPU=$(nproc --all)
OTHER=$(python3 - "$CORES" "$NCPU" <<'PY'
import sys
pinned = {int(c) for c in sys.argv[1].split(",")}
print(",".join(str(c) for c in range(int(sys.argv[2])) if c not in pinned))
PY
)

# power-profiles-daemon is D-Bus activated and resets governors, so mask it until reboot.
systemctl mask --runtime --now power-profiles-daemon 2>/dev/null
systemctl stop irqbalance cron atd unattended-upgrades 2>/dev/null
cpupower -c "$CORES" frequency-set --governor performance
for c in ${CORES//,/ }; do
    echo performance > "/sys/devices/system/cpu/cpu$c/cpufreq/energy_performance_preference" 2>/dev/null
done
echo 0 > /proc/sys/kernel/nmi_watchdog
for f in /proc/irq/[0-9]*/smp_affinity_list; do echo "$OTHER" > "$f" 2>/dev/null; done

echo "isolated:  $(cat /sys/devices/system/cpu/isolated)"
for c in ${CORES//,/ }; do
    echo "cpu$c: governor=$(cat /sys/devices/system/cpu/cpu$c/cpufreq/scaling_governor)" \
         "siblings=$(cat /sys/devices/system/cpu/cpu$c/topology/thread_siblings_list)" \
         "L2 shared with=$(cat /sys/devices/system/cpu/cpu$c/cache/index2/shared_cpu_list)"
done
echo "IRQs still on pinned cores (managed IRQs such as NVMe may remain):"
grep -H . /proc/irq/*/effective_affinity_list | grep -E ":($(echo "$CORES" | tr ',' '|'))\$" || echo "  none"
