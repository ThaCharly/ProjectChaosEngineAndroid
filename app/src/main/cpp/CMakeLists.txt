cmake_minimum_required(VERSION 3.22.1)
project(ChaosEngine)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# 1. SFML
FetchContent_Declare(sfml GIT_REPOSITORY https://github.com/SFML/SFML.git GIT_TAG 2.6.1)
set(SFML_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SFML_BUILD_DOC OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(sfml)

# 2. Box2D
FetchContent_Declare(box2d GIT_REPOSITORY https://github.com/erincatto/box2d.git GIT_TAG v2.4.1)
set(BOX2D_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(BOX2D_BUILD_TESTBED OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(box2d)

# 3. ImGui CORE (Only download source code)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui GIT_TAG v1.89.9)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
endif()

# 4. ImGui-SFML (Only download source code)
FetchContent_Declare(imgui_sfml GIT_REPOSITORY https://github.com/SFML/imgui-sfml GIT_TAG v2.6)
FetchContent_GetProperties(imgui_sfml)
if(NOT imgui_sfml_POPULATED)
    FetchContent_Populate(imgui_sfml)
endif()

# 5. Include Directories
include_directories(
        src/Core
        src/Physics
        src/Graphics
        src/Utils
        src/Gen
        ${sfml_SOURCE_DIR}/include
        ${box2d_SOURCE_DIR}/include
        ${imgui_SOURCE_DIR}
        ${imgui_sfml_SOURCE_DIR}
)

# 6. Source Files
file(GLOB_RECURSE SOURCES "src/*.cpp")

# Explicitly append ImGui files to your build
list(APPEND SOURCES
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_sfml_SOURCE_DIR}/imgui-SFML.cpp
)

# 7. Compilar
if(ANDROID)
    add_library(ChaosEngine SHARED ${SOURCES})

    # --- MAGIA NEGRA: Bypass del parser de CMake ---
    # Creamos un archivo físico en la carpeta temporal de build con las castraciones de OpenGL.
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/gles_patch.hpp "
#pragma once
#define GL_FILL 0x1B02
#define GL_POLYGON_MODE 0x0B40
#ifndef GL_FRONT_AND_BACK
#define GL_FRONT_AND_BACK 0x0408
#endif
#ifndef GL_FRONT
#define GL_FRONT 0x0404
#endif
#ifndef GL_BACK
#define GL_BACK 0x0405
#endif
// Anulamos las funciones de escritorio para que Android no explote
#define glPolygonMode(a, b)
#define glPushAttrib(a)
#define glPopAttrib()
#define glPushClientAttrib(a)
#define glPopClientAttrib()
    ")

    # Forzamos a Clang a tragarse este archivo al principio de cada .cpp
    target_compile_options(ChaosEngine PRIVATE
            "-include" "${CMAKE_CURRENT_BINARY_DIR}/gles_patch.hpp"
            -Wno-deprecated-declarations
    )

    target_compile_definitions(ChaosEngine PRIVATE SFML_OPENGL_ES)
else()
    add_executable(ChaosEngine ${SOURCES})
endif()

# 8. Linkeo
if(ANDROID)
    # En Android tenemos que linkear a mano OpenGL ES, EGL y las libs base del SO
    target_link_libraries(ChaosEngine
            sfml-graphics
            sfml-window
            sfml-system
            sfml-audio
            sfml-main
            box2d
            EGL
            GLESv1_CM
            GLESv2
            log
            android
    )

    # --- LA MAGIA ACÁ ---
    # Obligamos al linker a meter el punto de entrada de Android sí o sí,
    # sino lo optimiza y lo borra porque no lo ve llamado explícitamente en nuestro código.
    target_link_options(ChaosEngine PRIVATE -Wl,-u,ANativeActivity_onCreate)

else()
    # En PC zafamos porque SFML ya se encarga de arrastrar todo
    target_link_libraries(ChaosEngine
            sfml-graphics
            sfml-window
            sfml-system
            sfml-audio
            sfml-main
            box2d
    )
endif()