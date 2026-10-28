cmake_minimum_required(VERSION 3.28...3.30)

add_library(spazz-sponsor MODULE)
add_library(OBS::spazz-sponsor ALIAS spazz-sponsor)

target_sources(spazz-sponsor PRIVATE spazz-sponsor.c spazz-sponsor-spec.h)

target_link_libraries(spazz-sponsor PRIVATE OBS::libobs $<$<PLATFORM_ID:Windows>:OBS::w32-pthreads>)

if(OS_WINDOWS)
  configure_file(cmake/windows/obs-module.rc.in spazz-sponsor.rc)
  target_sources(spazz-sponsor PRIVATE spazz-sponsor.rc)
endif()

set_target_properties_obs(spazz-sponsor PROPERTIES FOLDER plugins PREFIX "")
