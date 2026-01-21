# Windows Porting Guide for input_unified.c

This document outlines what needs to be implemented to make the `cinput` directory fully functional on Windows.

## Current Status

Based on the code analysis, Windows support is **partially implemented** with stubs:

| Component | Status | Notes |
|-----------|--------|-------|
| **Input Injection** | 🔧 Partial | Mouse works, keyboard needs mapping |
| **Monitor Detection** | 🔧 Partial | Only primary monitor, needs multi-monitor support |
| **Input Capture** | ❌ Missing | No implementation at all |
| **WebSocket Client** | ✅ Complete | Uses libwebsockets (cross-platform) |
| **SSL/TLS** | ✅ Complete | Uses OpenSSL (cross-platform) |

## Required Implementations

### 1. Input Capture (HIGH PRIORITY - Missing)

**Location**: Need to add Windows input capture thread (similar to `input_capture_thread()` for Linux)

**What's needed**:
- Implement `RegisterRawInputDevices()` to register for keyboard and mouse input
- Create a Windows message loop thread to process `WM_INPUT` messages
- Convert Windows virtual key codes to the internal key representation
- Capture mouse position changes and button events
- Send captured events to the server via WebSocket

**API Requirements**:
- `RegisterRawInputDevices()` - Register input devices
- `GetRawInputData()` - Process input messages
- `GetMessage()` / `PeekMessage()` - Windows message loop
- `GetCursorPos()` - Get mouse position (fallback)

**Implementation approach**:
```c
// Create a separate thread that runs a Windows message loop
static void* windows_input_capture_thread(void *arg) {
    ClientState *client = (ClientState*)arg;
    
    // Register raw input devices for keyboard and mouse
    RAWINPUTDEVICE rid[2];
    rid[0].usUsagePage = 0x01;  // Generic Desktop
    rid[0].usUsage = 0x06;      // Keyboard
    rid[0].dwFlags = RIDEV_INPUTSINK;
    rid[0].hwndTarget = NULL;
    
    rid[1].usUsagePage = 0x01;  // Generic Desktop
    rid[1].usUsage = 0x02;      // Mouse
    rid[1].dwFlags = RIDEV_INPUTSINK;
    rid[1].hwndTarget = NULL;
    
    RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE));
    
    // Create hidden window to receive messages
    HWND hwnd = CreateWindow(...);
    
    // Message loop
    MSG msg;
    while (client->running) {
        if (PeekMessage(&msg, hwnd, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_INPUT) {
                // Process raw input
                UINT dwSize = 0;
                GetRawInputData((HRAWINPUT)msg.lParam, RID_INPUT, NULL, &dwSize, sizeof(RAWINPUTHEADER));
                // ... extract and send input events
            }
            DispatchMessage(&msg);
        }
        Sleep(1);  // Small delay to prevent CPU spinning
    }
}
```

**Key Challenges**:
- Need to create a hidden window or use `HWND_MESSAGE` for message-only window
- Virtual key code mapping to the key names used by the protocol
- Mouse relative movement vs absolute position
- Low-level hooks may require running as administrator (consider alternatives)

### 2. Keyboard Injection (MEDIUM PRIORITY - Stub)

**Location**: `inject_key_windows()` function at line ~2107

**What's needed**:
- Map key names (like "a", "Enter", "Shift") to Windows virtual key codes (VK_*)
- Handle special keys (Ctrl, Alt, Shift, Win)
- Handle extended keys (right Ctrl, right Alt, etc.)
- Support for all standard keys and modifiers

**Implementation approach**:
```c
static void inject_key_windows(const char *key, bool is_special, bool press) {
    INPUT input = {0};
    input.type = INPUT_KEYBOARD;
    input.ki.dwFlags = press ? 0 : KEYEVENTF_KEYUP;
    
    // Map key name to VK code
    WORD vk_code = map_key_name_to_vk(key, is_special);
    if (vk_code == 0) {
        LOG_WARN("Unknown key: %s", key);
        return;
    }
    
    // Check if extended key (right Ctrl, right Alt, etc.)
    if (is_extended_key(vk_code)) {
        input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }
    
    input.ki.wVk = vk_code;
    SendInput(1, &input, sizeof(INPUT));
}
```

