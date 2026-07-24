cmake_minimum_required(VERSION 3.24)
project(ApotheosisUIPreview LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

set(SRC
    preview_main.cpp
    PreviewWindow.cpp
    PreviewWindow.h
    ../widgets/IconFont.cpp
    ../widgets/IconFont.h
    ../widgets/StatusPill.cpp
    ../widgets/StatusPill.h
    ../widgets/MetricCard.cpp
    ../widgets/MetricCard.h
    ../widgets/TelemetryChart.cpp
    ../widgets/TelemetryChart.h
    ../widgets/SideNav.cpp
    ../widgets/SideNav.h
    ../widgets/TopNavBar.cpp
    ../widgets/TopNavBar.h
    ../widgets/CardWidget.cpp
    ../widgets/CardWidget.h
    ../pages/OverviewPage.cpp
    ../pages/OverviewPage.h
)

qt_standard_project_setup()
qt_add_executable(ApotheosisUIPreview ${SRC})

target_include_directories(ApotheosisUIPreview PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(ApotheosisUIPreview PRIVATE Qt6::Widgets)

add_custom_command(TARGET ApotheosisUIPreview POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_CURRENT_SOURCE_DIR}/preview.qss
        $<TARGET_FILE_DIR:ApotheosisUIPreview>/preview.qss
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:ApotheosisUIPreview>/assets
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_CURRENT_SOURCE_DIR}/../assets/tabler-icons.ttf
        $<TARGET_FILE_DIR:ApotheosisUIPreview>/assets/tabler-icons.ttf
)
