cmake_minimum_required(VERSION 3.28...3.30)

add_library(spazz-motion MODULE)
add_library(OBS::spazz-motion ALIAS spazz-motion)

target_sources(spazz-motion PRIVATE spazz-motion.c spazz-motion-anim.h)

target_link_libraries(spazz-motion PRIVATE OBS::libobs $<$<PLATFORM_ID:Windows>:OBS::w32-pthreads>)

if(OS_WINDOWS)
  configure_file(cmake/windows/obs-module.rc.in spazz-motion.rc)
  target_sources(spazz-motion PRIVATE spazz-motion.rc)
endif()

set_target_properties_obs(spazz-motion PROPERTIES FOLDER plugins PREFIX "")