**Key Mapping Function**:
- Create a lookup table or function to map key names to VK codes
- Handle case-sensitive keys (letter keys)
- Handle special keys: Enter, Tab, Backspace, Delete, Arrow keys, etc.
- Handle modifier keys: Shift, Ctrl, Alt, Win

### 3. Monitor Detection (LOW PRIORITY - Partial)

**Location**: `detect_monitors_windows()` function at line ~1066

**What's needed**:
- Use `EnumDisplayMonitors()` to enumerate all connected monitors
- Get monitor position (x, y) and size (width, height)
- Generate unique monitor IDs (m0, m1, m2, etc.)
- Handle monitor arrangement (extended displays)

**Implementation approach**:
```c
// Callback for EnumDisplayMonitors
BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor, 
                              LPRECT lprcMonitor, LPARAM dwData) {
    Monitor *monitors = (Monitor*)dwData;
    static int count = 0;
    
    MONITORINFOEX mi;
    mi.cbSize = sizeof(mi);
    GetMonitorInfo(hMonitor, &mi);
    
    snprintf(monitors[count].monitor_id, sizeof(monitors[count].monitor_id), 
             "m%d", count);
    monitors[count].x = mi.rcMonitor.left;
    monitors[count].y = mi.rcMonitor.top;
    monitors[count].width = mi.rcMonitor.right - mi.rcMonitor.left;
    monitors[count].height = mi.rcMonitor.bottom - mi.rcMonitor.top;
    
    count++;
    return TRUE;
}

static int detect_monitors_windows(Monitor *monitors, int max_monitors) {
    int count = 0;
    EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, (LPARAM)monitors);
    return count;
}
```

### 4. Input Grabbing/Blocking (LOW PRIORITY - Optional)

**For "main" role when inactive**: Currently Linux version can "grab" input to prevent local input. On Windows, this is more complex:

**Options**:
1. Use `SetWindowsHookEx()` with low-level hooks (requires admin)
2. Use `BlockInput()` (deprecated, affects entire system)
3. Accept local input but ignore it in message loop
4. Use raw input filtering

**Recommendation**: Implement input filtering in the message loop rather than system-wide blocking.

### 5. Threading (Windows-specific)

**Location**: Input capture thread creation at line ~3782 (Linux section)

**What's needed**:
- Use Windows threading API (`CreateThread()` or `_beginthreadex()`) instead of pthreads
- Ensure thread-safe operations
- Proper cleanup on thread exit

**Current issue**: The code only starts input capture threads for Linux:
```c
#ifdef PLATFORM_LINUX
    /* Start input capture thread */
    if (strcmp(g_client.config.role, "main") == 0) {
        // ... starts thread
    }
#endif
```

**Fix**: Add Windows equivalent section:
```c
#ifdef PLATFORM_WINDOWS
    /* Start Windows input capture thread */
    if (strcmp(g_client.config.role, "main") == 0) {
        g_client.input_thread_running = true;
        g_client.input_thread = CreateThread(NULL, 0, 
                                             windows_input_capture_thread, 
                                             &g_client, 0, NULL);
        if (!g_client.input_thread) {
            LOG_ERROR("Failed to create Windows input capture thread");
            g_client.input_thread_running = false;
        }
    }
#endif
```

## Dependencies

### Required Windows Libraries

Already included via MinGW:
- `user32.dll` - User interface functions (SendInput, SetCursorPos, etc.)
- `ws2_32.dll` - Winsock for networking (used by libwebsockets)
- `libwebsockets` - WebSocket library (cross-platform)
- `OpenSSL` - SSL/TLS support (cross-platform)

### Additional Headers Needed

