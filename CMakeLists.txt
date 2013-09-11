cmake_minimum_required(VERSION 2.8)
project(glgrab)
include(GNUInstallDirs)

set(GLGRAB_LIBDIR "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/glgrab")

add_definitions(-std=c99 -pedantic -Wall -fvisibility=hidden)

add_library(glgrab-audit SHARED audit.c)

add_library(glgrab-preload SHARED
  preload.c
  mrb_write.c
  )
target_link_libraries(glgrab-preload dl rt GL)

add_executable(glgrab-monitor EXCLUDE_FROM_ALL
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

configure_file(glgrab.in glgrab ESCAPE_QUOTES @ONLY)
configure_file(glgrab-ffmpeg.in glgrab-ffmpeg ESCAPE_QUOTES @ONLY)

install(TARGETS glgrab-audit glgrab-preload glgrab-avformat glgrab-export
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${GLGRAB_LIBDIR}
)

install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/glgrab
  ${CMAKE_CURRENT_BINARY_DIR}/glgrab-ffmpeg
  DESTINATION ${CMAKE_INSTALL_BINDIR}
)
