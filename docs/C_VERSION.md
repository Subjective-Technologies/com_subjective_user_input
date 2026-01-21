# input_unified.c - C Version of the KVM Client

A cross-platform KVM (Keyboard, Video, Mouse) client written in C that can be compiled into a single binary for Linux, Windows, and macOS.

## Features

- **Single Source File**: All functionality in `input_unified.c` (~1800 lines)
- **Cross-Platform**: Compiles on Linux, Windows, and macOS
- **WebSocket Communication**: Uses libwebsockets for reliable server communication
- **Input Capture**: Captures keyboard and mouse events on the "main" computer
- **Input Injection**: Injects input events on "player" computers
- **Monitor Detection**: Auto-detects connected monitors
- **SSL Support**: Optional TLS encryption for secure connections

## Platform Support Status

| Feature | Linux | Windows | macOS |
|---------|-------|---------|-------|
| Input Capture | ✅ evdev | ✅ Low-level hooks | 🔧 IOKit (stub) |
| Input Injection | ✅ XTest | ✅ SendInput | 🔧 CGEvent (stub) |
| Monitor Detection | ✅ XRandR | ✅ EnumDisplayMonitors | ✅ CoreGraphics |
| WebSocket | ✅ libwebsockets | ✅ libwebsockets | ✅ libwebsockets |
| SSL/TLS | ✅ OpenSSL | ✅ OpenSSL | ✅ OpenSSL |

**Legend**: ✅ Fully implemented | 🔧 Basic/stub implementation

## Building

### Prerequisites

#### Linux (Ubuntu/Debian)
```bash
# Install dependencies
make install-deps-linux

# Or manually:
sudo apt-get install build-essential libwebsockets-dev libssl-dev \
    libx11-dev libxtst-dev libxrandr-dev pkg-config

# Add user to input group (for evdev access without sudo)
sudo usermod -a -G input $USER
# Log out and back in for group change to take effect
```

#### macOS
```bash
# Install Homebrew if not already installed
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# Install dependencies
brew install libwebsockets openssl
```

#### Windows (MSYS2/MinGW)
```powershell
# Install MSYS2 from https://www.msys2.org/
# Then in MSYS2 MinGW64 terminal:
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-libwebsockets mingw-w64-x86_64-openssl
```

### Compilation

```bash
# Build for current platform
make

# Build for specific platform
make linux
make macos
make windows  # Cross-compile from Linux

# Clean build files
make clean
```

### Manual Compilation

```bash
# Linux
gcc -o input_unified input_unified.c \
    -lwebsockets -lssl -lcrypto -lX11 -lXtst -lXrandr -lpthread -lm

# macOS
clang -o input_unified input_unified.c \
    -I/usr/local/include -L/usr/local/lib \
    -lwebsockets -lssl -lcrypto \
    -framework CoreFoundation -framework IOKit \
    -framework ApplicationServices -framework Carbon

# Windows (MinGW)
x86_64-w64-mingw32-gcc -o input_unified.exe input_unified.c \
    -lwebsockets -lssl -lcrypto -lws2_32 -luser32
```

## Usage

```bash
# Show help
./input_unified --help

# Run as main computer (captures input)
./input_unified --role main --server ws://192.168.1.100:8765

# Run as player computer (receives input)
./input_unified --role player --server ws://192.168.1.100:8765

# With SSL
./input_unified --role player --server wss://myserver.com:443 --ssl

# Custom computer ID
./input_unified --role player --server ws://server:8765 --computer-id mypc

# Debug mode (verbose logging)
./input_unified --role main --server ws://localhost:8765 --debug
```

### Command Line Options

| Option | Description | Default |
|--------|-------------|---------|
| `--server URL` | WebSocket server URL | `ws://localhost:8765` |
| `--computer-id ID` | Unique identifier for this computer | hostname |
| `--role ROLE` | `main` or `player` | `player` |
| `--port PORT` | Server port (if not in URL) | `8765` |
| `--ssl` | Use SSL/TLS encryption | disabled |
| `--debug` | Enable debug logging | disabled |
| `--help` | Show help message | - |

## Architecture

### Main Computer (role: main)
- Captures all keyboard and mouse events via evdev/platform API
- Sends events to the server via WebSocket
- When this computer is INACTIVE, grabs input devices to prevent local input
- Warps cursor to center when becoming inactive

### Player Computer (role: player)
- Receives input events from the server
- Injects events into the local system via XTest/platform API
- Does not capture local input (except for edge detection)

### Communication Protocol

Messages are JSON-formatted WebSocket text frames:

```json
// Registration
{
    "type": "register_device",
    "computer_id": "mypc",
    "is_main": true,
    "monitors": [{"monitor_id": "m0", "x": 0, "y": 0, "width": 1920, "height": 1080}]
}

// Input Event
{
    "type": "input_event",
    "event_type": "keyboard",
    "data": {"action": "press", "key": "a", "is_special": false},
    "device_id": "mainpc",
    "delta_ms": 16.5
}

// Mouse Move
{
    "type": "input_event",
    "event_type": "mouse_move",
    "data": {"x": 500, "y": 300},
    "device_id": "mainpc",
    "delta_ms": 8.2
}
```

## Comparison with Python Version

| Aspect | Python (`input_unified.py`) | C (`input_unified.c`) |
|--------|---------------------------|----------------------|
| Lines of Code | ~2100 | ~1800 |
| Dependencies | pynput, websockets, evdev | libwebsockets, X11 |
| Startup Time | ~1-2s | ~50ms |
| Memory Usage | ~30-50MB | ~2-5MB |
| Distribution | Requires Python + venv | Single binary |
| Cross-platform | Via pynput | Via #ifdef |

## Troubleshooting

### Permission Denied (Linux)
```bash
# Add user to input group
sudo usermod -a -G input $USER
# Log out and back in

# Or run with sudo (not recommended for production)
sudo ./input_unified --role main
```

### X11 Display Not Found
```bash
# Make sure DISPLAY is set
export DISPLAY=:0

# For SSH sessions, use X forwarding
ssh -X user@host
```

### libwebsockets Not Found
```bash
# Check if installed
pkg-config --modversion libwebsockets

# Install if missing
sudo apt-get install libwebsockets-dev  # Debian/Ubuntu
brew install libwebsockets              # macOS
```

### Connection Refused
- Ensure the server (`input_server.py`) is running
- Check firewall allows the port (default 8765)
- Verify the server URL is correct

## Extending

### Adding Windows Support

Windows input capture, injection, and monitor detection are implemented via
low-level hooks, `SendInput`, and `EnumDisplayMonitors`. Future enhancements
can focus on edge cases and clipboard interoperability improvements.

### Adding macOS Support

The macOS stubs need:

1. **Input Capture**: Implement IOKit HID Manager event tap
2. **Input Injection**: Complete `CGEventPost()` with proper key code mapping
3. **Accessibility Permissions**: Handle macOS security prompts

## License

MIT License - Same as the main project.

