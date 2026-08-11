#============================================================================
# JAVITA — Jedi Knight: Jedi Academy (Single Player) for PlayStation Vita
#
# Single statically-linked ELF -> .self -> .vpk via the VitaSDK toolchain.
# Engine/renderer + game/cgame/icarus = OpenJK code/ (Jedi Academy single-player).
#============================================================================
cmake_minimum_required(VERSION 3.10)

# --- VitaSDK toolchain ---
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Define the VITASDK environment variable (point at your VitaSDK root).")
  endif()
endif()

# Allow older bundled CMake snippets to configure under CMake 4.x.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "")

project(JAVITA C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 14)

include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "JAVITA: Jedi Academy")
set(VITA_TITLEID  "JAVITA001")
set(PORT_VERSION  "0.9")
set(VITA_VERSION  "00.09")	# SFO APP_VER is NN.NN and cannot carry the patch digit

#----------------------------------------------------------------------------
# Include domains: native OpenJK Jedi Academy single-player (code/).
#----------------------------------------------------------------------------
include_directories(
  ${CMAKE_SOURCE_DIR}/src/compat   # newlib gap-fillers (e.g. <endian.h>); must be first
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/src/code/game
  ${CMAKE_SOURCE_DIR}/src/code/cgame
  ${CMAKE_SOURCE_DIR}/src/code/icarus
  ${CMAKE_SOURCE_DIR}/src/code
  ${CMAKE_SOURCE_DIR}/src/code/rd-common
  ${CMAKE_SOURCE_DIR}/src/code/rd-vanilla
  ${CMAKE_SOURCE_DIR}/src/shared
  ${CMAKE_SOURCE_DIR}/src/lib/gsl-lite/include
  ${CMAKE_SOURCE_DIR}/src/lib/minizip/include
  ${CMAKE_SOURCE_DIR}/src/lib/minizip/include/minizip
  ${CMAKE_BINARY_DIR}/generated
)

# Generate qcommon/q_version.h (normally produced by OpenJK's build).
set(GIT_TAG "JAVITA-${PORT_VERSION}")
set(SOURCE_DATE "${CMAKE_BUILD_TYPE}")
configure_file(
  ${CMAKE_SOURCE_DIR}/src/shared/qcommon/q_version.h.in
  ${CMAKE_BINARY_DIR}/generated/qcommon/q_version.h
  @ONLY)
configure_file(
  ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/template.xml.in
  ${CMAKE_BINARY_DIR}/generated/livearea/template.xml
  @ONLY)

#----------------------------------------------------------------------------
# Defines.
#  SP_GAME / _JK2EXE : OpenJK single-player build (engine + game unified)
#  VITA / __linux__  : Vita platform + take the unix code paths
#----------------------------------------------------------------------------
add_compile_definitions(
  SP_GAME
  _JK2EXE
  VITA
  __linux__
  ARCH_STRING="arm"
  NDEBUG
  FINAL_BUILD
  __FLOAT_WORD_ORDER=1
  IOAPI_NO_64          # minizip: use fopen (newlib has no fopen64)
)

#----------------------------------------------------------------------------
# Flags (StaticJK-proven ARM/NEON hard-float set).
#----------------------------------------------------------------------------
set(JK2VITA_ARCH_FLAGS
  -g -O3
  -march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=hard
  -ffast-math -fsigned-char -fno-short-enums
  -fsingle-precision-constant -fno-optimize-sibling-calls
)
add_compile_options(${JK2VITA_ARCH_FLAGS})
# C++ needs permissive (legacy id-Tech-3 era code) + GNU visibility.
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fpermissive>)
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-D__GNU_VISIBLE>)

#----------------------------------------------------------------------------
# Source set (see cmake/ja_sources.cmake).
#----------------------------------------------------------------------------

# Build SDL2 from the submodule with the vitaGL video driver off: the SDK's prebuilt
# SDL2 pulls SDL_vitagles_vgl.c, which references vgl* symbols we no longer link
set(VIDEO_VITA_VGL OFF CACHE BOOL "" FORCE)
set(VIDEO_VITA_PVR OFF CACHE BOOL "" FORCE)
set(VIDEO_VITA_PIB OFF CACHE BOOL "" FORCE)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(SDL2_DISABLE_SDL2MAIN ON CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/SDL-vitagl EXCLUDE_FROM_ALL)

include(${CMAKE_SOURCE_DIR}/cmake/ja_sources.cmake)

# Native GXM renderer backend, plus the bridge that rd-vanilla issues its draws through.
set(GXM_SOURCES
  ${CMAKE_SOURCE_DIR}/src/code/rd-gxm/gxm_device.cpp
  ${CMAKE_SOURCE_DIR}/src/code/rd-gxm/gxm_texture.cpp
  ${CMAKE_SOURCE_DIR}/src/code/rd-gxm/gxm_state.cpp
  ${CMAKE_SOURCE_DIR}/src/code/rd-gxm/gxm_backend.cpp
  ${CMAKE_SOURCE_DIR}/src/code/rd-vanilla/tr_gxm_bridge.cpp
)

add_executable(${PROJECT_NAME}.elf ${JA_SOURCES} ${GXM_SOURCES})
target_compile_definitions(${PROJECT_NAME}.elf PRIVATE USE_GXM_NATIVE)

# mp3code's global `window` (float decode table) collides with rd-vanilla's
# `window_t window`. Rename it consistently across the (self-contained) mp3
# decoder only, via a per-file macro -- no source edits, no effect elsewhere.
file(GLOB JK2_MP3_SOURCES ${CMAKE_SOURCE_DIR}/src/code/mp3code/*.c)
set_source_files_properties(${JK2_MP3_SOURCES} PROPERTIES COMPILE_DEFINITIONS "window=mp3code_window")

target_link_libraries(${PROJECT_NAME}.elf
  SDL2-static
  # image / compression
  jpeg
  png16
  z
  # math
  mathneon
  # C++ runtime
  stdc++
  # Sce stubs
  SceGxm_stub
  SceDisplay_stub
  SceCtrl_stub
  SceTouch_stub
  SceMotion_stub
  SceHid_stub
  SceAudio_stub
  SceAudioIn_stub
  SceCommonDialog_stub
  SceIme_stub
  SceSysmodule_stub
  SceKernelDmacMgr_stub
  ScePower_stub
  SceNet_stub
  SceNetCtl_stub
  c
  m
)

#----------------------------------------------------------------------------
# Package: ELF -> SELF -> VPK
#----------------------------------------------------------------------------
vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME}.elf UNSAFE)

set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE ${CMAKE_SOURCE_DIR}/sce_sys/icon0.png sce_sys/icon0.png
  FILE ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE ${CMAKE_BINARY_DIR}/generated/livearea/template.xml sce_sys/livearea/contents/template.xml
)
