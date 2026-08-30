cmake_minimum_required(VERSION 3.21)
project(MusicPlayer VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Multimedia)
find_package(TagLib REQUIRED)

qt_standard_project_setup()

qt_add_executable(music_player
    main.cpp
    cover_mac.mm
)

find_library(APPLE_IMAGEIO ImageIO REQUIRED)
find_library(APPLE_COREGRAPHICS CoreGraphics REQUIRED)

target_link_libraries(music_player PRIVATE
    Qt6::Widgets
    Qt6::Multimedia
    TagLib::TagLib
    iconv # macOS 系统库，用于 GBK 歌词解码
    ${APPLE_IMAGEIO} # 封面兜底解码（webp 等 Qt 官方包未带插件的格式）
    ${APPLE_COREGRAPHICS}
)
