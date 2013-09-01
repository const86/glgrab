cmake_minimum_required(VERSION 2.8)
project(glgrab)

add_library(glgrab-audit SHARED audit.c)
set_target_properties(glgrab-audit PROPERTIES
  LINK_FLAGS "-nostdlib")

add_library(glgrab-preload SHARED
  preload.c
  mrb_write.c
  )
target_link_libraries(glgrab-preload rt GL)

add_executable(glgrab-monitor
  monitor.c
  mrb_read.c
)

add_library(glgrab-avformat SHARED
  avformat.c
  avformat-preload.c
  mrb_read.c
)
target_link_libraries(glgrab-avformat avformat rt)
