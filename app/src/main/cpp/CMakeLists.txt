cmake_minimum_required(VERSION 3.22.1)
project(ChaosEngine)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# 1. SFML 3.0
FetchContent_Declare(sfml
        GIT_REPOSITORY https://github.com/SFML/SFML.git
        GIT_TAG 3.0.1
)
set(SFML_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SFML_BUILD_DOC OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(sfml)

# 2. Box2D
FetchContent_Declare(box2d
        GIT_REPOSITORY https://github.com/erincatto/box2d.git
        GIT_TAG v2.4.1
)
set(BOX2D_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(BOX2D_BUILD_TESTBED OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(box2d)

# 3. ImGui CORE — v1.91.9 es compatible con imgui-sfml master
FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui
        GIT_TAG v1.91.9
)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
endif()

# 4. ImGui-SFML
FetchContent_Declare(imgui_sfml
        GIT_REPOSITORY https://github.com/SFML/imgui-sfml
        GIT_TAG master
)
FetchContent_GetProperties(imgui_sfml)
if(NOT imgui_sfml_POPULATED)
    FetchContent_Populate(imgui_sfml)

    # Patch para Android / OpenGL ES
    if(ANDROID)
        set(SFML_CPP "${imgui_sfml_SOURCE_DIR}/imgui-SFML.cpp")
        file(READ "${SFML_CPP}" _src)

        string(REPLACE
                "glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);"
                "/* glPolygonMode N/A en GLES */"
                _src "${_src}")

        string(REPLACE
                "glGetIntegerv(GL_POLYGON_MODE, last_polygon_mode);"
                "/* GL_POLYGON_MODE N/A en GLES */ last_polygon_mode[0] = 0; last_polygon_mode[1] = 0;"
                _src "${_src}")

        string(REPLACE
                "glPopAttrib();"
                "/* glPopAttrib N/A en GLES */"
                _src "${_src}")

        string(REPLACE
                "glPolygonMode(GL_FRONT, (GLenum)last_polygon_mode[0]);"
                "/* glPolygonMode N/A en GLES */"
                _src "${_src}")

        string(REPLACE
                "glPolygonMode(GL_BACK, (GLenum)last_polygon_mode[1]);"
                "/* glPolygonMode N/A en GLES */"
                _src "${_src}")

        file(WRITE "${SFML_CPP}" "${_src}")
        message(STATUS "[ChaosEngine] imgui-SFML.cpp parcheado para OpenGL ES")
    endif()
endif()

include_directories(
        src/Core src/Physics src/Graphics src/Utils src/Gen
        ${sfml_SOURCE_DIR}/include
        ${box2d_SOURCE_DIR}/include
        ${imgui_SOURCE_DIR}
        ${imgui_sfml_SOURCE_DIR}
)

file(GLOB_RECURSE SOURCES "src/*.cpp")
list(APPEND SOURCES
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_sfml_SOURCE_DIR}/imgui-SFML.cpp
)

if(ANDROID)
    add_library(ChaosEngine SHARED ${SOURCES})
else()
    add_executable(ChaosEngine ${SOURCES})
endif()

if(ANDROID)
    target_compile_definitions(ChaosEngine PRIVATE SFML_OPENGL_ES)
endif()

if(ANDROID)
    target_link_libraries(ChaosEngine
            sfml-graphics sfml-window sfml-system sfml-audio sfml-main
            box2d EGL GLESv1_CM GLESv3 log android
    )
    target_link_options(ChaosEngine PRIVATE -Wl,-u,ANativeActivity_onCreate)
else()
    target_link_libraries(ChaosEngine
            sfml-graphics sfml-window sfml-system sfml-audio sfml-main box2d
    )
endif()