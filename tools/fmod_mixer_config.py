"""gen_spu_native.py's configuration for FMOD Ex's SPU mixer (runtime image 6, fmodex_spu.elf; lifted as
build/gen/spu/spu_0005_at_00F86A80/spu_recomp.c, 922 lifted functions).

What is here is what the lift alone does not say: the function pointers the mixer calls through (their
targets become functions of their own, reached from the generated dispatcher), its jump tables, the span of
its code, and names and notes. [C] read in the code (the image's disassembly), [H] inferred (names, roles).
"""

NAMESPACE = 'fmod_mixer'
PREFIX = 'spu_0005_at_00F86A80_spu_func_'
COVERAGE_MACRO = 'FMOD_MIXER_COVERAGE'

# The ELF's loadable bytes [C]: .SpuGUID 0x80, .init 0x90, .text 0xE0-0x103B0, .fini 0x103B0, .rodata
# 0x103E0-0x105C0, .ctors 0x10600, .dtors 0x10618, .data 0x10620-0x107F0, .bss 0x10800-0x36470.
CODE_BEGIN = 0x90          # .init
CODE_END = 0x103E0         # the end of .fini: what follows is data (the lift also took .rodata for code)
IMAGE_END = 0x10800        # the end of the image's loaded bytes (.bss after it)
ENTRY = 0xF0               # the ELF entry (crt0)

# Functions the mixer reaches only through a pointer [C]: an indirect call (bisl) or tail jump (bi $rN) to
# one of these is a direct call in the port. The .ctors (0x10604-0x10610, run backwards by .init's loop at
# 0xB0) and the internal callbacks the mixer stores into its objects (ila at the sites given).
ADDRESS_TAKEN = {
    0x9150: '.ctors[0] (run last by .init)',
    0xA040: '.ctors[1]',
    0xCC08: '.ctors[2]',
    0xD068: '.ctors[3] (run first)',
    0x0220: 'in a static descriptor in .rodata (0x103E8): r6 = this[108]; when set, this[0x20] = 0x608 and a call '
            'through r6 (bisl at 0x258); returns 0',
    0x0608: 'a callback stored into an object (ila at 0x248): this - 28 -> 0x3D0',
    0x0748: 'in static descriptors in .rodata (0x103F8, 0x10438, 0x10448): returns 0',
    0x9F58: 'a callback stored into an object (ila at 0xD458, 0xD6B8): this - 28 -> 0x94A8',
    0x9F68: 'a callback stored into an object (ila at 0x93D0, 0xD408, 0xD6A8)',
    0xA058: 'a callback stored into an object (ila at 0xD434)',
    0xA710: 'a callback stored into an object (ila at 0xD3E0)',
    0xAAF8: 'a callback stored into an object (ila at 0xD3B8)',
    0xBAA8: 'a callback kept on the stack (ila at 0x93A0) when a mode word is 8',
    0xDD58: 'a callback stored into an object (ila at 0xDCD8): r5 = 0 -> 0xBB58',
    0xDD60: 'a callback stored into an object (ila at 0xDCB0): r5 = 1 -> 0xC3C0',
}

# Jump tables [C]: the `bi $rN` just before each table, the table's address and its entry count (the
# clgti bound before it + 1). Their targets are blocks of the function that holds the jump: a switch.
JUMP_TABLES = {
    0x231C: 11,   # 0x2318: a size by a type code (11 cases: 0, 8, 16, 24, 32 bits)
    0x245C: 11,   # 0x2458
    0x873C: 11,   # 0x8738
    0xCD04: 7,    # 0xCD00: on object+0xD0 - 1
    0xD2FC: 8,    # 0xD2F8: the loader, on unit[0xD4] (0 = a DSP plugin, 0xD8E4)
}

# Names of functions [C: the code; the names H], printed with each generated function.
FUNCS = {
    0x0090: '.init: the .ctors run backwards from 0x10610 (bisl through each)',
    0x00E0: 'exit(status): .fini (0x103B0), then 0x10258',
    0x00F0: 'crt0, the ELF entry: the stack at 0x3FFD0, .init, main(r3-r6) (0xE190), then 0xEA98; stop 0',
    0xE190: 'main(ring EA, system object (r4/r5), the parameter block EA (r6)): GET the 0x80-byte parameter block '
            'to 0x36180, initialise, then the main loop (0xE39C)',
    0xE6C8: 'decrement-if-nonzero on a 128-byte line (LS r3 <- EA r4): GETLLAR, the word, PUTLLC until it holds; '
            'returns the word before',
    0xE788: 'load a program header (MP3 decoder) and its code by GET; returns its entry',
    0xE8C8: 'Mp3SpuDecode: load the MP3 decoder (flashMP3.pic) and jump to it',
    0xEA98: 'exit (from crt0)',
    0xEAC0: 'printf-like: the arguments through the mailboxes',
    0x10258: 'sys_spu_thread_exit(status): wait for the tags, WrOutMbox status, stop 0x102',
    0x102A0: 'sys_spu_thread_send_event(spup, data0, data1): WrOutMbox data1, WrOutIntrMbox spup<<24|data0, '
             'RdInMbox the result',
    0x102F8: 'sys_spu_thread_throw_event(spup, data0, data1)',
    0x10340: 'sys_spu_thread_receive_event(spuq, &d1, &d2, &d3): WrOutMbox spuq, stop 0x110, RdInMbox the result '
             'and the three data words',
    0x103B0: '.fini',
    0xDA30: 'the block (the ring path): one 256-sample block mixed and PUT into the ring [H]',
    0xDD70: 'the block (the other path) [H]',
    0xD278: 'the plugin loader: a jump table on unit[0xD4] (0 = a DSP: 0xD8E4)',
}

# Notes on blocks (comments before their labels).
BLOCK_NOTES = {
    0xE39C: 'the main loop: receive the block event (spuq 0x63, 0x10340), the two counters\' decrement-if-nonzero '
            '(0xE6C8 on 0x36200 / 0x36280), 0xFEEDBEEF ends it; the block (0xDA30); the reply (send_event spup 9)',
    0x1035C: 'stop 0x110: the receive-event service; the reply (status, data1-3) is read from the inbound mailbox',
}
