cmake_minimum_required(VERSION 3.16)
project(screenshot C)

# Options
option(MIN_SIZE "Prefer /O1 (min size) instead of /O2 (speed)" ON)
option(USE_STATIC_CRT "Use /MT instead of /MD" OFF)

# Executable (WIN32 -> /SUBSYSTEM:WINDOWS)
add_executable(screenshot WIN32 screenshot.c)

# Unicode + NDEBUG
target_compile_definitions(screenshot PRIVATE UNICODE _UNICODE NDEBUG)

# Release flags (avoid /O2 default clobbering)
if (MSVC)
  if (MIN_SIZE)
    set(CMAKE_C_FLAGS_RELEASE "/O1 /Gy /DNDEBUG" CACHE STRING "" FORCE)
  else()
    set(CMAKE_C_FLAGS_RELEASE "/O2 /Gy /DNDEBUG" CACHE STRING "" FORCE)
  endif()
  set(CMAKE_EXE_LINKER_FLAGS_RELEASE "/OPT:REF /OPT:ICF /INCREMENTAL:NO /DEBUG:NONE" CACHE STRING "" FORCE)
endif()

# IPO (LTO): /GL + /LTCG
set_property(TARGET screenshot PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)

# MSVC CRT: /MD (DLL) by default; /MT if requested
if (MSVC)
  if (USE_STATIC_CRT)
    set_property(TARGET screenshot PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  else()
    set_property(TARGET screenshot PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL$<$<CONFIG:Debug>:Debug>")
  endif()
endif()

# Win32 libs
target_link_libraries(screenshot PRIVATE user32 gdi32 msimg32)
