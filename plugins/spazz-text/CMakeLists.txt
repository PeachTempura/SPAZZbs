cmake_minimum_required(VERSION 3.28...3.30)

add_library(spazz-text MODULE)
add_library(OBS::spazz-text ALIAS spazz-text)

target_sources(spazz-text PRIVATE spazz-text.c spazz-text-spec.h)

target_link_libraries(spazz-text PRIVATE OBS::libobs $<$<PLATFORM_ID:Windows>:OBS::w32-pthreads>)

if(OS_WINDOWS)
  configure_file(cmake/windows/obs-module.rc.in spazz-text.rc)
  target_sources(spazz-text PRIVATE spazz-text.rc)
endif()

set_target_properties_obs(spazz-text PROPERTIES FOLDER plugins PREFIX "")
