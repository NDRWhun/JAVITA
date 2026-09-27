#============================================================================
# Jedi Knight: Jedi Academy for PlayStation Vita
#
# One tree, two statically-linked ELFs -> .self -> .vpk via the VitaSDK toolchain.
#   JAVITA.vpk    single player, OpenJK code/
#   JAMPVITA.vpk  multiplayer,   OpenJK codemp/ (game/cgame/ui partially linked)
# Both render through the native sceGxm backend; there is no vitaGL.
#============================================================================
cmake_minimum_required(VERSION 3.16)

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

if(NOT CMAKE_OBJCOPY)
  find_program(CMAKE_OBJCOPY arm-vita-eabi-objcopy)
endif()
if(NOT CMAKE_LINKER)
  find_program(CMAKE_LINKER arm-vita-eabi-ld)
endif()

set(PORT_VERSION  "1.1.0")
set(VITA_VERSION  "01.10")	# SFO APP_VER is NN.NN and cannot carry the patch digit

set(SP_APP_NAME   "JAVITA: Jedi Academy")
set(SP_TITLEID    "JAVITA001")
set(MP_APP_NAME   "JAMPVITA: Jedi Academy MP")
set(MP_TITLEID    "JAMPV0001")

#----------------------------------------------------------------------------
# Defines and flags shared by both games. SP_GAME/_JK2EXE are single-player
# only and are set on that target, since shared/ code branches on them.
#----------------------------------------------------------------------------
set(JK_COMMON_DEFINES
  VITA
  __linux__
  ARCH_STRING="arm"
  NDEBUG
  FINAL_BUILD
  __FLOAT_WORD_ORDER=1
  IOAPI_NO_64          # minizip: use fopen (newlib has no fopen64)
)
add_compile_definitions(${JK_COMMON_DEFINES})

set(JK_ARCH_FLAGS
  -g -O3
  -march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=hard
  -ffast-math -fsigned-char -fno-short-enums
  -fsingle-precision-constant -fno-optimize-sibling-calls
  # unwind tables, so an ERR_DROP thrown from C reaches Com_Frame
  -funwind-tables
)
add_compile_options(${JK_ARCH_FLAGS})
# C++ needs permissive (legacy id-Tech-3 era code) + GNU visibility.
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fpermissive>)
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-D__GNU_VISIBLE>)

# the module isolation block below still reads these names
set(JAMP_COMMON_DEFINES ${JK_COMMON_DEFINES})
set(JAMP_ARCH_FLAGS ${JK_ARCH_FLAGS})

#----------------------------------------------------------------------------
# Include domains every target shares; the per-game trees are set per target.
#----------------------------------------------------------------------------
include_directories(
  ${CMAKE_SOURCE_DIR}/src/compat   # newlib gap-fillers (e.g. <endian.h>); must be first
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/src/shared
  ${CMAKE_SOURCE_DIR}/src/lib/gsl-lite/include
  ${CMAKE_SOURCE_DIR}/src/lib/minizip/include
  ${CMAKE_SOURCE_DIR}/src/lib/minizip/include/minizip
)

# Each game gets its own qcommon/q_version.h so the banners do not collide.
set(SOURCE_DATE "${CMAKE_BUILD_TYPE}")
set(GIT_TAG "JAVITA-${PORT_VERSION}")
configure_file(
  ${CMAKE_SOURCE_DIR}/src/shared/qcommon/q_version.h.in
  ${CMAKE_BINARY_DIR}/generated/sp/qcommon/q_version.h
  @ONLY)
set(GIT_TAG "JAMPVITA-${PORT_VERSION}")
configure_file(
  ${CMAKE_SOURCE_DIR}/src/shared/qcommon/q_version.h.in
  ${CMAKE_BINARY_DIR}/generated/mp/qcommon/q_version.h
  @ONLY)
configure_file(
  ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/template.xml.in
  ${CMAKE_BINARY_DIR}/generated/livearea/template.xml
  @ONLY)

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

# The Sce stubs both ELFs link, in one place.
set(JK_SCE_STUBS
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
  SceAppMgr_stub
  SceAppUtil_stub
  ScePower_stub
  SceNet_stub
  SceNetCtl_stub
)

include(${CMAKE_SOURCE_DIR}/cmake/ja_sources.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/jamp_sources.cmake)

# One native sceGxm backend for both games. Only the tess bridge stays per-renderer,
# because tess and glState are private to each rd-vanilla.
set(GXM_SOURCES
  ${CMAKE_SOURCE_DIR}/src/rd-gxm/gxm_device.cpp
  ${CMAKE_SOURCE_DIR}/src/rd-gxm/gxm_texture.cpp
  ${CMAKE_SOURCE_DIR}/src/rd-gxm/gxm_state.cpp
  ${CMAKE_SOURCE_DIR}/src/rd-gxm/gxm_backend.cpp
)

