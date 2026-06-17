cmake_minimum_required(VERSION 3.24)
project(ApotheosisUI LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

file(GLOB_RECURSE SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
list(FILTER SOURCES EXCLUDE REGEX ".*/build/.*")

qt_standard_project_setup()
qt_add_executable(ApotheosisUI ${SOURCES})

target_include_directories(ApotheosisUI PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ApotheosisUI PRIVATE Qt6::Widgets)

if(WIN32)
    set_target_properties(ApotheosisUI PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
