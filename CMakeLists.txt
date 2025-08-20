# Matches:
#   cl /TC screenshot.c /MD /O1 /GL /Gy /DNDEBUG ^
#      /link user32.lib gdi32.lib msimg32.lib ^
#            /SUBSYSTEM:WINDOWS /LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO /DEBUG:NONE

cmake_minimum_required(VERSION 3.25)
project(screenshot C)

# Change the source filename here if needed
add_executable(screenshot WIN32 screenshot.c)

if (MSVC)
  # Compile flags (Release / MinSizeRel) — size-first, LTO, force C mode (/TC)
  set(CMAKE_C_FLAGS_RELEASE      "/TC /O1 /Gy /GL /DNDEBUG" CACHE STRING "" FORCE)
  set(CMAKE_C_FLAGS_MINSIZEREL   "/TC /O1 /Gy /GL /DNDEBUG" CACHE STRING "" FORCE)

  # Dynamic CRT (/MD) to keep the exe small (not /MT)
  set_property(TARGET screenshot PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreadedDLL$<$<CONFIG:Debug>:Debug>")

  # Linker flags — Windows GUI, LTO, fold & strip, no incremental, no debug info
  set(CMAKE_EXE_LINKER_FLAGS_RELEASE
      "/SUBSYSTEM:WINDOWS /LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO /DEBUG:NONE"
      CACHE STRING "" FORCE)
  set(CMAKE_EXE_LINKER_FLAGS_MINSIZEREL
      "/SUBSYSTEM:WINDOWS /LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO /DEBUG:NONE"
      CACHE STRING "" FORCE)

  # IPO pairs with /GL + /LTCG
  set_property(TARGET screenshot PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

# Win32 libs
target_link_libraries(screenshot PRIVATE user32 gdi32 msimg32 shell32)
