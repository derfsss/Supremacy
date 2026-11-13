cmake_minimum_required(VERSION 3.20)
project(Supremacy C)

# Builds (README.md has step-by-step instructions):
#   Windows (MSYS2 UCRT64 terminal, GCC and SDL 3 from MSYS2) and Linux
#   (GCC or Clang, SDL 3.2 or later):
#     cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
#     cmake --build build
#   AmigaOS 4 (Docker image walkero/amigagccondocker:os4-gcc15: GCC 15,
#   newlib, SDL 3 from the SDK): bash tools/build_os4.sh

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

set(CORE_SOURCES
  src/core/game.c
  src/core/economy.c
  src/core/setup.c
  src/core/craft.c
  src/core/combat.c
  src/core/enemy.c
  src/core/events.c
  src/core/day.c
)

# The game core as a static library (for the game) ...
add_library(supcore STATIC ${CORE_SOURCES})
target_include_directories(supcore PUBLIC src/core)

# ... and, on Windows, as a DLL for the Python checks against the original
# (test scripts load it with ctypes).
if(WIN32)
  add_library(supcore_test SHARED ${CORE_SOURCES})
  target_include_directories(supcore_test PUBLIC src/core)
  target_compile_definitions(supcore_test PRIVATE SUP_BUILD_DLL)
  set_target_properties(supcore_test PROPERTIES PREFIX "" OUTPUT_NAME "supcore")
  if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    # self-contained DLL: no libgcc_s dependency when Python loads it
    target_link_options(supcore_test PRIVATE -static-libgcc -static)
  endif()
endif()

# ---- sound: the original's sequencer + a Paula mixer ----
add_library(supsound STATIC src/sound/sequencer.c src/sound/paula.c src/sound/endmusic.c)
target_include_directories(supsound PUBLIC src/sound)

# ---- assets: loading the original game files at run time ----
add_library(supassets STATIC src/assets/gamedata.c)
target_include_directories(supassets PUBLIC src/assets)

# ---- ui: the original's screen buffer and text routines ----
file(GLOB UI_SOURCES CONFIGURE_DEPENDS src/ui/*.c)
add_library(supui STATIC ${UI_SOURCES})
target_include_directories(supui PUBLIC src/ui)
target_link_libraries(supui PUBLIC supcore supassets)

# ---- the game (SDL3 front end) ----
# The version banner names the commit built (cmake/build_id.cmake, run at
# every build: build_id.h only changes when the commit or -dirty does).
set(SUP_VERSION "1.0")
add_custom_target(build_id
  COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_BINARY_DIR}/gen/build_id.h
          -DVERSION=${SUP_VERSION}
          -P ${CMAKE_SOURCE_DIR}/cmake/build_id.cmake
  BYPRODUCTS ${CMAKE_BINARY_DIR}/gen/build_id.h)
set(GAME_SOURCES src/platform/main.c src/platform/launcher.c src/platform/extract.c
                 src/platform/os4_dialog.c)
if(AMIGAOS4)
  # The OS4 SDK has SDL3 (newlib) as a static library without a CMake config.
  add_executable(supremacy ${GAME_SOURCES})
  target_link_libraries(supremacy PRIVATE supui supassets supsound supcore SDL3 pthread m)
  # the Workbench icon next to the program
  add_custom_command(TARGET supremacy POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/assets/icons/supremacy.info
            $<TARGET_FILE_DIR:supremacy>/supremacy.info)
else()
  find_package(SDL3 CONFIG QUIET)
  if(SDL3_FOUND)
    if(WIN32)
      enable_language(RC)
      list(APPEND GAME_SOURCES src/platform/supremacy.rc)   # the icon
    endif()
    add_executable(supremacy ${GAME_SOURCES})
    target_link_libraries(supremacy PRIVATE supui supassets supsound supcore SDL3::SDL3)
  else()
    message(STATUS "SDL3 not found: the game executable is not built (core library only)")
  endif()
endif()
if(TARGET supremacy)
  add_dependencies(supremacy build_id)
  target_include_directories(supremacy PRIVATE ${CMAKE_BINARY_DIR}/gen)
endif()
