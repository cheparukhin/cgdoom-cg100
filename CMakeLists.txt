# CGDoom for the fx-CG100 / Graph Math+ (MPM 1.1 + gint).
#
#   export PATH=$PWD/toolchain/bin:$PATH
#   fxsdk build-cg            -> CGDoom.g3a and CGDoomHD.g3a (and
#                                build-cg/cgdoom.bin, build-cg/cgdoom-hd.bin)
#
# Development loop with MPM >= 1.1: press SETTINGS in the MPM menu, then
#   fxlink -pw build-cg/cgdoom.bin
# (the header-less binary; MPM maps it at 0x00300000).
#
# Limits: the binary must stay below 2 MiB (gint's linker script, rom l=2M).
# Host/sim builds and tests use the Makefile, not this file.

cmake_minimum_required(VERSION 3.15)
project(CGDoom VERSION 1.0 LANGUAGES C ASM)

include(GenerateG3A)
find_package(Gint 2.11 REQUIRED)

# One add-in contains every supported OS table. Require the matching runtime
# installed by tools/build-toolchain.sh; a stock gint silently lacks OS 2.20.
file(STRINGS "${GINT_CONFIG_PATH}" CGDOOM_GINT_COMPAT
  REGEX "^#define GINT_CG100_OS_COMPAT 1$")
if(NOT CGDOOM_GINT_COMPAT)
  message(FATAL_ERROR "CGDoom needs its OS-compatible gint runtime. Run tools/build-toolchain.sh gint, then configure again.")
endif()

# Sources: picked up automatically (new engine files and SH4 assembly too)
file(GLOB ENGINE_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/engine/*.c")
file(GLOB ENGINE_ASM CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/engine/sh4/*.S"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/engine/sh4/*.s")
file(GLOB ENGINE_SH4_C CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/engine/sh4/*.c")
file(GLOB PLATFORM_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/gint/*.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/gint/*.S")

# One add-in per screen format (src/platform/cgd_config.h), from the same
# sources: CGDoom.g3a (320x200) and CGDoomHD.g3a (396x224 with the extras,
# CGD_HD). -DCGDOOM_WIDE_G3A=ON also builds CGDoomW.g3a (396x224,
# 8-bit), to compare the costs of the two HD parts on the calculator.
option(CGDOOM_WIDE_G3A "Also build CGDoomW.g3a (widescreen, 8-bit)" OFF)

# Warnings: warnings.mk's CGD_WARN, the set of every build, without -Werror,
# so a newer compiler's new warning can't stop this build; check-sh compiles
# the engine with the same compiler and the set as errors.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/warnings.mk")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/warnings.mk" CGDOOM_WARN)
string(REGEX REPLACE "\\\\\n" " " CGDOOM_WARN "${CGDOOM_WARN}")
string(REGEX MATCH "\nCGD_WARN[ \t]*:=([^\n]*)" CGDOOM_WARN "${CGDOOM_WARN}")
separate_arguments(CGDOOM_WARN UNIX_COMMAND "${CMAKE_MATCH_1}")
list(REMOVE_ITEM CGDOOM_WARN -Werror)
if(NOT "-Wall" IN_LIST CGDOOM_WARN)
  message(FATAL_ERROR "Cannot read CGD_WARN from warnings.mk")
endif()

function(cgdoom_addin target defines g3a name icon_uns icon_sel)
  add_executable(${target}
    ${ENGINE_SOURCES} ${ENGINE_SH4_C} ${ENGINE_ASM} ${PLATFORM_SOURCES})

  target_include_directories(${target} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/platform"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/engine"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gint")
  target_compile_definitions(${target} PRIVATE ${defines})

  # -O2 (gpSP measured -O3 slower on CG100 hardware: I-cache), RANGECHECK off
  # (not defined), no strict aliasing (DOOM type-puns), sections for --gc.
  target_compile_options(${target} PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-std=gnu11>
    -O2 -fno-strict-aliasing -ffunction-sections -fdata-sections
    ${CGDOOM_WARN})

  target_link_options(${target} PRIVATE
    -Wl,--gc-sections
    "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/${target}.map"
    # Link-time guard on .data+.bss vs. gint's pre-main allocations
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gint/uram_check.ld")
  set_target_properties(${target} PROPERTIES
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/gint/uram_check.ld")

  target_link_libraries(${target} Gint::Gint)

  generate_g3a(TARGET ${target} OUTPUT "${g3a}" NAME "${name}"
    ICONS ${icon_uns} ${icon_sel})
endfunction()

# SH4 assembly (src/engine/sh4): readable DSP mnemonics (ldrs/ldre/ldrc). The fxSDK toolchain
# file already passes -m4-nofpu -mb -Wa,--dsp; the raw-opcode fallback of sh4_asm.h stays
# byte-identical (make -C src/engine/sh4/test asmcheck). The hot drawers go to .ilram (gint's
# linker script copies it to the 4 KiB IL memory). The engine selects the *_SH4 drawers by itself
# (__sh__, src/engine/r_draw.h CGD_SH4); -DCGD_NO_SH4 would keep the C drawers.
set_source_files_properties(${ENGINE_ASM} PROPERTIES
  COMPILE_DEFINITIONS "CGD_SH4_DSP_MNEMONICS")

cgdoom_addin(cgdoom "" "CGDoom.g3a" "CGDoom"
  assets/icon-uns.png assets/icon-sel.png)
cgdoom_addin(cgdoom-hd "CGD_HD=1" "CGDoomHD.g3a" "CGDoom HD"
  assets/icon-hd-uns.png assets/icon-hd-sel.png)
if(CGDOOM_WIDE_G3A)
  cgdoom_addin(cgdoom-wide "CGD_WIDESCREEN=1" "CGDoomW.g3a" "CGDoom W"
    assets/icon-hd-uns.png assets/icon-hd-sel.png)
endif()
