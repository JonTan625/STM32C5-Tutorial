# file-format: 1.0.0
if(CMAKE_BUILD_TYPE STREQUAL "release_GCC_NUCLEO-C542RC")
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE ../../application/example.c ../../application/main.c ../../application/example.h ../../application/main.h ../../application/p46.h ../../application/p46_sampler.c ../../application/p46_terminal.c ../../application/p46_alarm.c ../../application/p46_time.c ../../application/p46_config.c ../../application/p46_health.c)
elseif(CMAKE_BUILD_TYPE STREQUAL "debug_GCC_NUCLEO-C542RC")
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE ../../application/example.c ../../application/main.c ../../application/example.h ../../application/main.h ../../application/p46.h ../../application/p46_sampler.c ../../application/p46_terminal.c ../../application/p46_alarm.c ../../application/p46_time.c ../../application/p46_config.c ../../application/p46_health.c)
endif()
