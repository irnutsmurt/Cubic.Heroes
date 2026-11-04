"""R1: the executor depends on no part of the PS3 graphics runtime.

r1_check_deps.py <build dir>

Checks r1_replay.exe, built in <build dir> (tools/r1_dev_lift.sh's
build/r1/bld), three ways:
  link inputs   the objects and libraries build.ninja links it from: its own
                two files, the program translators (asset conversion) and
                glslang, nothing else;
  references    every external symbol those objects use (their COFF symbol
                tables), none of the runtime's: the FIFO walker and GCM HLE
                (cellGcm*, gcm_*), the RSX state (rsx_dsp_*, rsx_process_*,
                rsx_state, rsx_draw_engine*), the Vulkan backend (vk_eng_*),
                guest memory (vm_*), the PPU and SPU runtimes (ppu_*, spu_*,
                ps3_hle*);
  the binary    its imported DLLs, and none of the runtime's own log strings
                ("[cellGcmSys]", "FIFO resync", "[RSX vulkan]", ...).
Writes <build dir>/r1_deps.json; exits 1 on any finding.
"""
import json
import os
import re
import struct
import sys

ALLOWED_OBJ = ("r1_replay.c.obj", "r1_json.c.obj", "rsx_fp_decompiler.c.obj", "rsx_vp_decompiler.c.obj",
               "rsx_shader_spirv.cpp.obj")
ALLOWED_LIB = ("libglslang.a", "libSPIRV.a", "libglslang-default-resource-limits.a")
FORBIDDEN = re.compile(r"^_?(cellGcm|gcm_|rsx_dsp_|rsx_process|rsx_state|rsx_draw_engine|rsx_dispatch|vk_eng_|"
                       r"vm_read|vm_write|vm_base|ppu_|spu_|ps3_hle|lv2_|s_fifo|cellResc|rsx_vulkan|rsx_null)")
RUNTIME_STRINGS = [b"[cellGcmSys]", b"FIFO resync", b"[RSX vulkan]", b"[rsx engine]", b"[cellResc]",
                   b"[release-path]", b"GCM_SEMA_ACQUIRE", b"[r1] consumption capture"]


def coff_undefined(path):
    """External symbols an object file references but does not define."""
    b = open(path, "rb").read()
    sig1, sig2 = struct.unpack_from("<HH", b, 0)
    if sig1 == 0 and sig2 == 0xFFFF:   # bigobj: sections, symbol table, symbols at 44
        _, symptr, nsym = struct.unpack_from("<III", b, 44)
        ssize, secoff = 20, 4
    else:                              # IMAGE_FILE_HEADER: symbol table and count at 8
        symptr, nsym = struct.unpack_from("<II", b, 8)
        ssize, secoff = 18, 2
    strtab = symptr + nsym * ssize
    out = []
    i = 0
    while i < nsym:
        o = symptr + i * ssize
        raw = b[o:o + 8]
        if raw[:4] == b"\0\0\0\0":
            so = struct.unpack_from("<I", raw, 4)[0]
            name = b[strtab + so:b.index(b"\0", strtab + so)].decode(errors="replace")
        else:
            name = raw.rstrip(b"\0").decode(errors="replace")
        if secoff == 4:
            sec = struct.unpack_from("<i", b, o + 12)[0]
            cls, naux = b[o + 18], b[o + 19]
        else:
            sec = struct.unpack_from("<h", b, o + 12)[0]
            cls, naux = b[o + 16], b[o + 17]
        if sec == 0 and cls == 2:
            out.append(name)
        i += 1 + naux
    return sorted(set(out))


def pe_imports(path):
    b = open(path, "rb").read()
    pe = struct.unpack_from("<I", b, 0x3C)[0]
    nsec = struct.unpack_from("<H", b, pe + 6)[0]
    optsz = struct.unpack_from("<H", b, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", b, opt)[0]
    dd = opt + (112 if magic == 0x20B else 96)
    imp_rva = struct.unpack_from("<I", b, dd + 8)[0]
    secs = []
    for k in range(nsec):
        s = opt + optsz + k * 40
        va, vsz, rawp = struct.unpack_from("<III", b, s + 12)[0], struct.unpack_from("<I", b, s + 8)[0], \
            struct.unpack_from("<I", b, s + 20)[0]
        secs.append((va, vsz, rawp))

    def off(rva):
        for va, vsz, rawp in secs:
            if va <= rva < va + max(vsz, 1):
                return rawp + rva - va
        return None
    dlls = []
    p = off(imp_rva)
    while p is not None:
        name_rva = struct.unpack_from("<I", b, p + 12)[0]
        if not name_rva:
            break
        no = off(name_rva)
        dlls.append(b[no:b.index(b"\0", no)].decode())
        p += 20
    return dlls, b


def main():
    bld = sys.argv[1]
    ninja = open(os.path.join(bld, "build.ninja"), errors="replace").read()
    m = re.search(r"^build r1_replay\.exe: \S+ (.*?)(?: \| (.*?))?(?: \|\| .*)?$", ninja, re.M)
    if not m:
        sys.exit("no r1_replay.exe link line in build.ninja")
    objs = [x.replace("$ ", " ").replace("$:", ":") for x in re.split(r"(?<!\$) ", m.group(1)) if x]
    libs = [x.replace("$ ", " ").replace("$:", ":") for x in re.split(r"(?<!\$) ", m.group(2) or "") if x]
    findings = []
    for o in objs:
        if not o.endswith(ALLOWED_OBJ):
            findings.append(f"link input {o}")
    for l in libs:
        if not os.path.basename(l) in ALLOWED_LIB:
            findings.append(f"link library {l}")
    refs = {}
    for o in objs:
        und = coff_undefined(os.path.join(bld, o))
        refs[os.path.basename(o)] = len(und)
        for s in und:
            if FORBIDDEN.match(s):
                findings.append(f"{os.path.basename(o)} references {s}")
    exe = os.path.join(bld, "r1_replay.exe")
    dlls, data = pe_imports(exe)
    for s in RUNTIME_STRINGS:
        if s in data:
            findings.append(f"the binary holds the runtime's string {s.decode()!r}")
    res = {"executable": exe, "link_objects": [os.path.basename(o) for o in objs],
           "link_libraries": [os.path.basename(l) for l in libs],
           "external_references_per_object": refs, "imported_dlls": dlls,
           "runtime_strings_checked": [s.decode() for s in RUNTIME_STRINGS],
           "forbidden_symbol_pattern": FORBIDDEN.pattern, "findings": findings, "ok": not findings}
    with open(os.path.join(bld, "r1_deps.json"), "w") as f:
        json.dump(res, f, indent=1)
    print(json.dumps(res, indent=1))
    sys.exit(0 if not findings else 1)


if __name__ == "__main__":
    main()
