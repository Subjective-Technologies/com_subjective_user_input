#!/usr/bin/env python3
"""
WebSocket Input Server - Multi-Monitor KVM System
Receives user input streaming (keyboard, mouse) with precise timing information.
Supports multiple computers with multiple monitors and seamless edge-crossing.
"""

import asyncio
import json
import logging
import os
import sys
import time
from typing import Dict, Set, Optional, Tuple, List
from datetime import datetime
from dataclasses import dataclass
import websockets
from dotenv import load_dotenv

# Ensure local server modules are importable when run from project root
SERVER_DIR = os.path.dirname(__file__)
PROJECT_ROOT = os.path.abspath(os.path.join(SERVER_DIR, os.pardir))
sys.path.insert(0, SERVER_DIR)
CONFIG_PATH = os.path.join(PROJECT_ROOT, "config", "devices.conf")
# Note: Configuration is now in devices.conf (not .env)

# Load environment variables from .env file
load_dotenv()

# Custom FileHandler that flushes after each write for Dropbox sync
class FlushingFileHandler(logging.FileHandler):
    """FileHandler that flushes after each emit to ensure Dropbox syncs immediately."""
    def emit(self, record):
        super().emit(record)
        self.flush()

# Filter to suppress harmless InvalidUpgrade errors from websockets library
class SuppressInvalidUpgradeFilter(logging.Filter):
    """Filter out InvalidUpgrade errors - these are harmless HTTP requests that aren't WebSocket connections"""
    def filter(self, record):
        # Suppress errors about InvalidUpgrade - these are just HTTP requests (browsers, curl, health checks)
        # that aren't trying to establish WebSocket connections
        if record.levelno == logging.ERROR:
            msg = str(record.getMessage())
            if 'InvalidUpgrade' in msg or 'invalid Connection header' in msg:
                return False  # Don't log this
        return True  # Log everything else

# Get logs folder path from environment variable, default to 'logs'
logs_folder = os.getenv('LOGS_FOLDER_PATH', 'logs')

# Create logs directory if it doesn't exist
os.makedirs(logs_folder, exist_ok=True)

# Create timestamped log filename: server_YYYYMMDDHHMMSS.log
from datetime import datetime
timestamp = datetime.now().strftime('%Y%m%d%H%M%S')
log_filename = f'{logs_folder}/server_{timestamp}.log'

# Configure logging to both file and console
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s',
    handlers=[
        FlushingFileHandler(log_filename),
        logging.StreamHandler()  # Also log to console
    ]
)
logger = logging.getLogger(__name__)
logger.info(f"Logging to: {log_filename}")

# Suppress harmless InvalidUpgrade errors from websockets library
# These are just HTTP requests (browsers, curl, health checks) that aren't WebSocket connections
websockets_logger = logging.getLogger('websockets.server')
websockets_logger.addFilter(SuppressInvalidUpgradeFilter())

# ============================================================================
# Troubleshooting Flags
# ============================================================================
def _env_flag(name: str, default: str = "0") -> bool:
    """Utility to read boolean-ish environment variables."""
    return str(os.environ.get(name, default)).strip().lower() in ("1", "true", "yes", "on")

# Set to True to enable verbose mouse position logging (for troubleshooting)
ENABLE_MOUSE_POSITION_LOGGING = _env_flag("SERVER_MOUSE_LOGS", "0")
# Set to True to log EVERY input event payload (very noisy)
ENABLE_INPUT_EVENT_LOGGING = _env_flag("SERVER_INPUT_EVENT_LOGS", "0")
MOUSE_POSITION_LOG_INTERVAL_MS = 200  # Log mouse position every 200ms max (to reduce spam)

# ============================================================================
# Multi-Monitor KVM Data Structures
# ============================================================================

@dataclass
class Monitor:
    """Represents a monitor with its geometry and owning computer"""
    monitor_id: str        # Unique within the computer (e.g., "m0", "m1")
    computer_id: str       # Owning computer
    x: int                 # X position in computer's local coordinate space
    y: int                 # Y position in computer's local coordinate space
    width: int             # Monitor width in pixels
    height: int            # Monitor height in pixels

    def contains_point(self, x: float, y: float) -> bool:
        """Check if a point is within this monitor's bounds"""
        return (self.x <= x < self.x + self.width and
                self.y <= y < self.y + self.height)

    def get_edge_position(self, edge: str, x: float, y: float, threshold: int = None) -> Optional[float]:
        """
        Get the relative position [0.0-1.0] along an edge where a point crosses.
        Returns None if point is not at the edge.

        NOTE: This method is kept for backward compatibility.
        Edge detection is now primarily handled client-side.
        """
        if threshold is None:
            threshold = EDGE_THRESHOLD  # Use global config value

        if edge == "left" and x <= self.x + threshold:
            return (y - self.y) / self.height if self.height > 0 else 0.5
        elif edge == "right" and x >= self.x + self.width - threshold:
            return (y - self.y) / self.height if self.height > 0 else 0.5
        elif edge == "top" and y <= self.y + threshold:
            return (x - self.x) / self.width if self.width > 0 else 0.5
        elif edge == "bottom" and y >= self.y + self.height - threshold:
            return (x - self.x) / self.width if self.width > 0 else 0.5

        return None


@dataclass
class Computer:
    """Represents a computer with its monitors and role"""
    computer_id: str
    role: str              # "client" or "player" (or both)
    monitors: List[Monitor]
    websocket: Optional[websockets.WebSocketServerProtocol] = None
    registered_at: str = ""

    def get_monitor(self, monitor_id: str) -> Optional[Monitor]:
        """Get a monitor by ID"""
        for mon in self.monitors:
            if mon.monitor_id == monitor_id:
                return mon
        return None


# Monitor link configuration - loaded from devices.conf
# Format: (computer_id, monitor_id, edge) -> (target_computer_id, target_monitor_id, target_edge)
# This defines which edges connect to which other edges
#
# Load from configuration file
try:
    from config_loader import load_config
    config = load_config(CONFIG_PATH)
    MONITOR_LINKS = config.get_monitor_links_dict()
    EDGE_THRESHOLD = config.edge_threshold
    EDGE_OFFSET = config.edge_offset
    logger.info(f"Loaded configuration: {len(MONITOR_LINKS)} monitor links")
except Exception as e:
    logger.warning(f"Could not load config file: {e}, using hardcoded defaults")
    # Fallback to hardcoded values
    MONITOR_LINKS = {
        ("minipc", "m0", "right"): ("goldenthinker", "m0", "left"),
        ("goldenthinker", "m0", "left"): ("minipc", "m0", "right"),
    }
    EDGE_THRESHOLD = 10
    EDGE_OFFSET = 20


