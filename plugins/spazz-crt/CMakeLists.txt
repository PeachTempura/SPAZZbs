cmake_minimum_required(VERSION 3.28...3.30)

add_library(spazz-crt MODULE)
add_library(OBS::spazz-crt ALIAS spazz-crt)

target_sources(spazz-crt PRIVATE spazz-crt.c spazz-crt-params.h)

target_link_libraries(spazz-crt PRIVATE OBS::libobs $<$<PLATFORM_ID:Windows>:OBS::w32-pthreads>)

if(OS_WINDOWS)
  configure_file(cmake/windows/obs-module.rc.in spazz-crt.rc)
  target_sources(spazz-crt PRIVATE spazz-crt.rc)
endif()

set_target_properties_obs(spazz-crt PROPERTIES FOLDER plugins PREFIX "")
