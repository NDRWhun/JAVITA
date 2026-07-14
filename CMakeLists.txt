#============================================================================
# JAMPVITA — Jedi Knight: Jedi Academy (Multiplayer, JAMP) for PlayStation Vita
#
# Single statically-linked ELF -> .self -> .vpk via the VitaSDK toolchain.
# Engine/renderer = OpenJK codemp/; game/cgame/ui are partially linked with
# localized symbols (no dlopen on the Vita, and their bg_* copies collide).
#============================================================================
cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Define the VITASDK environment variable (point at your VitaSDK root).")
  endif()
endif()

set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "")

project(JAMPVITA C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 14)

include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "JAMPVITA: Jedi Academy MP")
set(VITA_TITLEID  "JAMPV0001")
set(VITA_VERSION  "00.01")

if(NOT CMAKE_OBJCOPY)
  find_program(CMAKE_OBJCOPY arm-vita-eabi-objcopy)
endif()
if(NOT CMAKE_LINKER)
  find_program(CMAKE_LINKER arm-vita-eabi-ld)
endif()

#----------------------------------------------------------------------------
# Include domains: engine sees codemp/ + shared/; modules add their own dirs.
#----------------------------------------------------------------------------
include_directories(
  ${CMAKE_SOURCE_DIR}/src/compat   # newlib gap-fillers (e.g. <endian.h>); must be first
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/src/codemp
  ${CMAKE_SOURCE_DIR}/src/codemp/rd-common
  ${CMAKE_SOURCE_DIR}/src/codemp/rd-vanilla
  ${CMAKE_SOURCE_DIR}/src/shared
  ${CMAKE_SOURCE_DIR}/src/lib/gsl-lite/include
  ${CMAKE_SOURCE_DIR}/src/lib/minizip/include
  ${CMAKE_SOURCE_DIR}/src/lib/minizip/include/minizip
  ${CMAKE_BINARY_DIR}/generated
  $ENV{VITASDK}/arm-vita-eabi/include/SDL2
)

set(GIT_TAG "JAMPVITA-0.1")
set(SOURCE_DATE "${CMAKE_BUILD_TYPE}")
configure_file(
  ${CMAKE_SOURCE_DIR}/src/shared/qcommon/q_version.h.in
  ${CMAKE_BINARY_DIR}/generated/qcommon/q_version.h
  @ONLY)

#----------------------------------------------------------------------------
# Defines/flags shared by engine and modules.
#----------------------------------------------------------------------------
set(JAMP_COMMON_DEFINES
  VITA
  __linux__
  ARCH_STRING="arm"
  NDEBUG
  FINAL_BUILD
  __FLOAT_WORD_ORDER=1
  IOAPI_NO_64          # minizip: use fopen (newlib has no fopen64)
)
add_compile_definitions(${JAMP_COMMON_DEFINES})

set(JAMP_ARCH_FLAGS
  -g -O3
  -march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=hard
  -ffast-math -fsigned-char -fno-short-enums
  -fsingle-precision-constant -fno-optimize-sibling-calls
)
add_compile_options(${JAMP_ARCH_FLAGS})
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fpermissive>)
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-D__GNU_VISIBLE>)

include(${CMAKE_SOURCE_DIR}/cmake/jamp_sources.cmake)

# botlib TUs carry their upstream define
file(GLOB JAMP_BOTLIB_SOURCES ${CMAKE_SOURCE_DIR}/src/codemp/botlib/*.cpp)
set_source_files_properties(${JAMP_BOTLIB_SOURCES} PROPERTIES COMPILE_DEFINITIONS "BOTLIB")

# mp3code's global `window` collides with the renderer's `window_t window`
file(GLOB JAMP_MP3_SOURCES ${CMAKE_SOURCE_DIR}/src/codemp/mp3code/*.c)
set_source_files_properties(${JAMP_MP3_SOURCES} PROPERTIES COMPILE_DEFINITIONS "window=mp3code_window")

#----------------------------------------------------------------------------
# Modules: object lib -> ld -r -> objcopy (rename entry, localize the rest).
#----------------------------------------------------------------------------
function(add_isolated_module MOD SUFFIX MODDEFINE)
  add_library(${MOD}_objs OBJECT ${ARGN})
  target_compile_definitions(${MOD}_objs PRIVATE ${JAMP_COMMON_DEFINES} ${MODDEFINE})
  target_compile_options(${MOD}_objs PRIVATE ${JAMP_ARCH_FLAGS})
  target_include_directories(${MOD}_objs PRIVATE
    ${CMAKE_SOURCE_DIR}/src/compat
    ${CMAKE_SOURCE_DIR}/src/codemp
    ${CMAKE_SOURCE_DIR}/src/codemp/game
    ${CMAKE_SOURCE_DIR}/src/shared
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

#----------------------------------------------------------------------------
# Engine ELF (renderer statically in) + the three module objects.
#----------------------------------------------------------------------------
add_executable(${PROJECT_NAME}.elf
  ${JAMP_ENGINE_SOURCES}
  ${JAMP_PLATFORM_SOURCES}
  ${jampgame_MODULE_OBJECT}
  ${cgame_MODULE_OBJECT}
  ${ui_MODULE_OBJECT}
)

target_link_libraries(${PROJECT_NAME}.elf
  SDL2
  vitaGL
  vitashark
  SceShaccCgExt
  jpeg
  png16
  z
  mathneon
  stdc++
  taihen_stub
  SceShaccCg_stub
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
  FILE ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
)
