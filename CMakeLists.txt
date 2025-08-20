# Matches:
#   cl /TC screenshot.c /MD /O1 /GL /Gy /DNDEBUG ^
#      /link user32.lib gdi32.lib msimg32.lib shell32.lib ^
#            /SUBSYSTEM:WINDOWS /LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO /DEBUG:NONE

cmake_minimum_required(VERSION 3.25)
project(screenshot C)

add_executable(screenshot WIN32 screenshot.c)

if (MSVC)
  set(CMAKE_C_FLAGS_RELEASE    "/TC /O1 /Gy /GL /DNDEBUG" CACHE STRING "" FORCE)
  set(CMAKE_C_FLAGS_MINSIZEREL "/TC /O1 /Gy /GL /DNDEBUG" CACHE STRING "" FORCE)

  set_property(TARGET screenshot PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

  set(CMAKE_EXE_LINKER_FLAGS_RELEASE
      "/SUBSYSTEM:WINDOWS /LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO /DEBUG:NONE"
      CACHE STRING "" FORCE)
  set(CMAKE_EXE_LINKER_FLAGS_MINSIZEREL
      "/SUBSYSTEM:WINDOWS /LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO /DEBUG:NONE"
      CACHE STRING "" FORCE)

  set_property(TARGET screenshot PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

target_link_libraries(screenshot PRIVATE user32 gdi32 msimg32 shell32)
