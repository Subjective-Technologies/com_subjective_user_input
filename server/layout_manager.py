"""
Dynamic Layout Manager for Multi-Monitor KVM System

Automatically arranges monitors as clients connect, eliminating the need for
manual configuration files. Uses OOP design patterns:
- Strategy Pattern: Different placement algorithms
- Factory Pattern: Creates appropriate strategies
- Facade Pattern: Simple interface to complex layout logic
"""

import logging
import time
from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple, Any

logger = logging.getLogger(__name__)


# ============================================================================
# Core Data Models
# ============================================================================

@dataclass
class MonitorInfo:
    """Represents a physical monitor with its properties and position"""
    monitor_id: str          # e.g., "m0", "m1"
    width: int              # Resolution width in pixels
    height: int             # Resolution height in pixels
    x: int = 0              # Global position X (top-left corner)
    y: int = 0              # Global position Y (top-left corner)

    def to_dict(self) -> dict:
        """Convert to dictionary for JSON serialization"""
        return {
            'monitor_id': self.monitor_id,
            'width': self.width,
            'height': self.height,
            'x': self.x,
            'y': self.y
        }


@dataclass
class ComputerInfo:
    """Represents a connected computer/client with its monitors"""
    computer_id: str         # e.g., "minipc", "laptop"
    is_main: bool           # True if this has physical keyboard/mouse
    monitors: List[MonitorInfo]
    websocket: Optional[Any] = None
    connected_at: float = field(default_factory=time.time)

    def to_dict(self) -> dict:
        """Convert to dictionary for JSON serialization"""
        return {
            'computer_id': self.computer_id,
            'is_main': self.is_main,
            'monitors': [m.to_dict() for m in self.monitors],
            'connected_at': self.connected_at
        }


@dataclass
class EdgeLink:
    """Represents a bidirectional connection between monitor edges"""
    from_computer: str
    from_monitor: str
    from_edge: str          # "left", "right", "top", "bottom"
    to_computer: str
    to_monitor: str
    to_edge: str

    def to_dict(self) -> dict:
        """Convert to dictionary for JSON serialization"""
        return {
            'from': f"{self.from_computer}.{self.from_monitor}.{self.from_edge}",
            'to': f"{self.to_computer}.{self.to_monitor}.{self.to_edge}"
        }


# ============================================================================
# Layout Strategy Pattern
# ============================================================================

class LayoutStrategy(ABC):
    """Abstract base class for monitor placement strategies"""

    @abstractmethod
    def calculate_position(
        self,
        existing_monitors: List[MonitorInfo],
        new_monitor: MonitorInfo,
        reference_computer: ComputerInfo
    ) -> Tuple[int, int]:
        """
        Calculate x, y position for new monitor.

        Args:
            existing_monitors: All currently placed monitors
            new_monitor: The monitor being placed
            reference_computer: The computer to place relative to (usually main)

        Returns:
            Tuple of (x, y) coordinates for the new monitor
        """
        pass

    @abstractmethod
    def create_edge_links(
        self,
        new_computer: ComputerInfo,
        reference_computer: ComputerInfo
    ) -> List[EdgeLink]:
        """
        Create bidirectional edge links between monitors.

        Args:
            new_computer: The newly connected computer
            reference_computer: The computer to link to (usually main)

        Returns:
            List of EdgeLink objects (bidirectional)
        """
        pass


