cmake_minimum_required(VERSION 2.8)
project(glgrab)
include(GNUInstallDirs)

set(GLGRAB_INSTALL_LIBDIR "${CMAKE_INSTALL_LIBDIR}/glgrab")
set(GLGRAB_LIBDIR "${CMAKE_INSTALL_PREFIX}/${GLGRAB_INSTALL_LIBDIR}")

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
  mrb_read.c
)
target_link_libraries(glgrab-avformat avcodec avformat opencv_core opencv_imgproc rt)
set_target_properties(glgrab-avformat PROPERTIES SOVERSION 0)

add_library(glgrab-av-preload SHARED
  avformat-preload.c
)
target_link_libraries(glgrab-av-preload glgrab-avformat)

add_executable(glgrab-export
  export.c
)
target_link_libraries(glgrab-export glgrab-avformat avcodec avformat avutil avdevice swscale rt)
set_target_properties(glgrab-export PROPERTIES
  COMPILE_FLAGS "-pthread"
  LINK_FLAGS "-pthread")

configure_file(glgrab.in glgrab ESCAPE_QUOTES @ONLY)
configure_file(glgrab-av.in glgrab-av ESCAPE_QUOTES @ONLY)

install(TARGETS glgrab-avformat
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(TARGETS glgrab-audit glgrab-preload glgrab-av-preload glgrab-export
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${GLGRAB_LIBDIR}
)

install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/glgrab
  ${CMAKE_CURRENT_BINARY_DIR}/glgrab-av
  DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES README
  DESTINATION ${CMAKE_INSTALL_DOCDIR}
)
