# CGDoom for the fx-CG100 / Graph Math+ (MPM 1.1 + gint).
#
#   export PATH=$PWD/toolchain/bin:$PATH
#   fxsdk build-cg            -> CGDoom.g3a (and build-cg/cgdoom.bin)
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
  REGEX "^#define GINT_CGDOOM_OS_COMPAT 1$")
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

add_executable(cgdoom
  ${ENGINE_SOURCES} ${ENGINE_SH4_C} ${ENGINE_ASM} ${PLATFORM_SOURCES})

target_include_directories(cgdoom PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src/platform"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/engine"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/gint")

# -O2 (gpSP measured -O3 slower on CG100 hardware: I-cache), RANGECHECK off
# (not defined), no strict aliasing (DOOM type-puns), sections for --gc.
target_compile_options(cgdoom PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-std=gnu11>
  -O2 -fno-strict-aliasing -ffunction-sections -fdata-sections
  -Wall -Wextra)

# DOOM/CGDoom code triggers these a lot (same set as the host Makefile)
set_source_files_properties(${ENGINE_SOURCES} ${ENGINE_SH4_C} PROPERTIES
  COMPILE_OPTIONS "-Wno-sign-compare;-Wno-unused-parameter;-Wno-unused-but-set-variable;-Wno-unused-but-set-parameter;-Wno-missing-field-initializers;-Wno-deprecated-declarations;-Wno-implicit-fallthrough")

# SH4 assembly (src/engine/sh4): readable DSP mnemonics (ldrs/ldre/ldrc). The fxSDK toolchain
# file already passes -m4-nofpu -mb -Wa,--dsp; the raw-opcode fallback of sh4_asm.h stays
# byte-identical (make -C src/engine/sh4/test asmcheck). The hot drawers go to .ilram (gint's
# linker script copies it to the 4 KiB IL memory). The engine selects the *_SH4 drawers by itself
# (__sh__, src/engine/r_draw.h CGD_SH4); -DCGD_NO_SH4 would keep the C drawers.
set_source_files_properties(${ENGINE_ASM} PROPERTIES
  COMPILE_DEFINITIONS "CGD_SH4_DSP_MNEMONICS")

target_link_options(cgdoom PRIVATE
  -Wl,--gc-sections
  "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/cgdoom.map"
  # Link-time guard on .data+.bss vs. gint's pre-main allocations
  "${CMAKE_CURRENT_SOURCE_DIR}/src/gint/uram_check.ld")
set_target_properties(cgdoom PROPERTIES
  LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/gint/uram_check.ld")

target_link_libraries(cgdoom Gint::Gint)

generate_g3a(TARGET cgdoom OUTPUT "CGDoom.g3a" NAME "CGDoom"
  ICONS assets/icon-uns.png assets/icon-sel.png)