# warn when a .cg source no longer matches the committed gxm_shaders.h
set(GXM_SHADER_DIR ${CMAKE_SOURCE_DIR}/src/rd-gxm/shaders)
file(STRINGS ${GXM_SHADER_DIR}/gxm_shaders.h GXM_SHADER_STAMPS REGEX "^// src-sha256 ")
foreach(stamp IN LISTS GXM_SHADER_STAMPS)
  if(stamp MATCHES "^// src-sha256 ([^ ]+) ([0-9a-f]+)")
    set(cg_name ${CMAKE_MATCH_1})
    set(cg_want ${CMAKE_MATCH_2})
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${GXM_SHADER_DIR}/${cg_name})
    file(READ ${GXM_SHADER_DIR}/${cg_name} cg_text)
    string(REPLACE "\r" "" cg_text "${cg_text}")
    string(SHA256 cg_have "${cg_text}")
    if(NOT cg_have STREQUAL cg_want)
      message(WARNING "${cg_name} changed since gxm_shaders.h was generated: re-run src/rd-gxm/shaders/build_shaders.py --cgc <psp2cgc>")
    endif()
  endif()
endforeach()

#============================================================================
# Single player: JAVITA.elf
#============================================================================
add_executable(JAVITA.elf ${JA_SOURCES} ${GXM_SOURCES}
  ${CMAKE_SOURCE_DIR}/src/code/rd-vanilla/tr_gxm_bridge.cpp)
target_compile_definitions(JAVITA.elf PRIVATE USE_GXM_NATIVE SP_GAME _JK2EXE)
target_include_directories(JAVITA.elf PRIVATE
  ${CMAKE_SOURCE_DIR}/src/code/game
  ${CMAKE_SOURCE_DIR}/src/code/cgame
  ${CMAKE_SOURCE_DIR}/src/code/icarus
  ${CMAKE_SOURCE_DIR}/src/code
  ${CMAKE_SOURCE_DIR}/src/code/rd-common
  ${CMAKE_SOURCE_DIR}/src/code/rd-vanilla
  ${CMAKE_BINARY_DIR}/generated/sp
)
target_link_libraries(JAVITA.elf
  SDL2-static
  jpeg png16 z
  mathneon
  stdc++
  ${JK_SCE_STUBS}
  c m
)

