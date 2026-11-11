"""tools/gen_spu_native.py's configuration for the game's water task (runtime image 1, lifted by the setup as
build/gen/spu/spu_0000_at_00F47380). Only where things are in the image -- its span, its entry, the functions
the host calls -- not what they do: the code is generated from the player's own lift (port/native/spu_native.h).
"""

NAMESPACE = 'water_gen'
TITLE = "The game's water task (runtime image 1)"
PREFIX = 'spu_0000_at_00F47380_spu_func_'
# The player's image, as the setup extracts it (build/gen/spu/images/spu_0000_at_00F47380.elf): another
# executable's image is not generated for.
IMAGE_SHA256 = '8e3276b7e672f41ae59ad781561a21e66ebb2d85fdebef31454a20f098603a48'

# The ELF's sections: .SpuGUID 0x3000, .init 0x3010, .text 0x3040-0x4550, .fini 0x4550-0x4580, .rodata
# 0x4580-0x45D0, .ctors/.dtors 0x4600-0x4610, .bss 0x4610-0xC760.
CODE_BEGIN = 0x3000
CODE_END = 0x4580
ENTRY = 0x3050

ADDRESS_TAKEN = {}
JUMP_TABLES = {}
BLOCK_NOTES = {}

# The functions the host calls (port/native/water_task.cpp), with r3 = the local-store address of the step's
# 24-byte parameter block and r1 the stack: one simulation step is the first, then the second.
STEP_HEIGHTS = 0x3098
STEP_NORMALS = 0x35E0
FUNCS = {
    STEP_HEIGHTS: "one step's heights (r3: the step's parameters in local store)",
    STEP_NORMALS: "the step's normal map (r3: the same)",
}
