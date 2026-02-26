set(COMPONENT_SRCDIRS ".")
set(COMPONENT_ADD_INCLUDEDIRS ".")
set(COMPONENT_REQUIRES "retro-go")
register_component()
rg_setup_compile_options(
    -Wno-error=stringop-truncation
    -Wno-error=format-overflow
    -Wno-implicit-fallthrough
    -Wno-format-overflow
    -Wno-stringop-truncation
    -Wno-unused-but-set-variable
    -Wno-unused-variable
    -Wno-unused-label
    -Wno-maybe-uninitialized
    -O3
    -DCZ80=1
)
file(GLOB_RECURSE c_sources "*.c" "*.cpp")
set_source_files_properties(${c_sources} PROPERTIES COMPILE_FLAGS "")
