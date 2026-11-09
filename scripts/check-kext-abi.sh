#!/bin/bash
#
# Fails when the object files linked into the kext disagree about the size of a structure, or when rhv::gStats is smaller than
# RHVStats.
#
# Why: 0.7.18 was linked from objects compiled against two versions of include/RyzenHVShared.h (the Makefile had no header
# dependencies). Log.cpp kept a 200-byte gStats, the new code wrote 256 bytes into it over the metaclass objects that follow, and
# the Mac panicked twice. Every object records {type, sizeof} in __DATA,__rhv_abi (kext/Source/AbiStamp.hpp); this compares them.
#
#   ./scripts/check-kext-abi.sh build/RyzenHV.kext

set -eu

KEXT="${1:-}"
if [ -z "$KEXT" ] || [ ! -d "$KEXT" ]; then
    echo "usage: $0 <path to .kext>" >&2
    exit 2
fi
EXE="$KEXT/Contents/MacOS/$(plutil -extract CFBundleExecutable raw -o - "$KEXT/Contents/Info.plist")"

/usr/bin/python3 -I - "$EXE" <<'PY'
import re, subprocess, sys
from collections import defaultdict

exe = sys.argv[1]
out = subprocess.run(["otool", "-X", "-s", "__DATA", "__rhv_abi", exe], capture_output=True, text=True, check=True).stdout
data = bytearray()
for line in out.splitlines():
    parts = line.split("\t", 1)
    if len(parts) != 2:
        continue
    for tok in parts[1].split():
        if re.fullmatch(r"[0-9a-fA-F]{8}", tok):      # otool prints 32-bit little-endian words for this section
            data += int(tok, 16).to_bytes(4, "little")
        elif re.fullmatch(r"[0-9a-fA-F]{2}", tok):
            data += bytes([int(tok, 16)])
if not data or len(data) % 64:
    print(f"check-kext-abi: no usable __DATA,__rhv_abi section in {exe} ({len(data)} bytes)", file=sys.stderr)
    sys.exit(1)

sizes = defaultdict(lambda: defaultdict(int))
for off in range(0, len(data), 64):
    rec = data[off:off + 64]
    name = rec[:56].split(b"\0", 1)[0].decode()
    sizes[name][int.from_bytes(rec[56:64], "little")] += 1

bad = {n: dict(v) for n, v in sizes.items() if len(v) != 1}
if bad:
    print("ERROR: the kext's object files were compiled against different layouts (stale objects, rebuild clean):", file=sys.stderr)
    for n, v in sorted(bad.items()):
        print(f"    {n}: " + ", ".join(f"{s} bytes in {c} objects" for s, c in sorted(v.items())), file=sys.stderr)
    sys.exit(1)

stats = next(iter(sizes["RHVStats"])) if "RHVStats" in sizes else None
syms = []
for line in subprocess.run(["nm", "-n", exe], capture_output=True, text=True, check=True).stdout.splitlines():
    f = line.split()
    if len(f) == 3:
        syms.append((int(f[0], 16), f[2]))
idx = next((i for i, s in enumerate(syms) if s[1] == "__ZN3rhv6gStatsE"), None)
if stats is None or idx is None or idx + 1 >= len(syms):
    print("ERROR: cannot find RHVStats in the stamps or rhv::gStats in the symbol table", file=sys.stderr)
    sys.exit(1)
room = syms[idx + 1][0] - syms[idx][0]
if room < stats:
    print(f"ERROR: rhv::gStats has {room} bytes before {syms[idx + 1][1]}, RHVStats is {stats}: the counters would overwrite it",
          file=sys.stderr)
    sys.exit(1)
nobj = max(sum(v.values()) for v in sizes.values())
print(f"check-kext-abi: {len(sizes)} types agree across {nobj} objects; gStats has {room} bytes for a {stats}-byte RHVStats")
PY
