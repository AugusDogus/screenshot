# Matches:
#   Windows: cl /TC screenshot_windows.c /MD /O1 /GL /Gy /DNDEBUG ^
#      /link user32.lib gdi32.lib msimg32.lib shell32.lib ^
#            /SUBSYSTEM:WINDOWS /LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO /DEBUG:NONE
#   macOS: clang screenshot_macos.m -framework Cocoa -framework Carbon -o screenshot

cmake_minimum_required(VERSION 3.25)

if(APPLE)
  project(screenshot C OBJC)
else()
  project(screenshot C)
endif()

if(APPLE)
  # macOS build
  add_executable(screenshot MACOSX_BUNDLE screenshot_macos.m)
  
  target_link_libraries(screenshot PRIVATE
    "-framework Cocoa"
    "-framework Carbon"
    "-framework CoreGraphics"
    "-framework ScreenCaptureKit"
  )
  
  set_target_properties(screenshot PROPERTIES
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.screenshot.app"
    MACOSX_BUNDLE_BUNDLE_NAME "Screenshot"
    MACOSX_BUNDLE_BUNDLE_VERSION "1.0"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/Info.plist"
  )
  
  # Enable ARC
  set_property(TARGET screenshot PROPERTY
    COMPILE_FLAGS "-fobjc-arc")
  
elseif(WIN32)
  # Windows build
  add_executable(screenshot WIN32 screenshot_windows.c)

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

elseif(UNIX AND NOT APPLE)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SHOT_LINUX REQUIRED IMPORTED_TARGET gtk4 gtk4-layer-shell-0)
  add_executable(screenshot screenshot_linux.c)
  target_link_libraries(screenshot PRIVATE PkgConfig::SHOT_LINUX)
endif()
