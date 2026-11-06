#!/usr/bin/env python3
"""R7's link evidence (docs/NATIVE_R7.md): which functions of the PS3 graphics
command path an executable contains, read from its PDB through DbgHelp --
every function symbol whose name matches one of the patterns below, counted
by group. A native release build (DOTHEROES_NATIVE_RELEASE) must contain none
of the command path's groups; the development build has all of them.

  tools/r7_symbols.py <dotheroes.exe> [...]        (its .pdb beside it)
  tools/r7_symbols.py --list <group> <exe>          (the symbols of one group)
"""
import ctypes
import ctypes.wintypes as wt
import fnmatch
import sys

# the command path, by part (the names as the PDB has them)
GROUPS = [
    ("FIFO walker (cellGcmSys.c)", ["gcm_rsx_process_fifo_unlocked", "cellGcm_rsx_process_fifo", "gcm_3d_method",
                                    "gcm_fifo_resync_why", "gcm_ref_publish_one", "gcm_flip_into_fifo",
                                    "gcm_ring_limit_raise", "gcm_wrap_into_lap", "gcm_fifo_bad_branch",
                                    "gcm_note_spu_put", "gcm_frame_trace"]),
    ("2D engines (cellGcmSys.c)", ["nv3089_blit", "gcm_2d_method", "nv309e_swz", "nv3089_engine_read",
                                   "nv3089_engine_wrote", "gcm_row_copy"]),
    ("method decoder and RSX state (rsx_commands.c)", ["rsx_process_method", "rsx_process_command_buffer",
                                                        "rsx_state_init", "rsx_process_vp_run", "rsx_set_backend",
                                                        "rsx_get_backend"]),
    ("register file (rsx_dispatch.c)", ["rsx_dispatch_method", "rsx_dispatch_init", "rsx_dsp_*"]),
    ("draw engine (rsx_draw_engine.c)", ["rsx_draw_engine_method", "rsx_draw_engine_init",
                                         "rsx_draw_engine_present", "rsx_draw_engine_guest_read",
                                         "rsx_draw_engine_set_bridge", "rsx_draw_engine_find_surface",
                                         "rsx_draw_engine_readback_surface", "eng_present", "sink_*"]),
    ("live draw (rsx_live_draw.c)", ["rsx_live_draw_*"]),
    ("Vulkan backend drawing (rsx_vulkan_backend.c)", ["vk_eng_draw*", "vk_eng_init", "vk_cb_draw_arrays",
                                                        "vk_cb_draw_indexed", "vk_cb_clear", "vk_guest_draw",
                                                        "vk_eng_translate", "vk_eng_flush_at", "vk_eng_present",
                                                        "vk_upload_texture", "vk_get_pipeline",
                                                        "rsx_vk_bridge_copy_image", "rsx_vk_bridge_copy_into",
                                                        "rsx_vk_bridge_signal"]),
    ("R4's bridge (ppfx_bridge.cpp)", ["*on_marker*", "*marker_reached*", "*bridge_install*", "*bridge_marker*",
                                       "*on_draw_pre*", "*handoffs*", "*handback*"]),
]
# kept on purpose (asset translation, the device and present): shown for contrast
KEPT = [
    ("shader translation", ["rsx_vp_decompile*", "rsx_fp_decompile*", "rsx_hlsl_to_spirv"]),
    ("texture and vertex formats, topology", ["rsx_texture_decode", "rsx_vertex_fetch_many",
                                              "rsx_draw_engine_topology_index_count"]),
    ("Vulkan device and present", ["rsx_vulkan_backend_init", "vk_present_window", "vk_create_swapchain",
                                   "rsx_vk_bridge_present_submit", "rsx_vk_bridge_device"]),
    ("native renderer and recorder", ["*asp_draw*", "*present_native*", "*r7_call*", "*transfer_op*"]),
]


class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.ULONG), ("TypeIndex", wt.ULONG), ("Reserved", ctypes.c_uint64 * 2),
                ("Index", wt.ULONG), ("Size", wt.ULONG), ("ModBase", ctypes.c_uint64), ("Flags", wt.ULONG),
                ("Value", ctypes.c_uint64), ("Address", ctypes.c_uint64), ("Register", wt.ULONG),
                ("Scope", wt.ULONG), ("Tag", wt.ULONG), ("NameLen", wt.ULONG), ("MaxNameLen", wt.ULONG),
                ("Name", ctypes.c_char * 1)]


def symbols(exe):
    dbg = ctypes.WinDLL("dbghelp")
    dbg.SymLoadModuleExW.restype = ctypes.c_uint64
    dbg.SymLoadModuleExW.argtypes = [ctypes.c_void_p, wt.HANDLE, wt.LPCWSTR, wt.LPCWSTR, ctypes.c_uint64,
                                     wt.DWORD, ctypes.c_void_p, wt.DWORD]
    CB = ctypes.WINFUNCTYPE(wt.BOOL, ctypes.POINTER(SYMBOL_INFO), wt.ULONG, ctypes.c_void_p)
    dbg.SymEnumSymbolsW = None
    dbg.SymEnumSymbols.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_char_p, CB, ctypes.c_void_p]
    proc = ctypes.c_void_p(0x52375359)
    dbg.SymSetOptions(0x2 | 0x10000)   # SYMOPT_UNDNAME | SYMOPT_DEBUG off
    dbg.SymInitializeW(proc, None, False)
    base = dbg.SymLoadModuleExW(proc, None, exe, None, 0x10000000, 0, None, 0)
    if not base:
        raise SystemExit(f"no symbols for {exe}")
    names = set()

    def cb(p, size, user):
        si = p.contents
        n = ctypes.string_at(ctypes.addressof(si) + SYMBOL_INFO.Name.offset, si.NameLen).decode(errors="replace")
        if si.Tag == 5:   # SymTagFunction
            names.add(n)
        return True
    dbg.SymEnumSymbols(proc, base, b"*", CB(cb), None)
    dbg.SymCleanup(proc)
    return names


def match(names, pats):
    out = set()
    for n in names:
        short = n.rsplit("::", 1)[-1]
        for p in pats:
            if fnmatch.fnmatchcase(short, p) or fnmatch.fnmatchcase(n, p):
                out.add(n)
                break
    return sorted(out)


def main():
    if len(sys.argv) > 3 and sys.argv[1] == "--list":
        names = symbols(sys.argv[3])
        for g, pats in GROUPS + KEPT:
            if g.startswith(sys.argv[2]):
                for n in match(names, pats):
                    print(n)
        return 0
    for exe in sys.argv[1:]:
        names = symbols(exe)
        print(f"{exe}: {len(names)} function symbols")
        total = 0
        for g, pats in GROUPS:
            m = match(names, pats)
            total += len(m)
            print(f"  command path  {g:<48} {len(m):5d}" + (f"   e.g. {', '.join(x.rsplit('::', 1)[-1] for x in m[:3])}" if m else ""))
        for g, pats in KEPT:
            m = match(names, pats)
            print(f"  kept          {g:<48} {len(m):5d}")
        print(f"  command-path functions in all: {total}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