class RightPlacementStrategy(LayoutStrategy):
    """Place new monitor to the right of reference monitor"""

    def calculate_position(
        self,
        existing_monitors: List[MonitorInfo],
        new_monitor: MonitorInfo,
        reference_computer: ComputerInfo
    ) -> Tuple[int, int]:
        ref_monitor = reference_computer.monitors[0]
        x = ref_monitor.x + ref_monitor.width
        y = ref_monitor.y
        return (x, y)

    def create_edge_links(
        self,
        new_computer: ComputerInfo,
        reference_computer: ComputerInfo
    ) -> List[EdgeLink]:
        ref_mon = reference_computer.monitors[0]
        new_mon = new_computer.monitors[0]

        return [
            # Reference right edge -> New left edge
            EdgeLink(
                reference_computer.computer_id, ref_mon.monitor_id, "right",
                new_computer.computer_id, new_mon.monitor_id, "left"
            ),
            # New left edge -> Reference right edge
            EdgeLink(
                new_computer.computer_id, new_mon.monitor_id, "left",
                reference_computer.computer_id, ref_mon.monitor_id, "right"
            )
        ]


class LeftPlacementStrategy(LayoutStrategy):
    """Place new monitor to the left of reference monitor"""

    def calculate_position(
        self,
        existing_monitors: List[MonitorInfo],
        new_monitor: MonitorInfo,
        reference_computer: ComputerInfo
    ) -> Tuple[int, int]:
        ref_monitor = reference_computer.monitors[0]
        x = ref_monitor.x - new_monitor.width
        y = ref_monitor.y
        return (x, y)

    def create_edge_links(
        self,
        new_computer: ComputerInfo,
        reference_computer: ComputerInfo
    ) -> List[EdgeLink]:
        ref_mon = reference_computer.monitors[0]
        new_mon = new_computer.monitors[0]

        return [
            # Reference left edge -> New right edge
            EdgeLink(
                reference_computer.computer_id, ref_mon.monitor_id, "left",
                new_computer.computer_id, new_mon.monitor_id, "right"
            ),
            # New right edge -> Reference left edge
            EdgeLink(
                new_computer.computer_id, new_mon.monitor_id, "right",
                reference_computer.computer_id, ref_mon.monitor_id, "left"
            )
        ]


class TopPlacementStrategy(LayoutStrategy):
    """Place new monitor above reference monitor"""

    def calculate_position(
        self,
        existing_monitors: List[MonitorInfo],
        new_monitor: MonitorInfo,
        reference_computer: ComputerInfo
    ) -> Tuple[int, int]:
        ref_monitor = reference_computer.monitors[0]
        x = ref_monitor.x
        y = ref_monitor.y - new_monitor.height
        return (x, y)

    def create_edge_links(
        self,
        new_computer: ComputerInfo,
        reference_computer: ComputerInfo
    ) -> List[EdgeLink]:
        ref_mon = reference_computer.monitors[0]
        new_mon = new_computer.monitors[0]

        return [
            # Reference top edge -> New bottom edge
            EdgeLink(
                reference_computer.computer_id, ref_mon.monitor_id, "top",
                new_computer.computer_id, new_mon.monitor_id, "bottom"
            ),
            # New bottom edge -> Reference top edge
            EdgeLink(
                new_computer.computer_id, new_mon.monitor_id, "bottom",
                reference_computer.computer_id, ref_mon.monitor_id, "top"
            )
        ]


class BottomPlacementStrategy(LayoutStrategy):
    """Place new monitor below reference monitor"""

    def calculate_position(
        self,
        existing_monitors: List[MonitorInfo],
        new_monitor: MonitorInfo,
        reference_computer: ComputerInfo
    ) -> Tuple[int, int]:
        ref_monitor = reference_computer.monitors[0]
        x = ref_monitor.x
        y = ref_monitor.y + ref_monitor.height
        return (x, y)

    def create_edge_links(
        self,
        new_computer: ComputerInfo,
        reference_computer: ComputerInfo
    ) -> List[EdgeLink]:
        ref_mon = reference_computer.monitors[0]
        new_mon = new_computer.monitors[0]

        return [
            # Reference bottom edge -> New top edge
            EdgeLink(
                reference_computer.computer_id, ref_mon.monitor_id, "bottom",
                new_computer.computer_id, new_mon.monitor_id, "top"
            ),
            # New top edge -> Reference bottom edge
            EdgeLink(
                new_computer.computer_id, new_mon.monitor_id, "top",
                reference_computer.computer_id, ref_mon.monitor_id, "bottom"
            )
        ]