class InputEvent:
    """Represents a single input event with timing information"""

    def __init__(self, event_type: str, data: dict, device_id: str, delta_ms: float):
        self.event_type = event_type  # keyboard, mouse_move, mouse_click, mouse_scroll
        self.data = data
        self.device_id = device_id
        self.delta_ms = delta_ms  # milliseconds since last input
        self.timestamp = datetime.now().isoformat()

    def to_dict(self):
        return {
            'event_type': self.event_type,
            'data': self.data,
            'device_id': self.device_id,
            'delta_ms': self.delta_ms,
            'timestamp': self.timestamp
        }


class InputServer:
    """WebSocket server for receiving input streams with dynamic layout management"""

    def __init__(self, host: str = "0.0.0.0", port: int = 8765, ssl_cert_path: str = None, ssl_key_path: str = None):
        from layout_manager import LayoutManager

        self.host = host
        self.port = port
        self.ssl_cert_path = ssl_cert_path
        self.ssl_key_path = ssl_key_path
        self.clients: Set = set()
        self.shutting_down: bool = False  # Flag to prevent sends during shutdown

        # Dynamic Layout Manager (NEW - replaces static configuration)
        self.layout_manager = LayoutManager()

        # Multi-monitor KVM state
        self.computers: Dict[str, Computer] = {}  # computer_id -> Computer (kept for compatibility)
        self.active_monitor: Tuple[str, str] = ("", "m0")  # (computer_id, monitor_id) - set dynamically
        self.cursor_position: Tuple[float, float] = (0, 0)  # Current cursor position

        # Edge crossing debounce - prevent rapid back-and-forth switching
        self.last_edge_crossing_time: float = 0
        self.last_crossed_from: Tuple[str, str] = ("", "")  # (computer_id, monitor_id) we just left
        self.edge_crossing_debounce_ms: float = 500  # Wait 500ms before allowing REVERSE crossing

        # Post-crossing protection - which edge we crossed TO (to clamp movements)
        self.last_crossed_to_edge: str = ""  # "left", "right", "top", "bottom"
        self.post_crossing_clamp_ms: float = 150  # Clamp movements toward crossed edge for 150ms (reduced from 1000ms to reduce lag feeling)

        # Edge trigger debounce - prevent spam logging of edge triggers (per edge)
        self.last_edge_trigger_time: Dict[str, float] = {}  # edge_key -> timestamp
        self.edge_trigger_debounce_ms: float = 500  # Only log edge trigger every 500ms per edge

        # Legacy registries (kept for backward compatibility during transition)
        self.device_registry: Dict[str, dict] = {}  # Input devices (sending input)
        self.player_registry: Dict[str, dict] = {}  # Player devices (receiving input)
        self.player_websockets: Dict[str, websockets.WebSocketServerProtocol] = {}  # player_id -> websocket

    async def register_device(self, device_id: str, metadata: dict = None):
        """Register a new input device"""
        self.device_registry[device_id] = {
            'id': device_id,
            'metadata': metadata or {},
            'registered_at': datetime.now().isoformat(),
            'last_input': None,
            'last_mouse_position': None
        }
        logger.info(f"Input device registered: {device_id}")

    async def register_player(self, player_id: str, websocket, metadata: dict = None):
        """Register a new player device"""
        self.player_registry[player_id] = {
            'id': player_id,
            'metadata': metadata or {},
            'registered_at': datetime.now().isoformat(),
            'last_output': None
        }
        self.player_websockets[player_id] = websocket
        logger.info(f"Player device registered: {player_id}")

    async def unregister_player(self, player_id: str):
        """Unregister a player device"""
        if player_id in self.player_registry:
            del self.player_registry[player_id]
        if player_id in self.player_websockets:
            del self.player_websockets[player_id]
        logger.info(f"Player device unregistered: {player_id}")

    # ========================================================================
    # Multi-Monitor Registration & Management
    # ========================================================================

    async def register_computer(self, computer_id: str, is_main: bool, monitors_data: List[dict],
                                 websocket) -> dict:
        """
        Register a computer with dynamic layout management.

        Args:
            computer_id: Unique ID for this computer
            is_main: True if this computer has physical keyboard/mouse
            monitors_data: List of monitor specs from client
            websocket: WebSocket connection

        Returns:
            Dictionary with layout information
        """
        from layout_manager import MonitorInfo

        # Convert monitor data to MonitorInfo objects
        monitors = []
        for mon_data in monitors_data:
            monitor = MonitorInfo(
                monitor_id=mon_data.get('monitor_id', 'm0'),
                width=mon_data.get('width', 1920),
                height=mon_data.get('height', 1080)
                # x, y will be set by LayoutManager
            )
            monitors.append(monitor)

        # Register with LayoutManager - it will auto-arrange and create edge links
        layout_info = self.layout_manager.register_computer(
            computer_id=computer_id,
            is_main=is_main,
            monitors=monitors,
            websocket=websocket
        )

        # Update edge links dictionary for fast lookup during edge crossing
        global MONITOR_LINKS
        MONITOR_LINKS = self.layout_manager.get_edge_links_dict()

        # Convert LayoutManager data to old Computer format for backward compatibility
        monitors_old = []
        for mon in monitors:
            monitors_old.append(Monitor(
                monitor_id=mon.monitor_id,
                computer_id=computer_id,
                x=mon.x,
                y=mon.y,
                width=mon.width,
                height=mon.height
            ))

        computer = Computer(
            computer_id=computer_id,
            role="main" if is_main else "player",
            monitors=monitors_old,
            websocket=websocket,
            registered_at=datetime.now().isoformat()
        )
        self.computers[computer_id] = computer

        # Ensure the device registry tracks mouse position for delta calculations
        await self.register_device(computer_id, {'role': computer.role})
        self.device_registry[computer_id]['last_mouse_position'] = None

        # Set active monitor to main computer
        if is_main:
            self.active_monitor = (computer_id, monitors[0].monitor_id)
            self.cursor_position = (
                monitors[0].x + monitors[0].width / 2,
                monitors[0].y + monitors[0].height / 2
            )
            logger.info("=" * 70)
            logger.info("🔒 INITIAL LOCK STATE")
            logger.info(f"  Main computer {computer_id}:{monitors[0].monitor_id} is LOCKED (ACTIVE)")
            logger.info(f"  All other computers will start as UNLOCKED (INACTIVE)")
            logger.info("=" * 70)

        # Print ASCII visualization to console
        logger.info(f"\n{layout_info['visualization']}")

        return layout_info

    async def unregister_computer(self, computer_id: str):
        """Unregister a computer and update layout"""
        if computer_id in self.computers:
            del self.computers[computer_id]

        # Unregister from layout manager
        layout_info = self.layout_manager.unregister_computer(computer_id)

        # Update edge links
        global MONITOR_LINKS
        MONITOR_LINKS = self.layout_manager.get_edge_links_dict()

        # Broadcast layout update to remaining clients (skip during shutdown)
        if not self.shutting_down:
            await self.broadcast_layout_update()

        logger.info(f"Computer unregistered: {computer_id}")

    async def broadcast_layout_update(self):
        """Broadcast current layout to all connected clients"""
        # Don't broadcast during shutdown
        if self.shutting_down:
            return

        layout_info = self.layout_manager.get_layout_info()
        message = json.dumps({
            'type': 'layout_update',
            'layout': layout_info
        })

        # Create a copy of the values to avoid RuntimeError during iteration
        for computer in list(self.computers.values()):
            if computer.websocket and not self.shutting_down:
                try:
                    await computer.websocket.send(message)
                except websockets.exceptions.ConnectionClosed:
                    logger.debug(f"Connection already closed for {computer.computer_id}")
                except Exception as e:
                    logger.error(f"Error broadcasting layout to {computer.computer_id}: {e}")

    def get_active_monitor_obj(self) -> Optional[Monitor]:
        """Get the currently active monitor object"""
        computer_id, monitor_id = self.active_monitor
        computer = self.computers.get(computer_id)
        if computer:
            return computer.get_monitor(monitor_id)
        return None

    def check_edge_crossing(self, x: float, y: float) -> Optional[Tuple[str, str, float, float]]:
        """
        Check if cursor position triggers an edge crossing to another monitor.
        Only processes edges that have valid links configured.
        Returns: (new_computer_id, new_monitor_id, new_x, new_y) or None
        """
        active_mon = self.get_active_monitor_obj()
        if not active_mon:
            logger.debug(f"Edge check: No active monitor found")
            return None

        # Check each edge for potential crossing - but only if there's a link for it
        for edge in ["left", "right", "top", "bottom"]:
            # First check if this edge has a link configured - skip if not
            link_key = (active_mon.computer_id, active_mon.monitor_id, edge)
            if link_key not in MONITOR_LINKS:
                continue  # No link for this edge, don't bother checking position

            edge_pos = active_mon.get_edge_position(edge, x, y)
            if edge_pos is not None:
                # Edge position detected and link exists - this is a valid crossing candidate
                target_computer_id, target_monitor_id, target_edge = MONITOR_LINKS[link_key]

                # Debounce edge trigger logging to reduce spam
                current_time = time.time() * 1000
                edge_key = f"{active_mon.computer_id}:{active_mon.monitor_id}:{edge}"
                last_trigger = self.last_edge_trigger_time.get(edge_key, 0)
                should_log = (current_time - last_trigger) >= self.edge_trigger_debounce_ms

                if should_log:
                    self.last_edge_trigger_time[edge_key] = current_time
                    logger.info(
                        f"Edge triggered: {active_mon.computer_id}:{active_mon.monitor_id} [{edge}] "
                        f"-> {target_computer_id}:{target_monitor_id} [{target_edge}] "
                        f"at ({x:.1f}, {y:.1f})"
                    )

                # Get target monitor
                target_computer = self.computers.get(target_computer_id)
                if not target_computer:
                    logger.warning(f"Target computer {target_computer_id} not found")
                    continue

                target_mon = target_computer.get_monitor(target_monitor_id)
                if not target_mon:
                    logger.warning(f"Target monitor {target_monitor_id} not found on {target_computer_id}")
                    continue

                # Calculate new cursor position on target monitor
                new_x, new_y = self.map_cursor_to_edge(
                    edge_pos, target_mon, target_edge
                )

                logger.info(
                    f"✓ Edge crossing ready: {active_mon.computer_id}:{active_mon.monitor_id} ({edge}) "
                    f"-> {target_computer_id}:{target_monitor_id} ({target_edge}) | "
                    f"Cursor: ({x:.1f}, {y:.1f}) -> ({new_x:.1f}, {new_y:.1f})"
                )

                return (target_computer_id, target_monitor_id, new_x, new_y)

        return None

    def map_cursor_to_edge(self, relative_pos: float, target_mon: Monitor, target_edge: str, edge_offset: int = None) -> Tuple[float, float]:
        """
        Map a relative position [0.0-1.0] along an edge to absolute coordinates on target monitor.
        Places cursor slightly inside the target monitor (not exactly at edge) to avoid immediate re-triggering.
        """
        # Clamp position to valid range
        relative_pos = max(0.0, min(1.0, relative_pos))

        # Offset from edge to place cursor inside monitor (prevents immediate re-trigger)
        if edge_offset is None:
            edge_offset = EDGE_OFFSET  # Use global config value

        if target_edge == "left":
            new_x = target_mon.x + edge_offset
            new_y = target_mon.y + relative_pos * target_mon.height
        elif target_edge == "right":
            new_x = target_mon.x + target_mon.width - edge_offset
            new_y = target_mon.y + relative_pos * target_mon.height
        elif target_edge == "top":
            new_x = target_mon.x + relative_pos * target_mon.width
            new_y = target_mon.y + edge_offset
        elif target_edge == "bottom":
            new_x = target_mon.x + relative_pos * target_mon.width
            new_y = target_mon.y + target_mon.height - edge_offset
        else:
            # Fallback to center
            new_x = target_mon.x + target_mon.width / 2
            new_y = target_mon.y + target_mon.height / 2

        return (new_x, new_y)

    async def set_active_monitor(self, computer_id: str, monitor_id: str, cursor_x: float, cursor_y: float):
        """
        Set the active monitor and notify all clients/players.
        This is called when edge-crossing occurs.
        """
        # Don't send during shutdown
        if self.shutting_down:
            return

        old_active = self.active_monitor
        self.active_monitor = (computer_id, monitor_id)
        self.cursor_position = (cursor_x, cursor_y)

        # Enhanced logging with lock state transitions
        logger.info("=" * 70)
        logger.info("🔒 LOCK STATE TRANSITION")
        logger.info(f"  UNLOCKING: {old_active[0]}:{old_active[1]} (now INACTIVE)")
        logger.info(f"  LOCKING:   {computer_id}:{monitor_id} (now ACTIVE)")
        logger.info(f"  Cursor position: ({cursor_x:.1f}, {cursor_y:.1f})")
        if ENABLE_MOUSE_POSITION_LOGGING:
            # Get target monitor to show local coordinates
            target_computer = self.computers.get(computer_id)
            if target_computer:
                target_mon = target_computer.get_monitor(monitor_id)
                if target_mon:
                    local_x = cursor_x - target_mon.x
                    local_y = cursor_y - target_mon.y
                    logger.info(f"  Local coords on {computer_id}:{monitor_id}: ({local_x:.1f}, {local_y:.1f})")
        logger.info("=" * 70)

        # Calculate LOCAL coordinates for the newly active monitor
        # The client expects local coordinates (relative to its monitor origin)
        local_cursor_x = cursor_x
        local_cursor_y = cursor_y
        target_computer = self.computers.get(computer_id)
        if target_computer:
            target_mon = target_computer.get_monitor(monitor_id)
            if target_mon:
                local_cursor_x = cursor_x - target_mon.x
                local_cursor_y = cursor_y - target_mon.y
                logger.info(f"  → Sending local cursor: ({local_cursor_x:.1f}, {local_cursor_y:.1f})")

        # Broadcast active monitor change to all computers
        message = json.dumps({
            'type': 'active_monitor_changed',
            'computer_id': computer_id,
            'monitor_id': monitor_id,
            'cursor_x': local_cursor_x,
            'cursor_y': local_cursor_y
        })

        # Create a copy of the values to avoid RuntimeError during iteration
        for comp in list(self.computers.values()):
            if comp.websocket and not self.shutting_down:
                try:
                    await comp.websocket.send(message)
                    state = "ACTIVE ✓" if comp.computer_id == computer_id else "INACTIVE ✗"
                    logger.info(f"  → Notified {comp.computer_id}: {state}")
                except websockets.exceptions.ConnectionClosed:
                    logger.debug(f"Connection already closed for {comp.computer_id}")
                except Exception as e:
                    logger.error(f"Error sending active monitor change to {comp.computer_id}: {e}")

    async def handle_edge_crossing_request(
        self,
        computer_id: str,
        monitor_id: str,
        edge: str,
        position: float,
        cursor_x: float,
        cursor_y: float
    ):
        """
        Handle edge crossing request from client.
        Client has detected edge crossing locally and is requesting a switch.
        """
        logger.info(f"🔄 Edge crossing request: {computer_id}:{monitor_id} [{edge}] at ({cursor_x:.1f}, {cursor_y:.1f})")

        # Check if this is a REVERSE crossing (going back to where we just came from)
        is_reverse_crossing = (
            (computer_id, monitor_id) == self.last_crossed_from
        )

        if is_reverse_crossing:
            # This is a reverse crossing - apply debounce
            current_time = time.time() * 1000  # Convert to milliseconds
            time_since_last_crossing = current_time - self.last_edge_crossing_time

            if time_since_last_crossing < self.edge_crossing_debounce_ms:
                logger.warning(
                    f"⛔ BLOCKED reverse edge crossing from {computer_id}:{monitor_id} "
                    f"(debounce: {time_since_last_crossing:.0f}ms < {self.edge_crossing_debounce_ms}ms)"
                )
                return  # Block this crossing
            else:
                logger.info(f"✓ Reverse crossing ALLOWED (waited {time_since_last_crossing:.0f}ms)")

        # Look up target monitor using edge links
        link_key = (computer_id, monitor_id, edge)
        if link_key not in MONITOR_LINKS:
            logger.warning(f"❌ No edge link found for {computer_id}:{monitor_id} edge={edge}")
            return

        target_computer_id, target_monitor_id, target_edge = MONITOR_LINKS[link_key]
        logger.info(f"  Target: {target_computer_id}:{target_monitor_id} [{target_edge}]")

        # Get target monitor to calculate new cursor position
        target_mon = None
        if target_computer_id in self.computers:
            for mon in self.computers[target_computer_id].monitors:
                if mon.monitor_id == target_monitor_id:
                    target_mon = mon
                    break

        if not target_mon:
            logger.warning(f"❌ Target monitor not found: {target_computer_id}:{target_monitor_id}")
            return

        # Calculate new cursor position on target monitor
        # Use EDGE_OFFSET to place cursor slightly inside the edge
        new_x, new_y = self._calculate_cursor_position_on_target(
            target_mon, target_edge, position
        )

        logger.info(f"  New cursor position: ({new_x:.1f}, {new_y:.1f})")

        # Update debounce tracking
        self.last_crossed_from = (computer_id, monitor_id)
        self.last_edge_crossing_time = time.time() * 1000

        # Switch active monitor (this will trigger the lock state transition)
        await self.set_active_monitor(target_computer_id, target_monitor_id, new_x, new_y)

    def _calculate_cursor_position_on_target(
        self,
        target_mon,
        target_edge: str,
        position: float
    ) -> tuple:
        """
        Calculate where the cursor should appear on the target monitor.
        position is 0.0-1.0 representing where along the edge the crossing occurred.
        """
        offset = EDGE_OFFSET  # Pixels inside the edge (usually 20)

        if target_edge == "left":
            new_x = target_mon.x + offset
            new_y = target_mon.y + position * target_mon.height
        elif target_edge == "right":
            new_x = target_mon.x + target_mon.width - offset
            new_y = target_mon.y + position * target_mon.height
        elif target_edge == "top":
            new_x = target_mon.x + position * target_mon.width
            new_y = target_mon.y + offset
        elif target_edge == "bottom":
            new_x = target_mon.x + position * target_mon.width
            new_y = target_mon.y + target_mon.height - offset
        else:
            # Fallback to center
            new_x = target_mon.x + target_mon.width / 2
            new_y = target_mon.y + target_mon.height / 2

        return (new_x, new_y)

    async def process_input_event(self, event: InputEvent):
        """
        Process a received input event with server-side edge detection.
        This is the SINGLE SOURCE OF TRUTH for state transitions.
        """
        # Update device last input time
        if event.device_id in self.device_registry:
            self.device_registry[event.device_id]['last_input'] = event.timestamp

        # Log the event (but reduce verbosity for high-frequency mouse moves)
        if event.event_type == 'mouse_move':
            logger.debug(
                f"Input received - Device: {event.device_id}, "
                f"Type: {event.event_type}, "
                f"Delta: {event.delta_ms}ms"
            )
        elif event.event_type == 'cursor_reset':
            # Client warped cursor to a new position - reset delta tracking
            x = event.data.get('x', 0)
            y = event.data.get('y', 0)
            if event.device_id in self.device_registry:
                device_info = self.device_registry[event.device_id]
                device_info['last_mouse_position'] = (x, y)
                
                # Only set skip_mouse_events if we haven't reset recently (within 500ms)
                # This prevents rapid re-centering from blocking too many events
                current_time = time.time() * 1000
                last_reset = device_info.get('cursor_reset_time', 0)
                if current_time - last_reset > 500:
                    # First reset in a while - skip minimal events for better responsiveness
                    device_info['skip_mouse_events'] = 1  # Reduced from 3 to improve mouse speed
                    logger.info(f"🔄 Cursor reset for {event.device_id}: new position ({x}, {y}), skipping 1 event")
                else:
                    # Recent reset - just update position, don't skip more events
                    logger.debug(f"🔄 Cursor reset for {event.device_id}: new position ({x}, {y}) (no skip, recent reset)")
                device_info['cursor_reset_time'] = current_time
            return  # Don't route this event, it's just for state sync
        
        elif event.event_type == 'clipboard_update':
            # Broadcast clipboard update to ALL clients (not just active)
            content = event.data.get('content', '')
            source = event.data.get('source', event.device_id)
            
            logger.info(f"📋 Clipboard update from {source} ({len(content)} chars)")
            
            # Broadcast to all connected computers
            message = json.dumps({
                'type': 'clipboard_update',
                'data': {
                    'content': content,
                    'source': source
                }
            })
            
            for computer in list(self.computers.values()):
                if computer.websocket and not self.shutting_down:
                    try:
                        await computer.websocket.send(message)
                    except Exception as e:
                        logger.debug(f"Failed to send clipboard to {computer.computer_id}: {e}")
            return  # Don't continue processing this as a regular input event
        
        elif ENABLE_INPUT_EVENT_LOGGING:
            logger.info(
                f"Input received - Device: {event.device_id}, "
                f"Type: {event.event_type}, "
                f"Delta: {event.delta_ms}ms, "
                f"Data: {event.data}"
            )

        # Handle mouse movement with SERVER-SIDE edge detection
        if event.event_type == 'mouse_move':
            x = event.data.get('x', 0)
            y = event.data.get('y', 0)

            active_computer_id, active_monitor_id = self.active_monitor
            active_computer = self.computers.get(active_computer_id)
            active_mon = active_computer.get_monitor(active_monitor_id) if active_computer else None
            sender_is_active = (event.device_id == active_computer_id)
            
            # Log mouse position for troubleshooting (throttled)
            if ENABLE_MOUSE_POSITION_LOGGING:
                current_time = time.time() * 1000
                if not hasattr(self, '_last_mouse_log_time'):
                    self._last_mouse_log_time = 0
                if current_time - self._last_mouse_log_time >= MOUSE_POSITION_LOG_INTERVAL_MS:
                    logger.info(
                        f"🖱️  Mouse: sender={event.device_id} local=({x}, {y}) | "
                        f"active={active_computer_id}:{active_monitor_id} | "
                        f"global=({self.cursor_position[0]:.1f}, {self.cursor_position[1]:.1f})"
                    )
                    self._last_mouse_log_time = current_time

            # Track last mouse position per device so we can derive deltas when routing
            device_info = self.device_registry.get(event.device_id)
            if not device_info:
                # Initialize on-the-fly if the device was not explicitly registered
                self.device_registry[event.device_id] = {
                    'id': event.device_id,
                    'metadata': {},
                    'registered_at': datetime.now().isoformat(),
                    'last_input': event.timestamp,
                    'last_mouse_position': None
                }
                device_info = self.device_registry[event.device_id]

            # Check if we should skip this event due to recent cursor_reset
            skip_count = device_info.get('skip_mouse_events', 0)
            if skip_count > 0:
                device_info['skip_mouse_events'] = skip_count - 1
                device_info['last_mouse_position'] = (x, y)  # Still update position
                logger.debug(f"Skipping mouse event from {event.device_id} (post-reset, {skip_count-1} remaining)")
                return  # Skip this event entirely
            
            last_mouse_pos = device_info.get('last_mouse_position')
            device_info['last_mouse_position'] = (x, y)
            dx = dy = 0
            if last_mouse_pos is not None:
                dx = x - last_mouse_pos[0]
                dy = y - last_mouse_pos[1]
                
                # Sanity check: reject huge deltas that are likely from cursor warp artifacts
                # Normal mouse movement shouldn't exceed 500 pixels in a single event
                if abs(dx) > 500 or abs(dy) > 500:
                    logger.debug(f"Rejecting huge delta ({dx}, {dy}) from {event.device_id} - likely warp artifact")
                    return

                # POST-CROSSING CLAMP: Prevent movements back towards the edge we just crossed TO
                # This prevents the "stuck at edge" cursor position from causing rapid back-and-forth
                current_time = time.time() * 1000
                time_since_crossing = current_time - self.last_edge_crossing_time
                if time_since_crossing < self.post_crossing_clamp_ms and self.last_crossed_to_edge:
                    clamped = False
                    if self.last_crossed_to_edge == "left" and dx < 0:
                        dx = 0  # Don't allow leftward movement right after crossing to left edge
                        clamped = True
                    elif self.last_crossed_to_edge == "right" and dx > 0:
                        dx = 0  # Don't allow rightward movement right after crossing to right edge
                        clamped = True
                    elif self.last_crossed_to_edge == "top" and dy < 0:
                        dy = 0  # Don't allow upward movement right after crossing to top edge
                        clamped = True
                    elif self.last_crossed_to_edge == "bottom" and dy > 0:
                        dy = 0  # Don't allow downward movement right after crossing to bottom edge
                        clamped = True
                    if clamped:
                        logger.debug(f"Clamped movement toward {self.last_crossed_to_edge} edge (post-crossing protection)")

            if sender_is_active:
                # Convert from sender's local coordinate space to global coordinate space
                # The sender (active computer) sends coordinates in its local space (0-width, 0-height)
                # But the server uses global coordinates for edge detection
                sender_computer = self.computers.get(event.device_id)
                if sender_computer and sender_computer.monitors:
                    # Assume first monitor for now (could be extended for multi-monitor)
                    sender_mon = sender_computer.monitors[0]
                    # Convert local to global: add monitor's global offset
                    global_x = x + sender_mon.x
                    global_y = y + sender_mon.y
                else:
                    # Fallback: assume coordinates are already global
                    global_x = x
                    global_y = y

                # Update cursor position (store global coordinates)
                self.cursor_position = (global_x, global_y)

                # Check for edge crossing using GLOBAL coordinates (SERVER-SIDE - SINGLE SOURCE OF TRUTH)
                crossing = self.check_edge_crossing(global_x, global_y)
            else:
                # Sender is NOT the active computer (e.g., main hardware driving a player).
                # Use deltas to move the global cursor within the currently active monitor.
                global_x = self.cursor_position[0] + dx
                global_y = self.cursor_position[1] + dy
                self.cursor_position = (global_x, global_y)

                # Still perform edge detection using the updated global position
                crossing = self.check_edge_crossing(global_x, global_y) if active_mon else None
            if crossing:
                new_computer_id, new_monitor_id, new_x, new_y = crossing
                current_computer_id, current_monitor_id = self.active_monitor

                # Check if this is a REVERSE crossing (going back to where we just came from)
                is_reverse_crossing = (
                    (new_computer_id, new_monitor_id) == self.last_crossed_from
                )

                if is_reverse_crossing:
                    # This is a reverse crossing - apply debounce
                    current_time = time.time() * 1000  # Convert to milliseconds
                    time_since_last_crossing = current_time - self.last_edge_crossing_time

                    if time_since_last_crossing < self.edge_crossing_debounce_ms:
                        logger.debug(
                            f"Reverse crossing blocked by debounce: "
                            f"{current_computer_id}:{current_monitor_id} -> {new_computer_id}:{new_monitor_id} "
                            f"(waited {time_since_last_crossing:.0f}ms / {self.edge_crossing_debounce_ms}ms)"
                        )
                        # Block this crossing
                        crossing = None

                # If crossing is allowed (not blocked), execute it ATOMICALLY
                if crossing:
                    # Determine which edge we're crossing TO on the target monitor
                    link_key = None
                    for edge in ["left", "right", "top", "bottom"]:
                        test_key = (current_computer_id, current_monitor_id, edge)
                        if test_key in MONITOR_LINKS:
                            target = MONITOR_LINKS[test_key]
                            if target[0] == new_computer_id and target[1] == new_monitor_id:
                                # This is the edge we crossed FROM, target_edge is where we crossed TO
                                self.last_crossed_to_edge = target[2]
                                break

                    # Update debounce tracking
                    self.last_edge_crossing_time = time.time() * 1000
                    self.last_crossed_from = (current_computer_id, current_monitor_id)

                    # ATOMIC TRANSITION: Switch active monitor
                    # new_x, new_y are in GLOBAL coordinates from map_cursor_to_edge
                    await self.set_active_monitor(new_computer_id, new_monitor_id, new_x, new_y)

                    # CRITICAL: Reset last_mouse_position for ALL devices after edge crossing
                    # This prevents the "stuck at edge" position from creating bad deltas
                    # that would push the cursor back towards the edge on the new monitor
                    for device_id, device_info in self.device_registry.items():
                        device_info['last_mouse_position'] = None
                    logger.debug(f"Reset last_mouse_position, crossed to {self.last_crossed_to_edge} edge")

                    # Update event data with new coordinates in GLOBAL space
                    # (will be converted to local space in route_to_active_computer)
                    event.data['x'] = new_x
                    event.data['y'] = new_y

                    logger.info(
                        f"Edge crossing executed: {current_computer_id}:{current_monitor_id} -> "
                        f"{new_computer_id}:{new_monitor_id} at global ({new_x:.1f}, {new_y:.1f})"
                    )
                    if ENABLE_MOUSE_POSITION_LOGGING:
                        logger.info(
                            f"   Cursor transition: global ({global_x:.1f}, {global_y:.1f}) → "
                            f"({new_x:.1f}, {new_y:.1f}) | "
                            f"Crossed from {current_computer_id} to {new_computer_id}"
                        )
                    
                    # Skip routing this event - cursor position is already set by edge crossing
                    # This prevents the old position from interfering with the new position
                    return
            else:
                # No edge crossing check when sender is not active
                # Just pass through for routing
                pass

        # Route input to the active monitor's computer only
        await self.route_to_active_computer(event)

    async def route_to_active_computer(self, event: InputEvent):
        """
        Broadcast input event to ALL clients.
        Each client decides whether to execute based on:
        - Is it the active computer? (should execute)
        - Is it the main computer? (should never execute, physical input works natively)
        """
        # Don't route during shutdown
        if self.shutting_down:
            return

        active_computer_id, active_monitor_id = self.active_monitor

        # Convert coordinates from global to each computer's local space
        # The server maintains global coordinates, but each client needs local coordinates
        active_computer = self.computers.get(active_computer_id)
        active_mon = None
        if active_computer:
            active_mon = active_computer.get_monitor(active_monitor_id)

        # Prepare event data with coordinates in the active computer's local space
        # Only copy when we plan to mutate (mouse events); otherwise reuse dict to avoid churn
        if event.event_type in ('mouse_move', 'mouse_click', 'mouse_scroll'):
            event_data = event.data.copy()
        else:
            event_data = event.data
        
        # Convert coordinates for ALL mouse events (move, click, scroll)
        # The server maintains global coordinates, clients need local coordinates
        if event.event_type in ('mouse_move', 'mouse_click', 'mouse_scroll') and active_mon:
            sender_is_active = (event.device_id == active_computer_id)
            
            if sender_is_active:
                # Sender IS the active computer - use coordinates from the event directly
                # These are already in the correct local space for the sender
                # For clicks/scrolls, the event has the actual click position
                local_x = event.data.get('x', 0)
                local_y = event.data.get('y', 0)
                global_x = local_x + active_mon.x
                global_y = local_y + active_mon.y
                # Also update cursor_position to sync with actual click position
                self.cursor_position = (global_x, global_y)
            else:
                # Sender is NOT the active computer - use server's tracked position
                # Convert from global to local: subtract monitor's global offset
                global_x = self.cursor_position[0]
                global_y = self.cursor_position[1]
                local_x = global_x - active_mon.x
                local_y = global_y - active_mon.y

            # Preserve raw (unclamped) local coordinates for precise edge detection
            local_x_raw = local_x
            local_y_raw = local_y
            
            # Clamp to monitor bounds to prevent out-of-bounds coordinates
            local_x = max(0, min(local_x, active_mon.width - 1))
            local_y = max(0, min(local_y, active_mon.height - 1))
            
            event_data['x'] = local_x
            event_data['y'] = local_y
            
            # CRITICAL FIX: Only cross edges once the cursor attempts to LEAVE the screen
            # (i.e., raw coordinates go past 0 or width-1). This prevents premature crossings
            # when hovering a pixel away from the edge.
            if event.event_type == 'mouse_move' and not sender_is_active:
                edge_overshoot = 0.1  # small tolerance for float rounding
                edge_to_check = None
                
                if local_x_raw < -edge_overshoot:
                    edge_to_check = 'left'
                elif local_x_raw > (active_mon.width - 1) + edge_overshoot:
                    edge_to_check = 'right'
                elif local_y_raw < -edge_overshoot:
                    edge_to_check = 'top'
                elif local_y_raw > (active_mon.height - 1) + edge_overshoot:
                    edge_to_check = 'bottom'
                
                if edge_to_check:
                    # Check if this edge has a link to another monitor
                    current_mon_id = f"{active_computer_id}:{active_monitor_id}"
                    target = self.layout_manager.get_linked_monitor(current_mon_id, edge_to_check)
                    if target:
                        # Calculate global position from clamped local for accurate crossing
                        clamped_global_x = local_x + active_mon.x
                        clamped_global_y = local_y + active_mon.y
                        
                        logger.info(
                            f"📍 Local edge detect: {current_mon_id} [{edge_to_check}] "
                            f"at local ({local_x:.1f}, {local_y:.1f}) raw ({local_x_raw:.2f}, {local_y_raw:.2f})"
                        )
                        
                        # Perform the edge crossing
                        crossed = await self.perform_edge_crossing(
                            current_mon_id, edge_to_check, (clamped_global_x, clamped_global_y)
                        )
                        if crossed:
                            # Sync cursor_position with where we actually crossed
                            self.cursor_position = (clamped_global_x, clamped_global_y)
                            return  # Don't send this mouse_move, crossing handles it
            
            if ENABLE_MOUSE_POSITION_LOGGING and event.event_type == 'mouse_click':
                logger.info(
                    f"🖱️  Click routed: global ({global_x:.1f}, {global_y:.1f}) → "
                    f"local ({local_x:.1f}, {local_y:.1f}) on {active_computer_id}:{active_monitor_id}"
                    f" [sender_is_active={sender_is_active}]"
                )

        # Prepare the message with active monitor info
        # Clients use this to determine if they should execute the input
        message = json.dumps({
            'type': 'input_event',
            'event_type': event.event_type,
            'data': event_data,  # Use converted coordinates
            'device_id': event.device_id,
            'delta_ms': event.delta_ms,
            'timestamp': event.timestamp,
            'active_computer_id': active_computer_id,  # Which computer is currently active
            'active_monitor_id': active_monitor_id
        })

        # Broadcast to ALL clients
        # Each client will decide whether to execute based on their state
        # Create a copy of the values to avoid RuntimeError during iteration
        for computer in list(self.computers.values()):
            if computer.websocket and not self.shutting_down:
                try:
                    await computer.websocket.send(message)
                    if ENABLE_MOUSE_POSITION_LOGGING and event.event_type == 'mouse_move':
                        is_target = (computer.computer_id == active_computer_id)
                        target_status = "✓ TARGET" if is_target else "✗ other"
                        logger.debug(
                            f"   → {target_status} {computer.computer_id} "
                            f"(local: {event_data.get('x', 0):.1f}, {event_data.get('y', 0):.1f})"
                        )
                    else:
                        logger.debug(f"Broadcast event to {computer.computer_id} (role: {computer.role})")
                except websockets.exceptions.ConnectionClosed:
                    logger.warning(f"Computer {computer.computer_id} connection closed")
                    if not self.shutting_down:
                        await self.unregister_computer(computer.computer_id)
                except Exception as e:
                    logger.error(f"Error broadcasting to {computer.computer_id}: {e}")

    async def broadcast_to_players(self, event: InputEvent):
        """
        Legacy broadcast method - kept for backward compatibility.
        New code should use route_to_active_computer instead.
        """
        if not self.player_websockets:
            logger.debug("No players registered to receive input")
            return

        # Prepare the message
        message = json.dumps({
            'type': 'input_event',
            'event_type': event.event_type,
            'data': event.data,
            'device_id': event.device_id,
            'delta_ms': event.delta_ms,
            'timestamp': event.timestamp
        })

        # Send to all players
        disconnected_players = []
        for player_id, websocket in self.player_websockets.items():
            try:
                await websocket.send(message)
                logger.debug(f"Sent event to player: {player_id}")
            except websockets.exceptions.ConnectionClosed:
                logger.warning(f"Player {player_id} connection closed")
                disconnected_players.append(player_id)
            except Exception as e:
                logger.error(f"Error sending to player {player_id}: {e}")
                disconnected_players.append(player_id)

        # Clean up disconnected players
        for player_id in disconnected_players:
            await self.unregister_player(player_id)

    async def handle_client(self, websocket, path=None):
        """
        Handle a new WebSocket client connection.
        Suppresses harmless InvalidMessage errors from port scanners/bots.
        """
        client_id = f"{websocket.remote_address[0]}:{websocket.remote_address[1]}"
        logger.info(f"Client connected: {client_id}")
        self.clients.add(websocket)

        # Track registered computer for cleanup
        registered_computer_id = None

        try:
            async for message in websocket:
                try:
                    data = json.loads(message)

                    # Handle different message types
                    msg_type = data.get('type')

                    if msg_type == 'register_device':
                        # Dynamic layout registration protocol
                        # New format: {type: 'register_device', computer_id, is_main, monitors}
                        if 'is_main' in data and 'monitors' in data:
                            computer_id = data.get('computer_id', 'unknown')
                            is_main = data.get('is_main', False)
                            monitors_data = data.get('monitors', [])

                            # Register with dynamic layout manager
                            layout_info = await self.register_computer(
                                computer_id, is_main, monitors_data, websocket
                            )
                            registered_computer_id = computer_id

                            # Send acknowledgment with complete layout information
                            await websocket.send(json.dumps({
                                'type': 'registration_success',
                                'computer_id': computer_id,
                                'is_main': is_main,
                                'status': 'success',
                                'layout': layout_info,
                                'active_monitor': {
                                    'computer_id': self.active_monitor[0],
                                    'monitor_id': self.active_monitor[1],
                                    'cursor_x': self.cursor_position[0],
                                    'cursor_y': self.cursor_position[1]
                                }
                            }))

                            # Broadcast layout update to all other clients
                            await self.broadcast_layout_update()

                        # Legacy format with 'role': backward compatibility
                        elif 'role' in data and 'monitors' in data:
                            computer_id = data.get('computer_id', data.get('device_id', 'unknown'))
                            role = data.get('role')
                            is_main = (role == 'main')
                            monitors_data = data.get('monitors', [])

                            layout_info = await self.register_computer(
                                computer_id, is_main, monitors_data, websocket
                            )
                            registered_computer_id = computer_id

                            await websocket.send(json.dumps({
                                'type': 'device_registered',
                                'computer_id': computer_id,
                                'role': role,
                                'status': 'success',
                                'layout': layout_info
                            }))

                        else:
                            # Legacy registration (backward compatibility)
                            device_id = data.get('device_id', 'mylaptop')
                            metadata = data.get('metadata', {})
                            await self.register_device(device_id, metadata)

                            # Send acknowledgment
                            await websocket.send(json.dumps({
                                'type': 'device_registered',
                                'device_id': device_id,
                                'status': 'success'
                            }))

                    elif msg_type == 'register_player':
                        # Legacy player registration (backward compatibility)
                        player_id = data.get('player_id', 'desktop1')
                        metadata = data.get('metadata', {})
                        await self.register_player(player_id, websocket, metadata)

                        # Send acknowledgment
                        await websocket.send(json.dumps({
                            'type': 'player_registered',
                            'player_id': player_id,
                            'status': 'success'
                        }))

                    elif msg_type == 'input_event':
                        # Parse input event
                        event = InputEvent(
                            event_type=data.get('event_type'),
                            data=data.get('data', {}),
                            device_id=data.get('device_id', 'mylaptop'),
                            delta_ms=data.get('delta_ms', 0)
                        )
                        await self.process_input_event(event)

                    elif msg_type == 'edge_crossing_request':
                        # Handle client-side edge crossing request
                        await self.handle_edge_crossing_request(
                            computer_id=data.get('computer_id'),
                            monitor_id=data.get('monitor_id'),
                            edge=data.get('edge'),
                            position=data.get('position'),
                            cursor_x=data.get('cursor_x'),
                            cursor_y=data.get('cursor_y')
                        )

                    elif msg_type == 'ping':
                        await websocket.send(json.dumps({'type': 'pong'}))

                    else:
                        logger.warning(f"Unknown message type: {msg_type}")

                except json.JSONDecodeError as e:
                    logger.error(f"Invalid JSON received: {e}")
                    await websocket.send(json.dumps({
                        'type': 'error',
                        'message': 'Invalid JSON format'
                    }))
                except Exception as e:
                    logger.error(f"Error processing message: {e}")
                    await websocket.send(json.dumps({
                        'type': 'error',
                        'message': str(e)
                    }))

        except websockets.exceptions.ConnectionClosed:
            logger.info(f"Client disconnected: {client_id}")
        finally:
            self.clients.discard(websocket)  # Use discard to avoid KeyError
            # Unregister computer if this was registered (skip during shutdown)
            if registered_computer_id and not self.shutting_down:
                await self.unregister_computer(registered_computer_id)

    async def process_request(self, path, request_headers):
        """Handle HTTP requests (for health checks, browser connections, etc.)"""
        # This allows browsers to connect and see a status page
        if path == "/health" or path == "/":
            return (
                200,
                [("Content-Type", "text/html")],
                b"<html><body><h1>Input Server Running</h1><p>WebSocket endpoint: ws://" +
                f"{self.host}:{self.port}".encode() +
                b"</p><p>Status: OK</p></body></html>"
            )
        # For other paths, let websockets handle it normally
        return None

    async def start(self):
        """Start the WebSocket server"""
        import signal

        logger.info(f"Starting Input Server on {self.host}:{self.port}")

        # Register default device
        await self.register_device('mylaptop', {'default': True})

        # Configure SSL/TLS for secure WebSocket (wss://)
        ssl_context = None
        import ssl

        # Use SSL certificate paths from configuration
        if self.ssl_cert_path and self.ssl_key_path:
            logger.info("SSL certificate paths configured:")
            logger.info(f"  Certificate: {self.ssl_cert_path}")
            logger.info(f"  Private Key: {self.ssl_key_path}")

            # Verify files exist
            if os.path.exists(self.ssl_cert_path) and os.path.exists(self.ssl_key_path):
                try:
                    ssl_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                    ssl_context.load_cert_chain(self.ssl_cert_path, self.ssl_key_path)
                    logger.info("✓ SSL/TLS enabled successfully")
                except Exception as e:
                    logger.error(f"✗ Failed to load SSL certificates: {e}")
                    logger.warning("Server will run without SSL (clients should use ws:// not wss://)")
            else:
                missing = []
                if not os.path.exists(self.ssl_cert_path):
                    missing.append(f"Certificate: {self.ssl_cert_path}")
                if not os.path.exists(self.ssl_key_path):
                    missing.append(f"Private Key: {self.ssl_key_path}")
                logger.error("✗ SSL certificate files not found:")
                for item in missing:
                    logger.error(f"  - {item}")
                logger.warning("Server will run without SSL (clients should use ws:// not wss://)")
        elif self.port == 443:
            logger.warning("⚠ Port 443 configured but no SSL certificates specified in devices.conf")
            logger.warning("For secure WebSocket (wss://), add to devices.conf [server] section:")
            logger.warning("  ssl_cert_path = /path/to/fullchain.pem")
            logger.warning("  ssl_key_path = /path/to/privkey.pem")
            logger.warning("Server will run without SSL (clients should use ws:// not wss://)")

        # Create a stop event for graceful shutdown
        stop_event = asyncio.Event()

        def signal_handler():
            logger.info("Shutdown signal received, stopping server gracefully...")
            self.shutting_down = True
            stop_event.set()

        # Register signal handlers for graceful shutdown
        loop = asyncio.get_event_loop()
        for sig in (signal.SIGINT, signal.SIGTERM):
            try:
                loop.add_signal_handler(sig, signal_handler)
            except NotImplementedError:
                # Windows doesn't support add_signal_handler
                pass

        # Start server with custom request processor
        protocol = "wss" if ssl_context else "ws"
        async with websockets.serve(
            self.handle_client,
            self.host,
            self.port,
            ssl=ssl_context,
            process_request=self.process_request
        ):
            logger.info(f"Input Server running on {protocol}://{self.host}:{self.port}")
            logger.info(f"Health check available at http{'s' if ssl_context else ''}://{self.host}:{self.port}/health")
            await stop_event.wait()  # Wait for shutdown signal
            logger.info("Server stopping...")


