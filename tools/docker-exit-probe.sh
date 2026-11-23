#!/bin/zsh
# docker-exit-probe.sh [seconds=60] [label=docker]: what the running Docker VM costs the host under RyzenHV. No root.
# Reads RyzenHV's counters (rhv-info stats) and the CPU's RAPL energy / Tctl (debug.amdcppc.sensors) at the start and the end
# of the window, and a top of the VM processes over the same window. Prints rates per second:
#   exits       every #VMEXIT (also host interrupts that hit while the guest ran)
#   to user     vmTraps: exits handed to the VMM (HyperKit): APIC MMIO (as NPF), HLT, IN/OUT, MSR, CPUID, time slices
#   npf user    vmNptFaults: nested page faults handed to the VMM; under HyperKit these are xAPIC register accesses
#   in kernel   exits - to user (host interrupts, kicks, lazy faults)
#   us/run      pre / guest+switch / post per VMRUN, front end in/out per trap (TSC cycles -> us)
# Run while Docker Desktop is up; nothing is changed.
T=${1:-60}; label=${2:-docker}
RHV=${RHV_INFO:-$HOME/develop/projects/rhv-wt-guest-mem/build/rhv-info}
tsc=$(sysctl -n machdep.tsc.frequency)
snap() { $RHV stats | awk -F' *: *' 'NF==2{gsub(/ /,"",$1); print $1"="$2}'; }
energy() { sysctl -n debug.amdcppc.sensors | sed -E 's/.*rapl=([0-9a-f]+)\/([0-9a-f]+).*/\2/'; }
tctl() { sysctl -n debug.amdcppc.sensors | sed -E 's/.*tctl=([0-9]+).*/\1/'; }
now() { perl -MTime::HiRes=time -e 'printf "%.3f\n", time'; }
unit=$(sysctl -n debug.amdcppc.sensors | sed -E 's/.*rapl=([0-9a-f]+)\/.*/\1/')
typeset -A A B
for kv in $(snap); do A[${kv%%=*}]=${kv#*=}; done; e0=$((16#$(energy))); t0=$(now); c0=$(tctl)
top -l 2 -s $T -o cpu -n 12 -stats pid,command,cpu,threads > /tmp/docker-exit-probe.top.$$ 2>&1
for kv in $(snap); do B[${kv%%=*}]=${kv#*=}; done; e1=$((16#$(energy))); t1=$(now); c1=$(tctl)
dt=$(echo "$t1 - $t0" | bc -l)
r() { echo "scale=0; (${B[$1]} - ${A[$1]}) / $dt" | bc -l; }
per() { local n=$((${B[$2]} - ${A[$2]})); if (( n > 0 )); then echo "scale=3; (${B[$1]} - ${A[$1]}) / $n / ($tsc / 1000000)" | bc -l; else echo n/a; fi; }
# RAPL energy unit: ESU in bits 12:8 of the unit register (AMD), 32-bit counter
esu=$(( (16#$unit >> 8) & 0x1f )); de=$(( (e1 - e0) & 0xffffffff ))
pw=$(echo "scale=1; $de / (2 ^ $esu) / $dt" | bc -l)
ex=$(r vmExits); tr=$(r vmTraps); npf=$(r vmNptFaults)
echo "== docker-exit-probe '$label' $(date '+%F %T') window ${dt}s"
echo "package ${pw} W, Tctl $((c0/1000)) -> $((c1/1000)) C"
echo "exits/s ${ex}   to user/s ${tr}   npf to user/s ${npf}   other to user/s $((tr - npf))   in kernel/s $((ex - tr))"
echo "per VMRUN us: pre $(per vmCycPre vmProfExits)  guest+switch $(per vmCycGuest vmProfExits)  post $(per vmCycPost vmProfExits)"
echo "per trap us: front end in $(per vmCycFeIn vmTraps)  out $(per vmCycFeOut vmTraps)  whole trap $(per vmCycTrap vmTraps)"
echo "tlb flushes/s $(r vmTlbFlushes)  new ASIDs/s $(r vmAsidNew)  lazy faults/s $(r vmLazyFaults)  reclaim evicted/s $(r vmReclaimEvicted)"
echo; echo "top (${T}s average):"; awk '/^PID/{n++} n==2' /tmp/docker-exit-probe.top.$$; rm -f /tmp/docker-exit-probe.top.$$
