# Screenshot

A small, fast region screenshot overlay for **Windows** (C/Win32/GDI), **macOS** (Objective-C/Cocoa), and **Linux Wayland/COSMIC** (C/GTK4 + layer-shell). It darkens the desktop, provides an intuitive selection interface with clipboard integration, and can be launched from the tray, menu bar, or COSMIC panel applet.

## Features

- **Selection Creation**: Left-click and drag to create a selection
- **Selection Movement**: Drag inside the selection to move it
- **Resize Handles**: Drag handles to resize (handles flip when crossing sides)
- **Clipboard Integration**: Copy selection to clipboard with Enter or Cmd+C (macOS) / Ctrl+C (Windows)
- **Easy Exit**: Cancel/exit with Esc or right-click
- **System Tray / Menu Bar**: Always accessible via tray icon (Windows) or menu bar (macOS)
- **COSMIC Applet**: Launch from the COSMIC panel on Linux
- **Global Hotkey**:
  - **Windows**: PrintScreen key triggers screenshot overlay instantly
  - **macOS**: Cmd+Shift+4 triggers screenshot overlay

## Usage

### Screenshot Interface

| Action                                                | Result                     |
| ----------------------------------------------------- | -------------------------- |
| **Left-click + drag**                                 | Create a selection         |
| **Drag inside selection**                             | Move the selection         |
| **Drag handles**                                      | Resize the selection       |
| **Enter** or **Cmd+C** (macOS) / **Ctrl+C** (Windows) | Copy to clipboard and exit |
| **Esc** or **Right-click**                            | Cancel and exit            |

### System Tray (Windows) / Menu Bar (macOS)

- **Left-click icon**: Launch screenshot overlay
- **Right-click icon** (Windows) / **Click icon** (macOS): Show context menu
  - **Take Screenshot**: Launch overlay
  - **Exit** / **Quit**: Close the application

### Global Hotkey

- **Windows**: PrintScreen (no modifiers needed)
- **macOS**: Cmd+Shift+4

## Building

### Windows

#### Prerequisites

- **MSVC** (Visual Studio Build Tools) with C++ and Windows SDK
- **CMake** (latest version)
- **Ninja** build system

#### Quick Build

Using the provided CMake presets:

```bash
cmake --workflow --preset build
```

#### Manual Build

```bash
mkdir build
cd build
cmake ..
cmake --build . --config Release
```

### macOS

#### Prerequisites

- **Xcode** or **Command Line Tools** (for clang/Objective-C)
- **CMake** (latest version)
- **macOS 12.3+** (Monterey or later) for ScreenCaptureKit

#### Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The app bundle will be at `build/screenshot.app`.

#### First Run

On first launch, macOS will prompt for **Screen Recording** permission. Grant permission in:

- **System Settings > Privacy & Security > Screen Recording**

Then restart the app.

#### Running

```bash
open build/screenshot.app
```

Or double-click `screenshot.app` in Finder.

### Linux (Wayland/COSMIC)

#### Prerequisites

- **CMake**
- **pkg-config**
- **GTK4**
- **gtk4-layer-shell**
- **wl-clipboard**
- An XDG desktop portal screenshot backend such as **COSMIC**

#### Build

```bash
cmake -B build-linux -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux
```

#### Build COSMIC Applet

```bash
cargo build --release --manifest-path cosmic-applet/Cargo.toml
```

#### Install Locally

```bash
install -Dm755 build-linux/screenshot ~/.local/bin/screenshot
```

For the COSMIC panel applet, also install:

```bash
install -Dm755 cosmic-applet/target/release/cosmic-applet-screenshot ~/.local/bin/cosmic-applet-screenshot
install -Dm644 cosmic-applet/data/com.screenshot.CosmicApplet.desktop ~/.local/share/applications/com.screenshot.CosmicApplet.desktop
```
