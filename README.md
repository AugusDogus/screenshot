# Screenshot

A small, fast region screenshot overlay written in C (Win32/GDI) for Windows x64. Darkens the desktop and provides an intuitive selection interface with clipboard integration. Features a system tray icon and global hotkey for easy access.

## Features

- **Selection Creation**: Left-click and drag to create a selection
- **Selection Movement**: Drag inside the selection to move it
- **Resize Handles**: Drag handles to resize (handles flip when crossing sides)
- **Clipboard Integration**: Copy selection to clipboard with Enter or Ctrl+C
- **Easy Exit**: Cancel/exit with Esc or right-click
- **System Tray**: Always accessible via system tray icon
- **Global Hotkey**: PrintScreen key triggers screenshot overlay instantly

## Usage

### Screenshot Interface
| Action | Result |
|--------|--------|
| **Left-click + drag** | Create a selection |
| **Drag inside selection** | Move the selection |
| **Drag handles** | Resize the selection |
| **Enter** or **Ctrl+C** | Copy to clipboard and exit |
| **Esc** or **Right-click** | Cancel and exit |

### System Tray
- **Left-click tray icon**: Launch screenshot overlay
- **Right-click tray icon**: Show context menu
  - **Take Screenshot**: Launch overlay (same as PrintScreen)
  - **Exit**: Close the application

### Global Hotkey
- **PrintScreen**: Instantly launch screenshot overlay (no modifiers needed)

## Building

### Prerequisites

- **MSVC** (Visual Studio Build Tools) with C++ and Windows SDK
- **CMake** (latest version)
- **Ninja** build system

### Quick Build

Using the provided CMake presets:

```bash
cmake --workflow --preset build
```

### Manual Build

```bash
mkdir build
cd build
cmake ..
cmake --build . --config Release
```
