# file-format: 1.0.0
if(CMAKE_BUILD_TYPE STREQUAL "release_GCC_NUCLEO-C542RC")
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE ../../application/example.c ../../application/main.c ../../application/example.h ../../application/main.h ../../application/p48.h ../../application/p48_sense.c ../../application/p48_terminal.c ../../application/p48_log.c ../../application/p48_time.c ../../application/p48_power.c ../../application/p48_health.c)
elseif(CMAKE_BUILD_TYPE STREQUAL "debug_GCC_NUCLEO-C542RC")
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE ../../application/example.c ../../application/main.c ../../application/example.h ../../application/main.h ../../application/p48.h ../../application/p48_sense.c ../../application/p48_terminal.c ../../application/p48_log.c ../../application/p48_time.c ../../application/p48_power.c ../../application/p48_health.c)
endif()
