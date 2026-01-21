# CRITICAL FIX NEEDED: X11 Mouse Input Capture Not Working

## Problem Summary

The X11 input capture fallback in `input_unified.c` is **barely detecting any mouse movements**. The logs show:
- **7849 polls in ~62 seconds** (120Hz polling working)
- **Only 1 mouse_move event captured** during active mouse movement
- Mouse clicks delayed by seconds or not captured at all

## Current Code Issue

In `x11_input_capture_thread()` (line ~1850), the mouse position detection logic has a problem:

```c
/* Mouse movement */
if ((root_x != prev_x || root_y != prev_y) && 
    (now - last_move_time >= move_throttle_ms)) {
    // ... send mouse_move
    prev_x = root_x;
    prev_y = root_y;
    last_move_time = now;
}
```

**The problem**: This condition may be too strict or the position comparison is failing. The code polls every 8ms but detects almost no movements.

## Specific Fixes Needed

### 1. Improve Position Change Detection

The current code might miss small movements. Consider:
- Logging when XQueryPointer succeeds but position hasn't changed
- Checking if `XQueryPointer` is returning false (failing)
- Removing or reducing the throttle check during active movement
- Adding debug logs to see actual poll results

### 2. Reduce Polling Interval

Current: 8ms = 120Hz
Try: 4ms = 250Hz for better responsiveness

### 3. Add Debug Logging

Add logs to understand why movements aren't being detected:
```c
// Log every 100 polls to see if XQueryPointer is working
if (poll_count % 100 == 0) {
    LOG_DEBUG("Poll #%d: mouse at (%d,%d), prev=(%d,%d), changed=%d", 
              poll_count, root_x, root_y, prev_x, prev_y, 
              (root_x != prev_x || root_y != prev_y));
}
```

### 4. Check XQueryPointer Return Value

The code uses `XQueryPointer()` but doesn't check if it's failing:
```c
if (XQueryPointer(display, root, &root_ret, &child_ret, 
                  &root_x, &root_y, &win_x, &win_y, &mask)) {
    // ... position detection
} else {
    LOG_WARN("XQueryPointer failed at poll %d", poll_count);
}
```

### 5. Consider Removing Throttle During Active Movement

The throttle check `(now - last_move_time >= move_throttle_ms)` might be preventing detection. Try:
- Remove throttle entirely for testing
- Or reduce throttle to 0ms during active movement
- Or use a different throttling strategy

## Expected Behavior (Python Version Reference)

The Python version uses `pynput.mouse.Listener` which:
- Calls `on_mouse_move(x, y)` callback on EVERY mouse movement
- No polling - pure event-based
- Immediately detects any position change
- Works reliably

We need the C version to achieve similar reliability.

## Testing After Fix

1. Run: `./start_client.sh --local-server` on minipc
2. Run: `./start_client.sh --connect <code>` on goldenthinker  
3. Move mouse continuously on minipc
4. **Expected**: Log should show mouse_move events at 60-120+ per second
5. **Current**: Log shows 1 mouse_move every 30+ seconds ❌

## Success Metrics

- ✅ Mouse movements logged at 60+ events/second during active movement
- ✅ Edge crossing detected within < 1 second of reaching screen edge
- ✅ Mouse clicks captured immediately (< 100ms delay)
- ✅ Logs show continuous stream of mouse_move events, not sparse ones

## Files to Modify

**`cinput/input_unified.c`**:
- Function: `x11_input_capture_thread()` starting around line 1850
- Focus on improving the position change detection logic
- Add comprehensive debug logging to diagnose the issue

## Code Location

```c
// Around line 1873-1901 in input_unified.c
while (client->input_thread_running && client->running) {
    poll_count++;
    
    /* Poll mouse position using XQueryPointer */
    if (XQueryPointer(display, root, &root_ret, &child_ret, 
                      &root_x, &root_y, &win_x, &win_y, &mask)) {
        // ... THIS IS WHERE THE PROBLEM IS
        // Position change detection not working reliably
    }
    
    usleep(8000);  // 8ms = 120Hz
}
```

Fix this code to reliably detect mouse movements!

