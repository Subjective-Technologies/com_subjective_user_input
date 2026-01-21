# Windows Porting - Quick Summary

## What Needs to be Done

The `cinput` directory has **partial Windows support**. Here's what's missing:

### ❌ **Input Capture (CRITICAL - Not Implemented)**

**Problem**: When running as `--role main`, the program cannot capture keyboard and mouse input on Windows.

**Solution Needed**: 
- Implement Windows Raw Input API to capture keyboard/mouse events
- Create a message loop thread to process `WM_INPUT` messages
- Send captured events to the server via WebSocket

**Code Location**: Currently no Windows input capture exists. Need to add around line ~2105.

---

### 🔧 **Keyboard Injection (Partial - Needs Key Mapping)**

**Problem**: `inject_key_windows()` function exists but doesn't map key names to Windows virtual key codes.

**Solution Needed**:
- Map key names (like "a", "Enter", "Ctrl") to `VK_*` codes
- Handle special/extended keys properly

**Code Location**: Lines ~2107-2117 in `input_unified.c`

---

### 🔧 **Monitor Detection (Partial - Only Primary Monitor)**

**Problem**: Only detects the primary monitor. Multi-monitor setups won't work correctly.

**Solution Needed**:
- Use `EnumDisplayMonitors()` to detect all monitors
- Get position and size for each monitor

**Code Location**: Lines ~1066-1075 in `input_unified.c`

---

## Current Status Table

| Feature | Status | Priority | Estimated Work |
|---------|--------|----------|----------------|
| **Input Capture** | ❌ Missing | 🔴 Critical | 500-800 lines |
| **Keyboard Injection** | 🔧 Stub | 🟡 High | 200-300 lines |
| **Mouse Injection** | ✅ Working | - | - |
| **Monitor Detection** | 🔧 Partial | 🟢 Medium | 50-100 lines |
| **WebSocket/SSL** | ✅ Working | - | - |

---

## Quick Build Instructions

### Option 1: Native Windows Build (MSYS2)

1. **Install MSYS2**: https://www.msys2.org/

2. **Install dependencies** (in MSYS2 MinGW64 terminal):
   ```bash
   pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-libwebsockets mingw-w64-x86_64-openssl
   ```

3. **Compile**:
   ```bash
   cd cinput
   gcc -Wall -Wextra -O2 -g -DWIN32 -o input_unified.exe input_unified.c \
       -lwebsockets -lssl -lcrypto -lws2_32 -luser32
   ```

### Option 2: Cross-compile from Linux/WSL

```bash
# Install cross-compiler
sudo apt-get install gcc-mingw-w64-x86-64

# Compile
cd cinput
x86_64-w64-mingw32-gcc -Wall -Wextra -O2 -g -DWIN32 -o input_unified.exe input_unified.c \
    -lwebsockets -lssl -lcrypto -lws2_32 -luser32
```

**Note**: You'll need the Windows DLLs (libwebsockets, openssl) at runtime.

---

## What Works Now

✅ **Mouse movement injection** - `SetCursorPos()` works  
✅ **Mouse button injection** - `SendInput()` with mouse events works  
✅ **WebSocket communication** - libwebsockets works on Windows  
✅ **SSL/TLS** - OpenSSL works on Windows  
✅ **Basic monitor detection** - Primary monitor only  

---

## What Doesn't Work

❌ **Input capture** - Cannot capture keyboard/mouse when `--role main`  
❌ **Keyboard injection** - Keys don't map correctly (stub function)  
❌ **Multi-monitor detection** - Only detects primary monitor  
❌ **Input grabbing** - No way to block local input when inactive  

---

## Implementation Priority

1. **First**: Implement Input Capture (enables "main" role)
2. **Second**: Complete Keyboard Injection (enables full keyboard support)
3. **Third**: Multi-monitor Detection (enables proper edge detection)

---

## Files to Modify

- **`input_unified.c`**:
  - Add Windows input capture thread (~line 2105)
  - Complete `inject_key_windows()` (~line 2107)
  - Complete `detect_monitors_windows()` (~line 1066)
  - Add thread startup for Windows (~line 3782)

---

## Next Steps

1. Read the detailed guide: `WINDOWS_PORTING_GUIDE.md`
2. Start with Input Capture implementation
3. Test with single monitor first
4. Add multi-monitor support
5. Complete keyboard injection

---

## Testing

Once implemented, test:

```bash
# Main computer (captures input)
./input_unified.exe --role main --server ws://localhost:8765

# Player computer (receives input)
./input_unified.exe --role player --server ws://localhost:8765
```

**Expected behavior**:
- Main computer: Keyboard/mouse events are captured and sent to server
- Player computer: Input events received from server are injected locally
- Edge detection: Moving mouse to screen edge switches active monitor

