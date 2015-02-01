cmake_minimum_required(VERSION 2.8)
project(glgrab C)
set(VERSION 0.2)

include(GNUInstallDirs)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED YES)

set(CMAKE_C_VISIBILITY_PRESET hidden)

option(WANT_EGL "Build EGL plugin" ON)
option(WANT_GLX "Build GLX plugin" ON)
option(WANT_FFMPEG "Build FFmpeg plugin" ON)

set(GLGRAB_INSTALL_LIBDIR "${CMAKE_INSTALL_LIBDIR}/glgrab")
set(GLGRAB_LIBDIR "${CMAKE_INSTALL_PREFIX}/${GLGRAB_INSTALL_LIBDIR}")

add_compile_options(-Wall)

add_library(glgrab SHARED glgrab.c bgra2yuv420p.c mrb_write.c)
target_link_libraries(glgrab PRIVATE rt GL)
set_target_properties(glgrab PROPERTIES SOVERSION 0 VERSION ${VERSION})
install(TARGETS glgrab EXPORT glgrab LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES mrb.h glgrab.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/glgrab)

if(WANT_GLX)
  add_library(glgrab-audit-glx MODULE audit.c audit-glx.c)

  add_library(glgrab-glx MODULE glx.c)
  target_link_libraries(glgrab-glx PRIVATE glgrab dl GL X11)
  install(TARGETS glgrab-audit-glx glgrab-glx LIBRARY DESTINATION ${GLGRAB_LIBDIR})

  configure_file(glgrab.in glgrab ESCAPE_QUOTES @ONLY)
  install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/glgrab DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(WANT_EGL)
  add_library(glgrab-audit-egl MODULE audit.c audit-egl.c)

  add_library(glgrab-egl MODULE egl.c)
  target_link_libraries(glgrab-egl PRIVATE glgrab dl EGL)
  install(TARGETS glgrab-audit-egl glgrab-egl LIBRARY DESTINATION ${GLGRAB_LIBDIR})

  configure_file(glgrab-egl.in glgrab-egl ESCAPE_QUOTES @ONLY)
  install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/glgrab-egl DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(WANT_FFMPEG)
  add_library(glgrab-avformat SHARED avformat.c mrb_read.c)
  target_link_libraries(glgrab-avformat PRIVATE avcodec avformat rt)
  set_target_properties(glgrab-avformat PROPERTIES SOVERSION 0 VERSION ${VERSION})
  install(TARGETS glgrab-avformat EXPORT glgrab LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})

  add_library(glgrab-av-preload MODULE avformat-preload.c)
  target_link_libraries(glgrab-av-preload PRIVATE glgrab-avformat)
  install(TARGETS glgrab-av-preload LIBRARY DESTINATION ${GLGRAB_LIBDIR})

  configure_file(glgrab-av.in glgrab-av ESCAPE_QUOTES @ONLY)
  install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/glgrab-av DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(FILES README DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(EXPORT glgrab DESTINATION ${GLGRAB_INSTALL_LIBDIR})
