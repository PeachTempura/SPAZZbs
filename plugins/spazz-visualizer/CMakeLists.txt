cmake_minimum_required(VERSION 3.28...3.30)

add_library(spazz-visualizer MODULE)
add_library(OBS::spazz-visualizer ALIAS spazz-visualizer)

target_sources(spazz-visualizer PRIVATE spazz-analyzer.c spazz-analyzer.h spazz-visualizer.c)

target_link_libraries(spazz-visualizer PRIVATE OBS::libobs $<$<PLATFORM_ID:Windows>:OBS::w32-pthreads>)

if(OS_WINDOWS)
  configure_file(cmake/windows/obs-module.rc.in spazz-visualizer.rc)
  target_sources(spazz-visualizer PRIVATE spazz-visualizer.rc)
endif()

set_target_properties_obs(spazz-visualizer PROPERTIES FOLDER plugins PREFIX "")
