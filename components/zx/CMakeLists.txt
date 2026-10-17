# ZX Spectrum 128K, +2, +3 (platform independent; host build in host/)
idf_component_register(SRCS "zx.c"
                            "ula.c"
                            "audio.c"
                            "tape.c"
                            "beta.c"
                            "snap.c"
                            "input.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES z80 upd765)

# Runs in the emulation loop. Jump tables for the switches (the IDF turns
# them off for code that may run with the flash cache disabled; the
# emulation task never does: flash writes stall it)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -fjump-tables -ftree-switch-conversion)
