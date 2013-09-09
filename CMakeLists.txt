cmake_minimum_required(VERSION 2.8)
project(glgrab)

add_definitions(-std=c99 -fvisibility=hidden)

add_library(glgrab-audit SHARED audit.c)

add_library(glgrab-preload SHARED
  preload.c
  mrb_write.c
  )
target_link_libraries(glgrab-preload dl rt GL)

add_executable(glgrab-monitor
  monitor.c
  mrb_read.c
)

add_library(glgrab-avformat SHARED
  avformat.c
  avformat-preload.c
  mrb_read.c
)
target_link_libraries(glgrab-avformat avcodec avformat)

add_executable(glgrab-export
  export.c
  avformat.c
  mrb_read.c
)
target_link_libraries(glgrab-export avcodec avformat avutil avdevice swscale)
set_target_properties(glgrab-export PROPERTIES
  COMPILE_FLAGS "-pthread"
  LINK_FLAGS "-pthread")