#!/usr/bin/env python3
"""
Configuration Loader for Multi-Monitor KVM System
Reads devices.conf and provides configuration objects
"""

import configparser
import os
import socket
from typing import Dict, List, Tuple, Optional
import logging

logger = logging.getLogger(__name__)


class MonitorConfig:
    """Represents a monitor configuration"""
    def __init__(self, monitor_id: str, x: int, y: int, width: int, height: int):
        self.monitor_id = monitor_id
        self.x = x
        self.y = y
        self.width = width
        self.height = height

    def to_dict(self):
        return {
            'monitor_id': self.monitor_id,
            'x': self.x,
            'y': self.y,
            'width': self.width,
            'height': self.height
        }

    def __repr__(self):
        return f"Monitor({self.monitor_id}, {self.x},{self.y}, {self.width}x{self.height})"


class ComputerConfig:
    """Represents a computer configuration"""
    def __init__(self, computer_id: str, role: str, monitors: List[MonitorConfig]):
        self.computer_id = computer_id
        self.role = role
        self.monitors = monitors

    def __repr__(self):
        return f"Computer({self.computer_id}, role={self.role}, monitors={len(self.monitors)})"


class KVMConfig:
    """Main configuration object for the KVM system"""

    def __init__(self, config_file: str = 'devices.conf'):
        self.config_file = config_file
        self.config = configparser.ConfigParser()

        # Read configuration
        if os.path.exists(config_file):
            self.config.read(config_file)
            logger.info(f"Loaded configuration from {config_file}")
        else:
            logger.warning(f"Configuration file {config_file} not found, using defaults")

        # Parse configuration
        self._load_server_config()
        self._load_computers()
        self._load_monitor_links()
        self._load_behavior_settings()

    def _load_server_config(self):
        """Load server configuration"""
        self.server_host = self.config.get('server', 'host', fallback='0.0.0.0')
        self.server_port = self.config.getint('server', 'port', fallback=443)

        # Load SSL paths - strip whitespace and treat empty strings as None
        ssl_cert = self.config.get('server', 'ssl_cert_path', fallback='').strip()
        ssl_key = self.config.get('server', 'ssl_key_path', fallback='').strip()

        self.ssl_cert_path = ssl_cert if ssl_cert else None
        self.ssl_key_path = ssl_key if ssl_key else None

    def _load_computers(self):
        """Load computer configurations"""
        self.computers: Dict[str, ComputerConfig] = {}

        for section in self.config.sections():
            if section.startswith('computer.'):
                computer_id = section.replace('computer.', '')
                role = self.config.get(section, 'role', fallback='player')

                # Parse monitors
                monitors = []
                monitor_lines = self.config.get(section, 'monitors', fallback='').strip()

                if monitor_lines:
                    for line in monitor_lines.split('\n'):
                        line = line.strip()
                        if line:
                            try:
                                # Format: monitor_id,x,y,width,height
                                parts = line.split(',')
                                if len(parts) == 5:
                                    monitor = MonitorConfig(
                                        monitor_id=parts[0].strip(),
                                        x=int(parts[1].strip()),
                                        y=int(parts[2].strip()),
                                        width=int(parts[3].strip()),
                                        height=int(parts[4].strip())
                                    )
                                    monitors.append(monitor)
                            except (ValueError, IndexError) as e:
                                logger.warning(f"Invalid monitor config in {section}: {line} - {e}")

                self.computers[computer_id] = ComputerConfig(computer_id, role, monitors)
                logger.info(f"Loaded computer: {computer_id} (role={role}, monitors={len(monitors)})")

    def _load_monitor_links(self):
        """Load monitor edge links"""
        self.monitor_links: Dict[Tuple[str, str, str], Tuple[str, str, str]] = {}

        if 'monitor_links' in self.config:
            for key, value in self.config['monitor_links'].items():
                try:
                    # Parse: computer.monitor.edge = target_computer.target_monitor.target_edge
                    source_parts = key.split('.')
                    target_parts = value.strip().split('.')

                    if len(source_parts) == 3 and len(target_parts) == 3:
                        source = (source_parts[0], source_parts[1], source_parts[2])
                        target = (target_parts[0], target_parts[1], target_parts[2])
                        self.monitor_links[source] = target
                        logger.debug(f"Monitor link: {source} -> {target}")
                    else:
                        logger.warning(f"Invalid monitor link format: {key} = {value}")
                except Exception as e:
                    logger.warning(f"Error parsing monitor link {key}: {e}")

    def _load_behavior_settings(self):
        """Load behavior settings"""
        self.edge_threshold = self.config.getint('behavior', 'edge_threshold', fallback=10)
        self.edge_offset = self.config.getint('behavior', 'edge_offset', fallback=20)
        self.hide_cursor = self.config.getboolean('behavior', 'hide_cursor_when_inactive', fallback=True)
        self.auto_detect = self.config.getboolean('detection', 'auto_detect_monitors', fallback=True)

    def get_computer(self, computer_id: str) -> Optional[ComputerConfig]:
        """Get computer configuration by ID"""
        return self.computers.get(computer_id)

    def get_current_computer(self) -> Optional[ComputerConfig]:
        """Get configuration for the current computer (by hostname)"""
        hostname = socket.gethostname()
        return self.get_computer(hostname)

    def get_monitor_links_dict(self) -> Dict:
        """Get monitor links as a dictionary (for server)"""
        return self.monitor_links

    def get_server_url(self, protocol: str = None) -> str:
        """
        Get server WebSocket URL.
        Auto-detects protocol based on port if not specified:
        - Port 443 -> wss:// (secure WebSocket)
        - Other ports -> ws:// (plain WebSocket)
        """
        if protocol is None:
            # Auto-detect protocol based on port
            protocol = 'wss' if self.server_port == 443 else 'ws'

        return f"{protocol}://{self.server_host}:{self.server_port}"


def load_config(config_file: str = 'devices.conf') -> KVMConfig:
    """Load KVM configuration from file"""
    return KVMConfig(config_file)


# Example usage
if __name__ == "__main__":
    logging.basicConfig(level=logging.DEBUG)

    config = load_config()

    print("=== KVM Configuration ===")
    print(f"Server: {config.server_host}:{config.server_port}")
    print(f"\nComputers:")
    for comp_id, comp in config.computers.items():
        print(f"  {comp}")
        for mon in comp.monitors:
            print(f"    {mon}")

    print(f"\nMonitor Links:")
    for source, target in config.monitor_links.items():
        print(f"  {source} -> {target}")

    print(f"\nBehavior:")
    print(f"  Edge threshold: {config.edge_threshold}px")
    print(f"  Edge offset: {config.edge_offset}px")
    print(f"  Hide cursor: {config.hide_cursor}")
