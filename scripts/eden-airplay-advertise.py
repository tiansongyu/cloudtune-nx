#!/usr/bin/env python3
"""Publish discovery for an Eden NRO; never receive or forward audio."""
import argparse
import ipaddress
import json
import os
from pathlib import Path
import re
import signal
import socket

import dbus
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib


def listening(pid, address, port):
    """Only advertise while this emulator owns the configured TCP listener."""
    inodes = set()
    for fd in Path(f"/proc/{pid}/fd").iterdir():
        try:
            match = re.fullmatch(r"socket:\[(\d+)\]", os.readlink(fd))
            if match:
                inodes.add(match[1])
        except FileNotFoundError:
            pass
    address_hex = socket.inet_aton(address)[::-1].hex().upper()
    endpoints = {f"{address_hex}:{port:04X}", f"00000000:{port:04X}"}
    for row in Path(f"/proc/{pid}/net/tcp").read_text().splitlines()[1:]:
        fields = row.split()
        if fields[1] in endpoints and fields[3] == "0A" and fields[9] in inodes:
            return True
    return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True, help="NRO's emulated SD airplay.json")
    parser.add_argument("--interface", required=True, help="Physical LAN interface, e.g. enp7s0")
    parser.add_argument("--pid", type=int, required=True, help="Eden process owning the NRO")
    args = parser.parse_args()
    interface = socket.if_nametoindex(args.interface)
    process = Path(f"/proc/{args.pid}")
    identity = (process / "stat").read_text().split()[21]
    DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()
    server = dbus.Interface(bus.get_object("org.freedesktop.Avahi", "/"),
                            "org.freedesktop.Avahi.Server")
    group = dbus.Interface(bus.get_object("org.freedesktop.Avahi", server.EntryGroupNew()),
                           "org.freedesktop.Avahi.EntryGroup")
    loop = GLib.MainLoop()
    published = None
    failed = False

    def state_changed(state, error):
        nonlocal failed
        if state in (3, 4):  # AVAHI_ENTRY_GROUP_COLLISION / FAILURE
            failed = True
            print(f"Discovery registration failed: {error}", flush=True)
            loop.quit()

    group.connect_to_signal("StateChanged", state_changed)

    def tick():
        nonlocal published, failed
        try:
            if (process / "stat").read_text().split()[21] != identity:
                loop.quit()
                return False
            config = json.loads(args.config.read_text())
            address = str(ipaddress.IPv4Address(config["address"]))
            port = int(config["port"])
            device = config["device_id"].upper()
            name = config["name"]
            if not re.fullmatch(r"[0-9A-F]{12}", device) or not 1024 <= port <= 65535:
                raise ValueError("Invalid receiver identity or port")
            if config.get("advertise", True):
                raise ValueError("Set advertise=false for the emulator discovery helper")
            active = config.get("enabled", True) and listening(args.pid, address, port)
            current = (address, port, device, name) if active else None
            if current != published:
                group.Reset()
                if active:
                    host = f"cloudtune-eden-{device.lower()}.local"
                    # Match airplay/mdns.cpp: classic UDP RAOP, no AirPlay 2.
                    txt = ["txtvers=1", "ch=2", "cn=0,1", "et=0,1", "ek=1",
                           "sr=44100", "ss=16", "tp=UDP", "vn=65537", "vs=105.1",
                           "md=0,2", "da=true", "sv=false", "sf=0x4",
                           "pw=false", "am=CloudTuneNX"]
                    # AVAHI_PUBLISH_NO_REVERSE: another local service may
                    # already own this host IP's reverse PTR.
                    group.AddAddress(interface, 0, dbus.UInt32(16), host, address)
                    group.AddService(interface, 0, dbus.UInt32(0), f"{device}@{name}",
                                     "_raop._tcp", "", host, dbus.UInt16(port),
                                     [dbus.ByteArray(value.encode()) for value in txt])
                    group.Commit()
                published = current
                print(f"Discovery: {current or 'withdrawn'}", flush=True)
        except FileNotFoundError:
            # Horizon-compatible settings replacement briefly moves the old
            # file aside. Keep watching while the emulator itself is alive.
            if process.exists():
                return True
            loop.quit()
            return False
        except Exception as error:
            failed = True
            print(f"Discovery helper stopped: {error}", flush=True)
            loop.quit()
            return False
        return True

    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda *_: loop.quit())
    try:
        if tick():
            GLib.timeout_add_seconds(1, tick)
            loop.run()
    finally:
        group.Free()
        bus.close()
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
