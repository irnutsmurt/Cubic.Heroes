# glslang's SPIRV/doc.cpp (tables of names for its disassembler) exhausts
# memory in clang 22 (Zig 0.17) at -O2 and above: compile it unoptimised.
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL
    set_source_files_properties "${CMAKE_SOURCE_DIR}/SPIRV/doc.cpp"
    DIRECTORY "${CMAKE_SOURCE_DIR}/glslang" PROPERTIES COMPILE_OPTIONS "-O0")
