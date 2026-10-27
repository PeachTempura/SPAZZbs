cmake_minimum_required(VERSION 3.28...3.30)

add_library(spazz-stinger MODULE)
add_library(OBS::spazz-stinger ALIAS spazz-stinger)

target_sources(spazz-stinger PRIVATE spazz-stinger.c spazz-stinger-spec.h)

target_link_libraries(spazz-stinger PRIVATE OBS::libobs $<$<PLATFORM_ID:Windows>:OBS::w32-pthreads>)

if(OS_WINDOWS)
  configure_file(cmake/windows/obs-module.rc.in spazz-stinger.rc)
  target_sources(spazz-stinger PRIVATE spazz-stinger.rc)
endif()

set_target_properties_obs(spazz-stinger PROPERTIES FOLDER plugins PREFIX "")
