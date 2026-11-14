#!/usr/bin/env python3
"""shell_check.py - is a new hardware build a bitstream-only update for the Linux image?

The PetaLinux image depends on three things from the XSA:
  * the PS7 configuration (FSBL ps7_init: clocks, DDR, MIO/EMIO, HP ports, interrupts enable)
  * the address map seen by the PS (device-tree nodes and their reg ranges)
  * the fabric interrupt wiring (device-tree interrupt numbers)
If all three match the frozen shell, the new bitstream can be loaded at run time through the
FPGA manager (or swapped into BOOT.BIN) with no PetaLinux build. Otherwise the report says
which rebuild is needed.

  python3 tools/shell_check.py freeze zed_base/system_wrapper.xsa tools/shell_v7.json
  python3 tools/shell_check.py check  zed_base/system_wrapper.xsa tools/shell_v7.json

Exit status of `check`: 0 = bitstream-only update, 1 = rebuild needed, 2 = usage/parse error.
"""

import hashlib
import json
import re
import sys
import zipfile
import xml.etree.ElementTree as ET


def read_xsa(path):
    z = zipfile.ZipFile(path)
    names = z.namelist()
    hwhs = [n for n in names if n.endswith(".hwh")]
    top = None
    for n in hwhs:                     # the top-level hwh contains the processing_system7 module
        root = ET.fromstring(z.read(n))
        if any(m.get("MODTYPE") == "processing_system7" for m in root.iter("MODULE")):
            top = root
            break
    if top is None:
        raise SystemExit("no hwh with a processing_system7 module in " + path)
    init = [n for n in names if n.endswith("ps7_init.c")]
    init_hash = hashlib.sha256(z.read(init[0])).hexdigest() if init else None
    return top, init_hash


def fingerprint(path):
    root, init_hash = read_xsa(path)
    mods = {m.get("INSTANCE"): m for m in root.iter("MODULE")}
    ps = next(m for m in root.iter("MODULE") if m.get("MODTYPE") == "processing_system7")

    # PS7 parameters that shape ps7_init and the device tree (all PCW_* except reports of
    # actual frequencies, which are derived and listed separately by Vivado)
    pcw = {}
    for p in ps.iter("PARAMETER"):
        n = p.get("NAME", "")
        if n.startswith("PCW_") and "_ACT_" not in n and not n.startswith("PCW_PACKAGE"):
            pcw[n] = p.get("VALUE")

    # address map as seen by the PS
    amap = []
    for r in ps.iter("MEMRANGE"):
        amap.append({"instance": r.get("INSTANCE"), "base": r.get("BASEVALUE"), "high": r.get("HIGHVALUE"),
                     "block": r.get("SLAVEBUSINTERFACE")})
    amap.sort(key=lambda d: (int(d["base"], 16) if d["base"] else 0, d["instance"] or ""))

    # fabric interrupts: IRQ_F2P <- concat inputs <- sources
    irqs = []
    port = next((p for p in ps.iter("PORT") if p.get("NAME") == "IRQ_F2P"), None)
    if port is not None:
        conns = [c for c in port.iter("CONNECTION")]
        for c in conns:
            cat = mods.get(c.get("INSTANCE"))
            if cat is None:
                continue
            # widths from the concat parameters, so unconnected inputs keep their bit positions
            par = {q.get("NAME"): q.get("VALUE") for q in cat.iter("PARAMETER")}
            src = {}
            for p in cat.iter("PORT"):
                m = re.fullmatch(r"In(\d+)", p.get("NAME", ""))
                if m:
                    src[int(m.group(1))] = ",".join(sorted(f'{x.get("INSTANCE")}/{x.get("PORT")}'
                                                           for x in p.iter("CONNECTION")))
            n = int(par.get("NUM_PORTS", max(src) + 1 if src else 0))
            bit = 0
            for i in range(n):
                width = int(par.get(f"IN{i}_WIDTH", "1"))
                irqs.append({"bits": f"{bit}" if width == 1 else f"{bit}-{bit + width - 1}",
                             "source": src.get(i) or "unconnected"})
                bit += width
    return {"ps7_init_sha256": init_hash, "pcw": pcw, "address_map": amap, "irq_f2p": irqs}


def compare(new, ref):
    issues = []
    if new["ps7_init_sha256"] != ref["ps7_init_sha256"]:
        issues.append(("PS7 initialisation (ps7_init.c) differs", "boot loader"))
    dp = sorted(k for k in set(new["pcw"]) | set(ref["pcw"]) if new["pcw"].get(k) != ref["pcw"].get(k))
    for k in dp[:40]:
        issues.append((f"PS7 {k}: {ref['pcw'].get(k)} -> {new['pcw'].get(k)}", "boot loader"))
    if len(dp) > 40:
        issues.append((f"... {len(dp) - 40} more PS7 parameters differ", "boot loader"))
    a = {(d["instance"], d["base"], d["high"]) for d in new["address_map"]}
    b = {(d["instance"], d["base"], d["high"]) for d in ref["address_map"]}
    for x in sorted(a - b, key=str):
        issues.append((f"address map: new/changed {x[0]} {x[1]}..{x[2]}", "device tree"))
    for x in sorted(b - a, key=str):
        issues.append((f"address map: removed/changed {x[0]} {x[1]}..{x[2]}", "device tree"))
    if new["irq_f2p"] != ref["irq_f2p"]:
        issues.append((f"fabric interrupts: {ref['irq_f2p']} -> {new['irq_f2p']}", "device tree"))
    return issues


def main():
    if len(sys.argv) != 4 or sys.argv[1] not in ("freeze", "check"):
        print(__doc__)
        sys.exit(2)
    cmd, xsa, ref = sys.argv[1:]
    fp = fingerprint(xsa)
    if cmd == "freeze":
        with open(ref, "w") as f:
            json.dump(fp, f, indent=1, sort_keys=True)
        print(f"shell fingerprint written to {ref}: {len(fp['pcw'])} PS7 parameters, "
              f"{len(fp['address_map'])} address ranges, {len(fp['irq_f2p'])} interrupt inputs")
        for d in fp["address_map"]:
            print(f"  {d['base']}..{d['high']}  {d['instance']}")
        for d in fp["irq_f2p"]:
            print(f"  IRQ_F2P[{d['bits']}] <- {d['source']}")
        return
    issues = compare(fp, json.load(open(ref)))
    if not issues:
        print("SHELL MATCH: bitstream-only update. Load it with the FPGA manager or swap system.bit "
              "in BOOT.BIN; no PetaLinux build.")
        sys.exit(0)
    kinds = {k for _, k in issues}
    print("SHELL CHANGED:")
    for msg, kind in issues:
        print(f"  [{kind}] {msg}")
    if "boot loader" in kinds:
        print("Needs: petalinux-config --get-hw-description=<xsa>, petalinux-build, new BOOT.BIN "
              "(and the device tree).")
    else:
        print("Needs: device tree only (runtime overlay, or petalinux-build -c device-tree and repackage).")
    sys.exit(1)


if __name__ == "__main__":
    main()
