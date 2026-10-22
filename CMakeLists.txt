cmake_minimum_required(VERSION 3.16)
project(lvgl_dash_sim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

file(GLOB_RECURSE LVGL_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/lvgl/src/*.c)

add_executable(dash_sim
  sim/main.c
  sim/png_writer.c
  ui/src/ui_widgets.c
  ui/src/ui_dashboard.c
  ui/src/font_cjk_16.c
  ${LVGL_SOURCES}
)

target_include_directories(dash_sim PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/sim
  ${CMAKE_CURRENT_SOURCE_DIR}/ui/include
  ${CMAKE_CURRENT_SOURCE_DIR}/lvgl
)

# 让 lvgl 用 #include "lv_conf.h" 从 include 路径找配置
target_compile_definitions(dash_sim PRIVATE LV_CONF_INCLUDE_SIMPLE)

# 系统 zlib（macOS SDK 自带），PNG 输出用
target_link_libraries(dash_sim PRIVATE z)
target_link_libraries(dash_sim PRIVATE m)

target_compile_options(dash_sim PRIVATE -Wall -Wextra -Wno-unused-parameter)