# ============================================================================
# Layout Strategy Factory
# ============================================================================

class LayoutStrategyFactory:
    """Factory for creating layout strategies based on connection order"""

    _strategies = [
        RightPlacementStrategy(),   # 1st additional computer
        LeftPlacementStrategy(),    # 2nd additional computer
        TopPlacementStrategy(),     # 3rd additional computer
        BottomPlacementStrategy()   # 4th additional computer
    ]

    @classmethod
    def get_strategy_for_position(cls, position: int) -> Optional[LayoutStrategy]:
        """
        Get strategy based on connection order:
        0: main computer (no strategy needed - placed at origin)
        1: right of main
        2: left of main
        3: top of main
        4: bottom of main
        5+: cycles through strategies again

        Args:
            position: The connection order (0 = first/main, 1 = second, etc.)

        Returns:
            LayoutStrategy instance or None for main computer
        """
        if position == 0:
            return None  # Main computer doesn't need a strategy

        # Cycle through strategies for positions 1, 2, 3, 4, 5, ...
        strategy_index = (position - 1) % len(cls._strategies)
        return cls._strategies[strategy_index]


# ============================================================================
# Layout Manager (Facade Pattern)
# ============================================================================

class LayoutManager:
    """
    Manages the global monitor layout with automatic arrangement.

    This class provides a simple facade interface to the complex layout logic,
    handling computer registration, monitor positioning, and edge link creation.
    """

    def __init__(self):
        self.computers: Dict[str, ComputerInfo] = {}
        self.edge_links: List[EdgeLink] = []
        self.main_computer_id: Optional[str] = None
        self.connection_order: List[str] = []  # Track order of connections

    def register_computer(
        self,
        computer_id: str,
        is_main: bool,
        monitors: List[MonitorInfo],
        websocket: Any
    ) -> dict:
        """
        Register a new computer and automatically arrange its monitors.

        Args:
            computer_id: Unique identifier for the computer
            is_main: True if this computer has physical keyboard/mouse
            monitors: List of MonitorInfo objects for this computer
            websocket: WebSocket connection for this computer

        Returns:
            Dictionary with complete layout information

        Raises:
            ValueError: If constraints are violated (e.g., multiple main computers)
        """
        # Create computer info
        computer = ComputerInfo(
            computer_id=computer_id,
            is_main=is_main,
            monitors=monitors,
            websocket=websocket
        )

        # First computer to connect becomes main automatically
        if not self.computers:
            # First computer - always treat as main regardless of is_main flag
            computer.is_main = True  # Override to ensure first computer is main

            # Place main computer at origin (0, 0)
            for monitor in computer.monitors:
                monitor.x = 0
                monitor.y = 0

            self.main_computer_id = computer_id
            self.computers[computer_id] = computer
            self.connection_order.append(computer_id)

            logger.info(f"✓ Registered MAIN computer (first to connect): {computer_id} at origin")
            return self.get_layout_info()

        # Additional computers - always treat as player
        if is_main:
            logger.warning(f"Computer {computer_id} requested main role, but main already exists ({self.main_computer_id}). Registering as player.")

        computer.is_main = False  # Override to ensure additional computers are players

        # Get placement strategy based on connection order
        position = len(self.connection_order)
        strategy = LayoutStrategyFactory.get_strategy_for_position(position)

        # Get reference computer (main computer)
        main_computer = self.computers[self.main_computer_id]

        # Calculate positions for new monitors
        for monitor in computer.monitors:
            x, y = strategy.calculate_position(
                self._get_all_monitors(),
                monitor,
                main_computer
            )
            monitor.x = x
            monitor.y = y

        # Create edge links
        new_links = strategy.create_edge_links(computer, main_computer)
        self.edge_links.extend(new_links)

        # Register computer
        self.computers[computer_id] = computer
        self.connection_order.append(computer_id)

        logger.info(
            f"✓ Registered computer: {computer_id} "
            f"using {strategy.__class__.__name__} "
            f"at position ({computer.monitors[0].x}, {computer.monitors[0].y})"
        )

        return self.get_layout_info()

    def unregister_computer(self, computer_id: str) -> dict:
        """
        Unregister a computer and update layout.

        Args:
            computer_id: ID of computer to unregister

        Returns:
            Dictionary with updated layout information
        """
        if computer_id not in self.computers:
            logger.warning(f"Computer {computer_id} not registered, nothing to unregister")
            return self.get_layout_info()

        # Remove computer
        del self.computers[computer_id]
        self.connection_order.remove(computer_id)

        # Remove edge links involving this computer
        self.edge_links = [
            link for link in self.edge_links
            if link.from_computer != computer_id and link.to_computer != computer_id
        ]

        logger.info(f"✗ Unregistered computer: {computer_id}")

        # If main computer disconnected, reset everything
        if computer_id == self.main_computer_id:
            self.computers.clear()
            self.edge_links.clear()
            self.main_computer_id = None
            self.connection_order.clear()
            logger.warning("⚠ Main computer disconnected - layout completely reset")

        return self.get_layout_info()

    def _get_all_monitors(self) -> List[MonitorInfo]:
        """Get flat list of all monitors across all computers"""
        monitors = []
        for computer in self.computers.values():
            monitors.extend(computer.monitors)
        return monitors

    def get_edge_links_dict(self) -> Dict[Tuple[str, str, str], Tuple[str, str, str]]:
        """
        Get edge links as a dictionary for fast lookup.

        Returns:
            Dict mapping (computer_id, monitor_id, edge) to (target_computer, target_monitor, target_edge)
        """
        links_dict = {}
        for link in self.edge_links:
            key = (link.from_computer, link.from_monitor, link.from_edge)
            value = (link.to_computer, link.to_monitor, link.to_edge)
            links_dict[key] = value
        return links_dict

    def get_linked_monitor(
        self,
        monitor_ref,
        edge: str
    ) -> Optional[Tuple[str, str, str]]:
        """
        Look up which monitor/edge is linked to the given monitor edge.

        Args:
            monitor_ref: Either a tuple (computer_id, monitor_id) or string
                         formatted as "computer_id:monitor_id".
            edge: Edge name ("left", "right", "top", "bottom").

        Returns:
            Tuple (target_computer_id, target_monitor_id, target_edge) or None
            if no link exists.
        """
        if isinstance(monitor_ref, tuple):
            computer_id, monitor_id = monitor_ref
        else:
            parts = str(monitor_ref).split(':', 1)
            if len(parts) != 2:
                return None
            computer_id, monitor_id = parts

        for link in self.edge_links:
            if (
                link.from_computer == computer_id and
                link.from_monitor == monitor_id and
                link.from_edge == edge
            ):
                return (link.to_computer, link.to_monitor, link.to_edge)

        return None

    def get_layout_info(self) -> dict:
        """
        Get complete layout information including ASCII visualization.

        Returns:
            Dictionary with computers, edge_links, main_computer_id, and visualization
        """
        return {
            'computers': {
                cid: comp.to_dict()
                for cid, comp in self.computers.items()
            },
            'edge_links': [link.to_dict() for link in self.edge_links],
            'main_computer_id': self.main_computer_id,
            'connection_order': self.connection_order,
            'visualization': self.generate_ascii_visualization()
        }

    def generate_ascii_visualization(self) -> str:
        """
        Generate ASCII art visualization of the monitor layout.

        Returns:
            Multi-line string with ASCII art showing monitor positions
        """
        if not self.computers:
            return "╔════════════════════════════════╗\n║  No computers connected yet    ║\n╔════════════════════════════════╗"

        # Find bounds of the entire layout
        all_monitors = self._get_all_monitors()
        if not all_monitors:
            return "No monitors found"

        min_x = min(m.x for m in all_monitors)
        max_x = max(m.x + m.width for m in all_monitors)
        min_y = min(m.y for m in all_monitors)
        max_y = max(m.y + m.height for m in all_monitors)

        # Scale to ASCII grid (1 char = 100 pixels for better proportions)
        scale = 100
        width = (max_x - min_x) // scale + 3
        height = (max_y - min_y) // scale + 3

        # Create grid filled with spaces
        grid = [[' ' for _ in range(width)] for _ in range(height)]
        labels = {}

        # Draw each monitor
        for computer in self.computers.values():
            for monitor in computer.monitors:
                # Calculate grid position
                gx = (monitor.x - min_x) // scale + 1
                gy = (monitor.y - min_y) // scale + 1
                gw = max(3, monitor.width // scale)
                gh = max(2, monitor.height // scale)

                # Choose symbol based on computer type
                symbol = '█' if computer.is_main else '▓'
                corner = '█' if computer.is_main else '▓'

                # Draw border
                # Top and bottom borders
                for x in range(gx, min(gx + gw, width)):
                    if 0 <= gy < height:
                        grid[gy][x] = symbol
                    if 0 <= gy + gh - 1 < height:
                        grid[gy + gh - 1][x] = symbol

                # Left and right borders
                for y in range(gy, min(gy + gh, height)):
                    if 0 <= y < height:
                        if gx < width:
                            grid[y][gx] = symbol
                        if gx + gw - 1 < width:
                            grid[y][gx + gw - 1] = symbol

                # Draw corners
                if 0 <= gy < height and gx < width:
                    grid[gy][gx] = corner
                if 0 <= gy < height and gx + gw - 1 < width:
                    grid[gy][gx + gw - 1] = corner
                if 0 <= gy + gh - 1 < height and gx < width:
                    grid[gy + gh - 1][gx] = corner
                if 0 <= gy + gh - 1 < height and gx + gw - 1 < width:
                    grid[gy + gh - 1][gx + gw - 1] = corner

                # Store label position (center of monitor)
                label_x = gx + gw // 2
                label_y = gy + gh // 2
                if 0 <= label_y < height and 0 <= label_x < width:
                    # Truncate long names and add resolution info
                    label = f"{computer.computer_id[:8]}"
                    res_label = f"{monitor.width}x{monitor.height}"
                    labels[(label_y, label_x)] = (label, res_label)

        # Add labels (computer name and resolution)
        for (y, x), (label, res_label) in labels.items():
            # Add computer name
            start_x = max(0, x - len(label) // 2)
            for i, char in enumerate(label):
                if start_x + i < width:
                    grid[y][start_x + i] = char

            # Add resolution below
            if y + 1 < height:
                start_x = max(0, x - len(res_label) // 2)
                for i, char in enumerate(res_label):
                    if start_x + i < width:
                        grid[y + 1][start_x + i] = char

        # Convert grid to string
        lines = [''.join(row) for row in grid]

        # Add header
        header = [
            "",
            "╔═══════════════════════════════════════════════════════════════╗",
            "║              DYNAMIC MONITOR LAYOUT                           ║",
            "╚═══════════════════════════════════════════════════════════════╝",
            ""
        ]

        # Add legend
        legend = [
            "",
            "Legend:",
            "  █ = Main computer (has physical keyboard/mouse)",
            "  ▓ = Player computer (receives input remotely)",
            ""
        ]

        # Add connection info
        info = [
            f"Connected: {len(self.computers)} computer(s)",
            f"Main: {self.main_computer_id or 'None'}",
            f"Order: {' → '.join(self.connection_order)}",
            f"Links: {len(self.edge_links)} edge connection(s)",
            ""
        ]

        return '\n'.join(header + lines + legend + info)