async def main():
    import argparse
    
    parser = argparse.ArgumentParser(description='Input Server for Multi-Monitor KVM')
    parser.add_argument('--port', type=int, default=None,
                       help='Port to listen on (overrides config file)')
    parser.add_argument('--no-ssl', action='store_true',
                       help='Disable SSL/TLS (for local LAN use)')
    parser.add_argument('--host', default='0.0.0.0',
                       help='Host to bind to (default: 0.0.0.0)')
    
    args = parser.parse_args()
    
    # Get configuration from devices.conf (or environment variables as fallback)
    # Load config to get port, host, and SSL settings
    ssl_cert_path = None
    ssl_key_path = None

    try:
        from config_loader import load_config
        kvm_config = load_config('devices.conf')
        bind_host = args.host
        port = args.port if args.port else kvm_config.server_port
        
        # Only use SSL if not disabled and certs are configured
        if not args.no_ssl:
            ssl_cert_path = kvm_config.ssl_cert_path
            ssl_key_path = kvm_config.ssl_key_path
        
        logger.info(f"Loaded server config from devices.conf: port={port}")

        if ssl_cert_path and ssl_key_path and not args.no_ssl:
            logger.info(f"SSL certificates configured:")
            logger.info(f"  Cert: {ssl_cert_path}")
            logger.info(f"  Key: {ssl_key_path}")
        elif args.no_ssl:
            logger.info("SSL disabled (--no-ssl flag)")
        else:
            logger.warning(f"No SSL certificates configured in devices.conf")

    except Exception as e:
        # Fallback to environment variables
        logger.warning(f"Could not load devices.conf: {e}, using environment variables")
        bind_host = args.host
        port = args.port if args.port else int(os.getenv('SERVER_PORT', '8765'))

    server = InputServer(host=bind_host, port=port, ssl_cert_path=ssl_cert_path, ssl_key_path=ssl_key_path)
    await server.start()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        logger.info("Server shutdown complete.")
