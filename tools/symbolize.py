#!/usr/bin/env python3
"""Name the host function at an address of a crash report (port/diag/diag.cpp
prints "module <exe> base <b> rva 0x<rva>"), from the build's PDB, with the
system's dbghelp.dll. Windows only.

  tools/symbolize.py <dotheroes.exe> 0x<rva> [0x<rva> ...]
"""
import ctypes
import ctypes.wintypes as wt
import os
import sys


class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.ULONG), ("TypeIndex", wt.ULONG), ("Reserved", ctypes.c_uint64 * 2),
                ("Index", wt.ULONG), ("Size", wt.ULONG), ("ModBase", ctypes.c_uint64), ("Flags", wt.ULONG),
                ("Value", ctypes.c_uint64), ("Address", ctypes.c_uint64), ("Register", wt.ULONG),
                ("Scope", wt.ULONG), ("Tag", wt.ULONG), ("NameLen", wt.ULONG), ("MaxNameLen", wt.ULONG),
                ("Name", ctypes.c_char * 1024)]


class IMAGEHLP_LINE64(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.DWORD), ("Key", ctypes.c_void_p), ("LineNumber", wt.DWORD),
                ("FileName", ctypes.c_char_p), ("Address", ctypes.c_uint64)]


def main():
    exe = os.path.abspath(sys.argv[1])
    dbg = ctypes.WinDLL("dbghelp")
    proc = wt.HANDLE(0x1234)
    dbg.SymSetOptions(0x00000002 | 0x00000010)   # SYMOPT_UNDNAME | SYMOPT_LOAD_LINES
    if not dbg.SymInitialize(proc, os.path.dirname(exe).encode(), False):
        sys.exit("SymInitialize failed")
    dbg.SymLoadModuleEx.restype = ctypes.c_uint64
    dbg.SymLoadModuleEx.argtypes = [wt.HANDLE, wt.HANDLE, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint64,
                                    wt.DWORD, ctypes.c_void_p, wt.DWORD]
    base = 0x140000000
    if not dbg.SymLoadModuleEx(proc, None, exe.encode(), None, base, 0, None, 0):
        sys.exit("SymLoadModuleEx failed")
    for a in sys.argv[2:]:
        addr = base + int(a, 16)
        si = SYMBOL_INFO()
        si.SizeOfStruct = 88   # sizeof(SYMBOL_INFO) in C, its Name[1] included
        si.MaxNameLen = 1024
        disp = ctypes.c_uint64(0)
        name = "?"
        if dbg.SymFromAddr(proc, ctypes.c_uint64(addr), ctypes.byref(disp), ctypes.byref(si)):
            name = "%s+0x%x" % (si.Name.decode(errors="replace"), disp.value)
        ln = IMAGEHLP_LINE64()
        ln.SizeOfStruct = ctypes.sizeof(IMAGEHLP_LINE64)
        d2 = wt.DWORD(0)
        where = ""
        if dbg.SymGetLineFromAddr64(proc, ctypes.c_uint64(addr), ctypes.byref(d2), ctypes.byref(ln)):
            where = " %s:%d" % (ln.FileName.decode(errors="replace"), ln.LineNumber)
        print("%s %s%s" % (a, name, where))
    dbg.SymCleanup(proc)


if __name__ == "__main__":
    main()
