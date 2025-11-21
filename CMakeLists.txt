cmake_minimum_required(VERSION 3.15)
project(SplenderProof LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(SPLENDER_IMGUI_DEMO "Compile imgui_demo.cpp into imgui static lib" ON)

find_package(glfw3 CONFIG REQUIRED)
find_package(glad CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(OpenGL REQUIRED)

find_package(assimp CONFIG)
# If the CONFIG mode fails for some setups, also try the module mode fallback
if(NOT assimp_FOUND)
    find_package(assimp)
endif()

# Ensure don't pull in GLFW's legacy headers via include order in sources
add_compile_definitions(GLFW_INCLUDE_NONE)

# ImGui location (expected in repository at third_party/imgui)..
set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/third_party/imgui")
if (NOT EXISTS "${IMGUI_DIR}")
    message(FATAL_ERROR "ImGui not found at ${IMGUI_DIR}. Place ImGui under third_party/imgui")
endif()

set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
)
if (SPLENDER_IMGUI_DEMO)
    list(APPEND IMGUI_SOURCES ${IMGUI_DIR}/imgui_demo.cpp)
endif()

set(IMGUI_BACKENDS
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

add_library(imgui STATIC
    ${IMGUI_SOURCES}
    ${IMGUI_BACKENDS}
)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_definitions(imgui PRIVATE IMGUI_IMPL_OPENGL_LOADER_GLAD)
target_link_libraries(imgui PUBLIC glfw)

# Core library
# -------------------------

set(PROJECT_CORE_SOURCES
    src/loader.cpp
    src/renderer.cpp
    src/ui.cpp
    src/globals.cpp
    src/app.cpp
    src/usersettings.cpp
)

add_library(splender_core STATIC ${PROJECT_CORE_SOURCES})

target_include_directories(splender_core
    PUBLIC
        ${CMAKE_SOURCE_DIR}/src
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends
)

target_link_libraries(splender_core
    PUBLIC
        glad::glad
        glm::glm
)

if(assimp_FOUND)
    message(STATUS "Assimp found: enabling Assimp support")
    target_compile_definitions(splender_core PUBLIC USE_ASSIMP)
    target_link_libraries(splender_core PUBLIC assimp::assimp)
else()
    message(STATUS "Assimp not found: building without Assimp support (OBJ-only loader will be used)")
endif()

# -------------------------
# Application / executable
# -------------------------
set(SPLENDER_MAIN_SRC src/main.cpp)
if (NOT EXISTS "${CMAKE_SOURCE_DIR}/${SPLENDER_MAIN_SRC}")
    message(FATAL_ERROR "Main source not found: ${SPLENDER_MAIN_SRC}")
endif()

set(APP_ICON "${CMAKE_SOURCE_DIR}/assets/icons/splender_logo.ico")
if (NOT EXISTS "${APP_ICON}")
    message(FATAL_ERROR "Icon not found: ${APP_ICON}")
endif()

# Ensure output dir exists for generated RC
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/resources")

configure_file(
    "${CMAKE_SOURCE_DIR}/resources/app.rc.in"
    "${CMAKE_BINARY_DIR}/resources/app.rc"
    @ONLY
)

set(GENERATED_APP_RC "${CMAKE_BINARY_DIR}/resources/app.rc")


add_executable(splender_gl ${SPLENDER_MAIN_SRC})
target_sources(splender_gl PRIVATE "${GENERATED_APP_RC}")


target_include_directories(splender_gl PRIVATE ${CMAKE_SOURCE_DIR}/src ${IMGUI_DIR})
target_link_libraries(splender_gl
    PRIVATE
        splender_core
        imgui
        glad::glad
        glfw
        glm::glm
        OpenGL::GL
)

# Link Assimp into the executable as well if available
if(assimp_FOUND)
    target_link_libraries(splender_gl PRIVATE assimp::assimp)
endif()

# Compiler warnings and flags
if (MSVC)
    target_compile_options(splender_gl PRIVATE /W4 /permissive-)
else()
    target_compile_options(splender_gl PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(imgui PROPERTIES POSITION_INDEPENDENT_CODE ON)
set_target_properties(splender_gl PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

install(TARGETS splender_gl RUNTIME DESTINATION bin)

message(STATUS "Built executable main: ${SPLENDER_MAIN_SRC}")
if (NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    message(STATUS "If you use vcpkg, configure cmake with -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake")
endif()
