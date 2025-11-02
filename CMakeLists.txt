
# apparently i need this stuff
cmake_minimum_required(VERSION 3.15)
project(SplenderProof LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(glfw3 CONFIG REQUIRED)   
find_package(glad CONFIG REQUIRED)   
find_package(glm CONFIG REQUIRED)

add_executable(splender_gl src/main_gl.cpp)
target_link_libraries(splender_gl PRIVATE glfw glad::glad glm::glm)
set_target_properties(splender_gl PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)


