# Fix X11 Input Capture - Mouse Movement Detection Issue

## Problem Analysis

The C version of `input_unified.c` has an X11-based input capture fallback mode that is **not reliably detecting mouse movements**. 

### Current Behavior (from logs):

1. **Edge detection works**: Server successfully detects edge crossing and switches active monitor
   - Log: `"Edge triggered: minipc:m0 [right] -> goldenthinker:m0 [left] at (2554.0, 792.0)"`
   - Active monitor correctly switches from minipc to goldenthinker

2. **Mouse movement capture is broken**: 
   - X11 polling runs at ~120Hz (7849 polls in ~62 seconds)
   - **Only 1 mouse_move event captured in 30+ seconds** when user was actively moving mouse
   - Mouse clicks are also not being captured reliably

3. **Log evidence**:
   ```
   MINIPC LOG:
   - Initial mouse position: (1287, 1275)
   - X11 input capture thread started (XQueryPointer polling)
   - [30 seconds later] Only 1 mouse_move event logged after monitor switch
   - X11 input capture thread stopped (polls: 7849) - but captured almost nothing
   ```

### Root Cause

The X11 input capture uses `XQueryPointer` polling which:
- Polls at 120Hz (every 8ms)
- Checks if `root_x != prev_x || root_y != prev_y`
- But the position comparison might be failing due to:
  1. Position not changing between polls (within 8ms)
  2. Thread timing issues
  3. XQueryPointer failing silently
  4. Position changes being too small to detect

### Comparison with Python Version

The Python version (`input_unified.py`) works because it:
- Uses `pynput` library with **event callbacks** (not polling)
- Calls `on_mouse_move(x, y)` on EVERY mouse movement (real-time)
- Does **client-side edge detection** in `_check_edge_crossing()`
- Sends `edge_crossing_request` to server when edge is detected
- Much more reliable and responsive

## Required Fix

Fix the X11 input capture in `input_unified.c` to reliably detect mouse movements. Options:

### Option 1: Improve XQueryPointer Polling (Recommended)
- Reduce polling interval (maybe 4ms for 250Hz?)
- Add better position change detection
- Add debug logging to see why moves aren't being detected
- Check if XQueryPointer is failing

### Option 2: Use XRecord Extension (Better but more complex)
- Use XRecord extension for event-based input capture (similar to pynput)
- Requires X11 XRecord extension support
- More reliable but more code

### Option 3: Implement Client-Side Edge Detection (Like Python)
- Capture mouse movements more aggressively
- Detect edges on client side before sending to server
- Send `edge_crossing_request` message (like Python version)
- This reduces server load and improves responsiveness

## Files to Modify

1. **`cinput/input_unified.c`**:
   - Function: `x11_input_capture_thread()` (line ~1850)
   - Improve mouse position change detection
   - Add better error handling
   - Increase polling rate or use event-based capture

2. **Message handling** (if implementing Option 3):
   - Add `edge_crossing_request` message type
   - Implement client-side edge detection logic

## Testing

After fix, test:
1. Move mouse on minipc - should see frequent `mouse_move` events in logs
2. Move mouse to right edge - should trigger monitor switch quickly (< 1 second)
3. Click mouse - should be captured immediately
4. Move mouse on goldenthinker when active - should work smoothly

## Success Criteria

- Mouse movements captured at least 60+ times per second during active mouse movement
- Edge detection triggers within < 1 second of reaching screen edge
- Mouse clicks captured immediately
- Logs show continuous `mouse_move` events during mouse activity (not just 1 every 30 seconds)

