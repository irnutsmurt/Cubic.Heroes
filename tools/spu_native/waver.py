"""tools/gen_spu_native.py's configuration for the game's waver task (runtime image 2, lifted by the setup as
build/gen/spu/spu_0001_at_00F48F80). Only where things are in the image -- its span, its entry, the function
the host calls -- not what they do: the code is generated from the player's own lift (port/native/spu_native.h).
"""

NAMESPACE = 'waver_gen'
TITLE = "The game's waver task (runtime image 2)"
PREFIX = 'spu_0001_at_00F48F80_spu_func_'
# The player's image, as the setup extracts it (build/gen/spu/images/spu_0001_at_00F48F80.elf).
IMAGE_SHA256 = '71dbf9afccd4e97dc961e8748bc091f1d2a2633d10ae71663e3b68dbd951ea90'

# The ELF's sections: .SpuGUID 0x3000, .init 0x3010, .text 0x3040-0x3958, .fini 0x3958-0x3988, .rodata
# 0x3990-0x39A0, .ctors/.dtors 0x3A00-0x3A10, .bss 0x3A10-0x8B60.
CODE_BEGIN = 0x3000
CODE_END = 0x3990
ENTRY = 0x3050

ADDRESS_TAKEN = {}
JUMP_TABLES = {}
BLOCK_NOTES = {}

# The function the host calls (port/native/waver_task.cpp): the task's main, r3 = the task's argument (words
# 0-1: the 64-bit address of its parameter block), r1 = 0x3FFD0 as the entry sets it; it returns 0.
MAIN = 0x36D8
FUNCS = {
    MAIN: "the task's main (r3: its argument)",
}