```c
#include <windows.h>
#include <winuser.h>      // For SendInput, SetCursorPos
#include <wincon.h>       // For console functions (if needed)
```

## Build Requirements

### On Windows (Native Build)

1. **Install MSYS2/MinGW-w64**:
   ```powershell
   # Download from https://www.msys2.org/
   # Then in MSYS2 MinGW64 terminal:
   pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-libwebsockets mingw-w64-x86_64-openssl
   ```

2. **Compile**:
   ```bash
   cd cinput
   gcc -o input_unified.exe input_unified.c \
       -lwebsockets -lssl -lcrypto -lws2_32 -luser32
   ```

### On Linux (Cross-compile)

1. **Install MinGW cross-compiler**:
   ```bash
   sudo apt-get install gcc-mingw-w64-x86-64
   sudo apt-get install libwebsockets-dev openssl-dev  # For headers
   ```

2. **Cross-compile**:
   ```bash
   x86_64-w64-mingw32-gcc -o input_unified.exe input_unified.c \
       -lwebsockets -lssl -lcrypto -lws2_32 -luser32
   ```

**Note**: Cross-compiling requires Windows libraries (.dll) to be available. You may need to copy DLLs or use static linking.

## Testing Checklist

Once implemented, test:

- [ ] **Input Capture (main role)**:
  - [ ] Keyboard keys are captured and sent to server
  - [ ] Mouse movement is captured smoothly (60+ Hz)
  - [ ] Mouse clicks (left, right, middle) are captured
  - [ ] Mouse scroll is captured
  - [ ] Special keys (Ctrl, Alt, Shift, Win) work
  - [ ] Key combinations (Ctrl+C, Alt+Tab, etc.) work

- [ ] **Input Injection (player role)**:
  - [ ] Keyboard keys are injected correctly
  - [ ] Mouse movement is smooth
  - [ ] Mouse clicks work
  - [ ] Key mapping is correct (all keys match)

- [ ] **Monitor Detection**:
  - [ ] All monitors are detected
  - [ ] Monitor positions and sizes are correct
  - [ ] Works with single monitor
  - [ ] Works with multiple monitors in extended mode

- [ ] **Edge Detection**:
  - [ ] Moving mouse to screen edge switches active monitor
  - [ ] Cursor position updates correctly after switch

## Code Locations to Modify

1. **Add Windows input capture thread**:
   - After line ~2105 (after macOS injection functions)
   - Before WebSocket client section (~2180)

2. **Complete keyboard injection**:
   - Lines ~2107-2117: `inject_key_windows()`

3. **Complete monitor detection**:
   - Lines ~1066-1075: `detect_monitors_windows()`

4. **Add thread startup code**:
   - Around line ~3782: Add Windows equivalent of Linux input capture thread startup

5. **Add input grabbing/blocking (optional)**:
   - After input capture thread implementation

## Resources

- **Windows Raw Input API**: https://docs.microsoft.com/en-us/windows/win32/inputdev/raw-input
- **SendInput API**: https://docs.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput
- **Virtual Key Codes**: https://docs.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes
- **EnumDisplayMonitors**: https://docs.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-enumdisplaymonitors

## Priority Order

1. **Input Capture** (highest) - Without this, the "main" role doesn't work at all
2. **Keyboard Injection** - Complete the stub for full keyboard support
3. **Monitor Detection** - Multi-monitor support for proper edge detection
4. **Input Blocking** - Nice to have for cleaner UX

## Estimated Complexity

- **Input Capture**: High complexity (500-800 lines of code)
  - Windows message loop
  - Raw input processing
  - Key code mapping
  - Thread synchronization

- **Keyboard Injection**: Medium complexity (200-300 lines)
  - Key mapping table/function
  - Extended key handling

- **Monitor Detection**: Low complexity (50-100 lines)
  - EnumDisplayMonitors callback
  - Monitor info extraction

- **Total**: ~750-1200 lines of code to add/modify