# mp3code's global `window` (float decode table) collides with rd-vanilla's
# `window_t window`. Rename it consistently across the (self-contained) mp3
# decoder only, via a per-file macro -- no source edits, no effect elsewhere.
file(GLOB SP_MP3_SOURCES ${CMAKE_SOURCE_DIR}/src/code/mp3code/*.c)
set_source_files_properties(${SP_MP3_SOURCES} PROPERTIES COMPILE_DEFINITIONS "window=mp3code_window")

#============================================================================
# Multiplayer: JAMPVITA.elf
#
# game/cgame/ui cannot be dlopened on the Vita and their bg_* copies collide,
# so each is partially linked and its symbols localized behind one entry point.
#============================================================================
file(GLOB JAMP_BOTLIB_SOURCES ${CMAKE_SOURCE_DIR}/src/codemp/botlib/*.cpp)
set_source_files_properties(${JAMP_BOTLIB_SOURCES} PROPERTIES COMPILE_DEFINITIONS "BOTLIB")

file(GLOB MP_MP3_SOURCES ${CMAKE_SOURCE_DIR}/src/codemp/mp3code/*.c)
set_source_files_properties(${MP_MP3_SOURCES} PROPERTIES COMPILE_DEFINITIONS "window=mp3code_window")

function(add_isolated_module MOD SUFFIX MODDEFINE)
  add_library(${MOD}_objs OBJECT ${ARGN})
  target_compile_definitions(${MOD}_objs PRIVATE ${JAMP_COMMON_DEFINES} ${MODDEFINE})
  target_compile_options(${MOD}_objs PRIVATE ${JAMP_ARCH_FLAGS})
  target_include_directories(${MOD}_objs PRIVATE
    ${CMAKE_SOURCE_DIR}/src/compat
    ${CMAKE_SOURCE_DIR}/src/codemp
    ${CMAKE_SOURCE_DIR}/src/codemp/game
    ${CMAKE_SOURCE_DIR}/src/shared
    ${CMAKE_BINARY_DIR}/generated/mp
  )
  set(rsp ${CMAKE_BINARY_DIR}/${MOD}_objs.rsp)
  file(GENERATE OUTPUT ${rsp} CONTENT "$<JOIN:$<TARGET_OBJECTS:${MOD}_objs>,\n>")
  set(out ${CMAKE_BINARY_DIR}/${MOD}_module.o)
  add_custom_command(OUTPUT ${out}
    COMMAND ${CMAKE_LINKER} -r -o ${CMAKE_BINARY_DIR}/${MOD}_pre.o @${rsp}
    COMMAND ${CMAKE_OBJCOPY} --redefine-sym GetModuleAPI=GetModuleAPI_${SUFFIX} ${CMAKE_BINARY_DIR}/${MOD}_pre.o
    COMMAND ${CMAKE_OBJCOPY} -G GetModuleAPI_${SUFFIX} ${CMAKE_BINARY_DIR}/${MOD}_pre.o ${out}
    DEPENDS ${MOD}_objs ${rsp} $<TARGET_OBJECTS:${MOD}_objs>
    COMMENT "Isolating module ${MOD} (entry GetModuleAPI_${SUFFIX})"
    COMMAND_EXPAND_LISTS)
  set_source_files_properties(${out} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
  set(${MOD}_MODULE_OBJECT ${out} PARENT_SCOPE)
endfunction()

add_isolated_module(jampgame game  _GAME    ${JAMP_GAME_SOURCES})
add_isolated_module(cgame    cgame _CGAME   ${JAMP_CGAME_SOURCES})
add_isolated_module(ui       ui    UI_BUILD ${JAMP_UI_SOURCES})

add_executable(JAMPVITA.elf
  ${JAMP_ENGINE_SOURCES}
  ${JAMP_PLATFORM_SOURCES}
  ${GXM_SOURCES}
  ${CMAKE_SOURCE_DIR}/src/codemp/rd-vanilla/tr_gxm_bridge.cpp
  ${jampgame_MODULE_OBJECT}
  ${cgame_MODULE_OBJECT}
  ${ui_MODULE_OBJECT}
)
target_compile_definitions(JAMPVITA.elf PRIVATE USE_GXM_NATIVE)
target_include_directories(JAMPVITA.elf PRIVATE
  ${CMAKE_SOURCE_DIR}/src/codemp
  ${CMAKE_SOURCE_DIR}/src/codemp/rd-common
  ${CMAKE_SOURCE_DIR}/src/codemp/rd-vanilla
  ${CMAKE_BINARY_DIR}/generated/mp
)
target_link_libraries(JAMPVITA.elf
  SDL2-static
  jpeg png16 z
  mathneon
  stdc++
  ${JK_SCE_STUBS}
  c m
)

#============================================================================
# Package: ELF -> SELF -> VPK, once per game.
#
# vita-elf-create walks the whole input and corrupts its heap near these ELF
# sizes; the stripped copy keeps the basename so the module name is unchanged.
#============================================================================
set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")

foreach(GAME JAVITA JAMPVITA)
  set(PKG_ELF ${CMAKE_BINARY_DIR}/package/${GAME}.elf)
  add_custom_command(OUTPUT ${PKG_ELF}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/package
    COMMAND ${CMAKE_OBJCOPY} --strip-debug $<TARGET_FILE:${GAME}.elf> ${PKG_ELF}
    DEPENDS ${GAME}.elf
    COMMENT "Stripping debug info for packaging ${GAME}"
    VERBATIM)
  add_custom_target(${GAME}-package-elf ALL DEPENDS ${PKG_ELF})
  vita_create_self(${GAME}.self ${PKG_ELF} UNSAFE)
  add_dependencies(${GAME}.elf-velf ${GAME}-package-elf)
endforeach()

vita_create_vpk(JAVITA.vpk ${SP_TITLEID} JAVITA.self
  VERSION ${VITA_VERSION}
  NAME ${SP_APP_NAME}
  FILE ${CMAKE_SOURCE_DIR}/sce_sys/icon0.png sce_sys/icon0.png
  FILE ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE ${CMAKE_BINARY_DIR}/generated/livearea/template.xml sce_sys/livearea/contents/template.xml
)

# vita_create_vpk appends its -s flags to VITA_MKSFOEX_FLAGS, so each vpk starts from the base set
set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")
# shares the single-player LiveArea art for now; only the SFO name differs
vita_create_vpk(JAMPVITA.vpk ${MP_TITLEID} JAMPVITA.self
  VERSION ${VITA_VERSION}
  NAME ${MP_APP_NAME}
  FILE ${CMAKE_SOURCE_DIR}/sce_sys/icon0.png sce_sys/icon0.png
  FILE ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE ${CMAKE_BINARY_DIR}/generated/livearea/template.xml sce_sys/livearea/contents/template.xml
)
