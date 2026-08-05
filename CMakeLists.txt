# Quake 2 (lp-public/Ports/Quake2) — CMake build for Aurora OS
#
# Stage 0 (разведка) завершён: движок использует OpenGL ES 2 через wrapper
# (OpenGLWrapper.c + EGLWrapper.c), SDL2 — платформенный слой, звук — SDL audio.
# После этапа 0 прежняя premake-сборка заменяется этим CMakeLists.txt.

cmake_minimum_required(VERSION 3.16)
project(Quake2Aurora C)

option(AURORA_FBO "Включить FBO-модуль для поворота/ресайза (этап 2)" OFF)
option(BUNDLE_SDL2 "Собирать встроенный SDL2 из libsdl (Aurora-фикс)" OFF)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Базовые флаги, близкие к оригинальному premake-рецепту.
add_compile_options(
    -ffast-math
    -Wall
    -Wno-unused-function
    -Wno-unused-parameter
    -Wno-missing-field-initializers
    -Wno-switch
    -fPIC
    -fvisibility=hidden
    -fcommon
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-g)
else()
    add_compile_options(-O2)
endif()

# Платформенные дефайны wrapper'а и движка.
add_compile_definitions(EGLW_GLES2 NOUNCRYPT ZIP __unix__)

# Дефайны для порта под Аврору.
add_compile_definitions(AURORA_OS)
set(AURORA_ORG "org.example" CACHE STRING "Organization name для песочницы Авроры")
set(AURORA_APP "quake2" CACHE STRING "Application name для песочницы Авроры")
add_compile_definitions(AURORA_ORG="${AURORA_ORG}")
add_compile_definitions(AURORA_APP="${AURORA_APP}")

if(AURORA_FBO)
    add_compile_definitions(AURORA_FBO)
endif()

# ARCH / OSTYPE используются движком для идентификации бинарника.
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    add_compile_definitions(ARCH=\"aarch64\")
elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "armv7l" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "armv7hl")
    add_compile_definitions(ARCH=\"arm\")
else()
    add_compile_definitions(ARCH=\"${CMAKE_SYSTEM_PROCESSOR}\")
endif()
add_compile_definitions(OSTYPE=\"Linux\")

# Пути include: исходники Quake2, совместимость Engine, zlib.
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/quake2
    ${CMAKE_CURRENT_SOURCE_DIR}/src/engine
    ${CMAKE_CURRENT_SOURCE_DIR}/src/zlib
)

# --- Зависимости ------------------------------------------------------------

find_package(PkgConfig REQUIRED)

if(BUNDLE_SDL2)
    # Встроенный SDL2 с фиксами под Аврору (savegame/SDL, ветка auroraos-sdl2).
    # Опции по рецепту AGENTS.MD: Wayland вместо X11, PulseAudio, shared.
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    set(SDL_AUDIO ON CACHE BOOL "" FORCE)
    set(SDL_PULSEAUDIO ON CACHE BOOL "" FORCE)
    set(SDL_RPATH OFF CACHE BOOL "" FORCE)
    set(SDL_WAYLAND ON CACHE BOOL "" FORCE)
    set(SDL_WAYLAND_LIBDECOR OFF CACHE BOOL "" FORCE)
    set(SDL_X11 OFF CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    add_subdirectory(libsdl)
    set(SDL2_TARGET SDL2)
    # EGL под Wayland: нативные типы wl_display/wl_egl_window вместо X11
    # (в таргете Авроры eglplatform.h иначе тянет X11/Xlib.h, которого нет).
    add_compile_definitions(WL_EGL_PLATFORM)
else()
    pkg_check_modules(SDL2 sdl2 REQUIRED IMPORTED_TARGET)
    set(SDL2_TARGET PkgConfig::SDL2)
endif()

pkg_check_modules(GLESV2 glesv2 REQUIRED)
pkg_check_modules(EGL egl REQUIRED)

# Собираем zlib статикой из исходников (оригинальный CMakeLists zlib
# в upstream-репозитории lp-public неполный, поэтому делаем минимальный target).
add_library(zlib_quake2 STATIC
    src/zlib/adler32.c
    src/zlib/compress.c
    src/zlib/crc32.c
    src/zlib/deflate.c
    src/zlib/gzclose.c
    src/zlib/gzlib.c
    src/zlib/gzread.c
    src/zlib/gzwrite.c
    src/zlib/inflate.c
    src/zlib/infback.c
    src/zlib/inffast.c
    src/zlib/inftrees.c
    src/zlib/trees.c
    src/zlib/uncompr.c
    src/zlib/zutil.c
)
# zlib хочет, чтобы zconf.h лежал рядом с zlib.h.
configure_file(src/zlib/zconf.h.included ${CMAKE_CURRENT_BINARY_DIR}/zlib_config/zconf.h COPYONLY)
target_include_directories(zlib_quake2 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/zlib
    ${CMAKE_CURRENT_BINARY_DIR}/zlib_config
)

# --- Исполняемый Quake2 ----------------------------------------------------

file(GLOB_RECURSE QUAKE2_SOURCES
    src/quake2/backends/unix/*.c
    src/quake2/backends/sdl/*.c
    src/quake2/backends/qal.c
    src/quake2/client/*.c
    src/quake2/client/menu/*.c
    src/quake2/client/refresh/*.c
    src/quake2/client/refresh/files/*.c
    src/quake2/client/refresh/constants/*.c
    src/quake2/client/sound/*.c
    src/quake2/common/*.c
    src/quake2/common/shared/*.c
    src/quake2/common/unzip/*.c
    src/quake2/server/*.c
    src/engine/OpenGLES/*.c
    src/engine/SDL/*.c
)

# Исключаем Windows-бэкенды, если glob их захватил через symlink/копию.
list(FILTER QUAKE2_SOURCES EXCLUDE REGEX "src/quake2/backends/windows/.*")

add_executable(quake2 ${QUAKE2_SOURCES})

target_include_directories(quake2 PRIVATE
    ${SDL2_INCLUDE_DIRS}
    ${GLESV2_INCLUDE_DIRS}
    ${EGL_INCLUDE_DIRS}
)

target_link_libraries(quake2
    ${SDL2_TARGET}
    ${GLESV2_LIBRARIES}
    ${EGL_LIBRARIES}
    zlib_quake2
    m
    dl
)

# --- Миссион-пакеты (game.so) ----------------------------------------------

function(add_quake2_mission name subdir)
    file(GLOB_RECURSE GAME_SOURCES
        src/quake2/common/shared/*.c
        src/quake2/${subdir}/*.c
    )
    add_library(${name} SHARED ${GAME_SOURCES})
    target_include_directories(${name} PRIVATE src/quake2 src/quake2/${subdir})
    set_target_properties(${name} PROPERTIES
        PREFIX ""
        OUTPUT_NAME "game"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${name}"
    )
endfunction()

add_quake2_mission(baseq2 game)
add_quake2_mission(ctf ctf/src)
add_quake2_mission(rogue rogue/src)
add_quake2_mission(xatrix xatrix/src)
