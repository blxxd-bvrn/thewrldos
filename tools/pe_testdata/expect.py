#!/usr/bin/env python3
"""Turns `objdump -p` output into the expectations rust/src/pe.rs's tests read:
   base HEX / entry HEX / size HEX / subsystem N
   export NAME RVA | export #ORD RVA | forward NAME TARGET
   import DLL NAME | import DLL #ORD            (DLL in lower case)"""
import re, sys
text = open(sys.argv[1]).read()
out = []
for key, name in (("ImageBase", "base"), ("AddressOfEntryPoint", "entry"), ("SizeOfImage", "size")):
    m = re.search(r"^%s\s+([0-9a-fA-F]+)" % key, text, re.M)
    out.append("%s %x" % (name, int(m.group(1), 16)))
out.append("subsystem %d" % int(re.search(r"^Subsystem\s+([0-9a-fA-F]+)", text, re.M).group(1), 16))
m = re.search(r"Export Address Table -- Ordinal Base (\d+)\n(.*?)\n\n", text, re.S)
if m:
    base = int(m.group(1)); rvas = {}
    for idx, ordv, rva, kind in re.findall(r"\[\s*(\d+)\] \+base\[\s*(\d+)\]\s+([0-9a-f]+) (Export|Forwarder) RVA", m.group(2)):
        rvas[int(idx)] = (int(rva, 16), kind)
        if kind == "Export": out.append("export #%d %x" % (int(ordv), int(rva, 16)))
    for fidx, tgt in re.findall(r"\[\s*(\d+)\] \+base\[\s*\d+\]\s+[0-9a-f]+ Forwarder RVA -- (\S+)", m.group(2)):
        rvas[int(fidx)] = (rvas[int(fidx)][0], "Forwarder", tgt)
    names = re.search(r"\[Ordinal/Name Pointer\] Table\n(.*?)\n\n", text, re.S)
    for idx, nm in re.findall(r"\[\s*(\d+)\] (\S+)", names.group(1)):
        e = rvas[int(idx)]
        out.append("forward %s %s" % (nm, e[2]) if e[1] == "Forwarder" else "export %s %x" % (nm, e[0]))
imp = re.search(r"The Import Tables.*?\n(.*?)(?:\nThe |\Z)", text, re.S)
if imp:
    dll = None
    for line in imp.group(1).splitlines():
        d = re.match(r"\s+DLL Name: (\S+)", line)
        if d: dll = d.group(1).lower(); continue
        e = re.match(r"\s+([0-9a-f]+)\s+(\d+)\s+(\S+)$", line)
        if e and dll:
            out.append("import %s %s" % (dll, "#%d" % int(e.group(2)) if e.group(3) == "<none>" else e.group(3)))
print("\n".join(out))
