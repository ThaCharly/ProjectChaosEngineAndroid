cmake_minimum_required(VERSION 3.22.1)
project(ChaosEngine)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# 1. SFML directo desde la fuente (Chau find_package)
FetchContent_Declare(
        sfml
        GIT_REPOSITORY https://github.com/SFML/SFML.git
        GIT_TAG        2.6.1
)
# Apagamos cosas de SFML que no nos importan para que compile más rápido
set(SFML_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SFML_BUILD_DOC OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(sfml)

# 2. Box2D directo desde la fuente
FetchContent_Declare(
        box2d
        GIT_REPOSITORY https://github.com/erincatto/box2d.git
        GIT_TAG        v2.4.1
)
set(BOX2D_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(BOX2D_BUILD_TESTBED OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(box2d)

# 3. ImGui (Igual que lo tenías)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui GIT_TAG v1.89.9)
FetchContent_MakeAvailable(imgui)
set(IMGUI_DIR ${imgui_SOURCE_DIR})
set(IMGUI_SFML_FIND_SFML OFF)
FetchContent_Declare(imgui-sfml GIT_REPOSITORY https://github.com/SFML/imgui-sfml GIT_TAG v2.6)
FetchContent_MakeAvailable(imgui-sfml)

# 4. Includes dinámicos
include_directories(
        src/Core
        src/Physics
        src/Graphics
        src/Utils
        src/Gen
        ${sfml_SOURCE_DIR}/include
        ${box2d_SOURCE_DIR}/include
)

# 5. Código Fuente
file(GLOB_RECURSE SOURCES "src/*.cpp")

if(ANDROID)
    add_library(ChaosEngine SHARED ${SOURCES})
else()
    add_executable(ChaosEngine ${SOURCES})
endif()

# 6. Linkeo (OJO ACÁ: Agregamos sfml-main)
target_link_libraries(ChaosEngine
        sfml-graphics
        sfml-window
        sfml-system
        sfml-audio
        sfml-main   # <--- VITAL EN ANDROID. Intercepta el NativeActivity y llama a tu main()
        box2d
        ImGui-SFML::ImGui-SFML
)