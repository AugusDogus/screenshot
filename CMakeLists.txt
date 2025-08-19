# cl /TC screenshot.c /MD /O1 /GL /Gy /DNDEBUG ^
#    /link user32.lib gdi32.lib msimg32.lib ^
#          /SUBSYSTEM:WINDOWS /LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO /DEBUG:NONE

cmake_minimum_required(VERSION 3.25)
project(screenshot C)

# Change the source filename here if yours is different (e.g. screenshot.c)
add_executable(screenshot WIN32 screenshot.c)

# Match flags exactly (avoid CMake's default /O2 and friends)
if (MSVC)
  # Compile flags (Release + MinSizeRel)
  set(CMAKE_C_FLAGS_RELEASE      "/O1 /Gy /GL /DNDEBUG /TC" CACHE STRING "" FORCE)
  set(CMAKE_C_FLAGS_MINSIZEREL   "/O1 /Gy /GL /DNDEBUG /TC" CACHE STRING "" FORCE)

  # Dynamic runtime (/MD), not static (/MT)
  set_property(TARGET screenshot PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreadedDLL$<$<CONFIG:Debug>:Debug>")

  # Linker flags (Release + MinSizeRel)
  set(CMAKE_EXE_LINKER_FLAGS_RELEASE    "/SUBSYSTEM:WINDOWS /LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO /DEBUG:NONE" CACHE STRING "" FORCE)
  set(CMAKE_EXE_LINKER_FLAGS_MINSIZEREL "/SUBSYSTEM:WINDOWS /LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO /DEBUG:NONE" CACHE STRING "" FORCE)
endif()

# Windows libs
target_link_libraries(screenshot PRIVATE user32 gdi32 msimg32)
