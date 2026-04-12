set(COMPONENT_SRCDIRS "src")
set(COMPONENT_ADD_INCLUDEDIRS ".")
set(COMPONENT_REQUIRES "retro-go")
register_component()
rg_setup_compile_options(
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-array-bounds
    -Wno-implicit-fallthrough
    -DRIGHTSHIFT_IS_SAR
    -DFAST_LSB_WORD_ACCESS
    -O3
    -Ofast
    -fexpensive-optimizations
    -falign-functions=16
    -funroll-loops
)
