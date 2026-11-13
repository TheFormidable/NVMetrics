#!/usr/bin/env python3
"""extract_rm_layer.py <driver checkout> <out dir>

Rebuilds the NullMoth driver's resource-manager layer (nvRmApi.c/.h, nvrm_xnu.c/.h, nvrm_surfshare_abi.h and the
nvmtl_cfg.h they include) from the driver's own NVK patch chain, so nvmetrics-agent links the same code the driver uses
without building the whole patched Mesa tree. The chain and its order are read from the driver's build/build_nvk.sh
(release 2.9); only the hunks of those files are applied, in that order, with patch(1). Nothing is copied into
this repository: the files are made fresh from the checkout each build. Python 3.9+, standard library only."""
import os
import re
import subprocess
import sys
import tempfile

# paths under src/nouveau/vulkan/, kept as they are in Mesa so the files' relative includes still resolve:
# <out>/nvkmd/nvrm/*.c and <out>/nvmtl_cfg.h
WANT = ("nvkmd/nvrm/nvRmApi.c", "nvkmd/nvrm/nvRmApi.h", "nvkmd/nvrm/nvrm_xnu.c", "nvkmd/nvrm/nvrm_xnu.h",
        "nvkmd/nvrm/nvrm_surfshare_abi.h", "nvmtl_cfg.h")
PREFIX = "src/nouveau/vulkan/"


def chain(driver):
    text = open(os.path.join(driver, "build", "build_nvk.sh"), encoding="utf-8").read()
    m = re.search(r'^\s*2\.9\)\s*FIXES=\$NVK/(\S+)\s*\n\s*CHAIN="([^"]+)"', text, re.M)
    if not m:
        sys.exit("extract_rm_layer: cannot find the 2.9 patch chain in build/build_nvk.sh")
    fixes, names = m.group(1), m.group(2).split()
    return [fixes if n == "@FIXES" else n + ".patch" for n in names]


def sections(lines):
    """(target path, lines) per file section; a section starts at a '--- ' line followed by '+++ '"""
    out, cur, path = [], None, None
    i = 0
    while i < len(lines):
        ln = lines[i]
        if ln.startswith("--- ") and i + 1 < len(lines) and lines[i + 1].startswith("+++ "):
            if cur is not None:
                out.append((path, cur))
            tgt = lines[i + 1][4:].split("\t")[0].strip()
            path = tgt[2:] if tgt.startswith("b/") else tgt
            cur = [ln, lines[i + 1]]
            i += 2
            continue
        if ln.startswith("diff ") and cur is not None:
            out.append((path, cur))
            cur, path = None, None
        elif cur is not None:
            cur.append(ln)
        i += 1
    if cur is not None:
        out.append((path, cur))
    return out


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    driver, out = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    nvk = os.path.join(driver, "nvk")
    work = tempfile.mkdtemp(prefix="nvmetrics-rm.")
    applied = 0
    for name in chain(driver):
        lines = open(os.path.join(nvk, name), encoding="utf-8", errors="surrogateescape").read().splitlines(True)
        keep = [body for path, body in sections(lines)
                if path and path.startswith(PREFIX) and path[len(PREFIX):] in WANT]
        if not keep:
            continue
        mini = "".join("".join(b) for b in keep)
        r = subprocess.run(["patch", "-p1", "-s", "--no-backup-if-mismatch", "-d", work],
                           input=mini.encode("utf-8", "surrogateescape"), capture_output=True)
        if r.returncode != 0:
            sys.exit("extract_rm_layer: %s does not apply:\n%s%s" % (name, r.stdout.decode(), r.stderr.decode()))
        applied += 1
    os.makedirs(out, exist_ok=True)
    for f in WANT:
        src = os.path.join(work, PREFIX, f)
        if not os.path.isfile(src):
            sys.exit("extract_rm_layer: the chain did not produce %s" % f)
        os.makedirs(os.path.dirname(os.path.join(out, f)), exist_ok=True)
        with open(src, "rb") as a, open(os.path.join(out, f), "wb") as b:
            b.write(a.read())
    print("extract_rm_layer: %d files from %d patches -> %s" % (len(WANT), applied, out))


if __name__ == "__main__":
    main()
