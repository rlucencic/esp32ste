# Atari STE machine (platform independent; host build in host/)
idf_component_register(SRCS "st.c"
                            "mem.c"
                            "io.c"
                            "mfp.c"
                            "video.c"
                            "fdc.c"
                            "floppy.c"
                            "psg.c"
                            "acia.c"
                            "tos_patch.c"
                            "gemdos.c"
                            "audio.c"
                            "blitter.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES musashi
                       PRIV_REQUIRES diskspace)

# Runs together with the CPU core in the emulation loop
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
