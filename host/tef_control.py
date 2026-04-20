#!/usr/bin/env python3
"""
TEF6686 Headless Tuner - Host Control CLI

Connect to the tuner via USB CDC serial port.

Usage:
    python tef_control.py /dev/ttyACM0
    python tef_control.py COM3          (Windows)

Interactive commands:
    f/F       Tune down/up 100 kHz (FM)
    g/G       Seek down/up (FM)
    s         Show status
    q         Quit
    u         Show signal quality
    r         Show decoded RDS
    m         Toggle mute
    v +/-     Volume up/down
    e         Toggle event display
    a         Toggle audio stream
    ?         Help
    Ctrl+C    Quit
"""

import sys
import time
import threading
import argparse
from dataclasses import dataclass

try:
    import serial
except ImportError:
    print("pyserial required: pip install pyserial")
    sys.exit(1)


@dataclass
class TunerState:
    band: str = "FM"
    frequency: int = 87500
    tuned: bool = False
    stereo: bool = False
    rds_sync: bool = False
    level: float = 0.0
    snr: int = 0
    bandwidth: int = 0
    # RDS
    pi: str = "0000"
    pty: int = 0
    tp: bool = False
    ta: bool = False
    ps: str = ""
    rt: str = ""
    volume: int = 20
    muted: bool = False
    audio_on: bool = False
    events_on: bool = False


class TefControl:
    def __init__(self, port: str, baud: int = 115200):
        self.ser = serial.Serial(port, baud, timeout=0.1)
        self.state = TunerState()
        self.running = True
        self.lock = threading.Lock()
        self._reader_thread = threading.Thread(target=self._reader, daemon=True)
        self._reader_thread.start()
        time.sleep(1)

    def send(self, cmd: str) -> None:
        with self.lock:
            self.ser.write((cmd.strip() + "\r\n").encode())

    def send_and_wait(self, cmd: str, timeout: float = 2.0) -> str:
        """Send command and wait for OK/ERROR response."""
        with self.lock:
            self.ser.write((cmd.strip() + "\r\n").encode())
            deadline = time.time() + timeout
            buf = ""
            while time.time() < deadline:
                data = self.ser.read(256).decode(errors="replace")
                buf += data
                if "OK " in buf or "ERROR" in buf or "SEEK_FOUND" in buf or "SEEK_ABORTED" in buf:
                    break
                if not data:
                    break
            return buf.strip()

    def _reader(self) -> None:
        """Background reader for async events."""
        buf = ""
        while self.running:
            try:
                data = self.ser.read(256).decode(errors="replace")
                if not data:
                    continue
                buf += data
                while "\r\n" in buf:
                    line, buf = buf.split("\r\n", 1)
                    line = line.strip()
                    if not line:
                        continue
                    self._handle_line(line)
                    if line.startswith("EV_"):
                        self._display_event(line)
            except Exception:
                if self.running:
                    time.sleep(0.1)

    def _handle_line(self, line: str) -> None:
        if line.startswith("STATUS "):
            self._parse_status(line)
        elif line.startswith("EV_STATUS "):
            self._parse_status(line)
        elif line.startswith("EV_RDS ") or line.startswith("RDSDEC "):
            self._parse_rds(line)
        elif line.startswith("QUALITY "):
            self._parse_quality(line)

    def _parse_status(self, line: str) -> None:
        parts = dict(p.split("=", 1) for p in line.split()[1:] if "=" in p)
        self.state.band = parts.get("band", self.state.band)
        self.state.frequency = int(parts.get("freq", self.state.frequency))
        self.state.tuned = parts.get("tuned") == "1"
        self.state.stereo = parts.get("stereo") == "1"
        self.state.rds_sync = parts.get("rds") == "1"
        self.state.level = float(parts.get("level", 0))
        self.state.snr = int(parts.get("snr", 0))

    def _parse_rds(self, line: str) -> None:
        parts = dict(p.split("=", 1) for p in line.split()[1:] if "=" in p)
        self.state.pi = parts.get("pi", self.state.pi)
        self.state.pty = int(parts.get("pty", 0))
        self.state.tp = parts.get("tp") == "1"
        self.state.ta = parts.get("ta") == "1"
        ps = parts.get("ps", "")
        if ps.startswith('"') and ps.endswith('"'):
            ps = ps[1:-1]
        self.state.ps = ps.strip()
        rt = parts.get("rt", "")
        if rt.startswith('"') and rt.endswith('"'):
            rt = rt[1:-1]
        self.state.rt = rt.strip()

    def _parse_quality(self, line: str) -> None:
        parts = dict(p.split("=", 1) for p in line.split()[1:] if "=" in p)
        self.state.level = float(parts.get("level", 0).rstrip("dBf"))
        self.state.snr = int(parts.get("snr", 0).rstrip("dB"))
        self.state.bandwidth = int(parts.get("bw", 0).rstrip("kHz"))

    def _display_event(self, line: str) -> None:
        print(f"\r  {line}")
        sys.stdout.write("> ")
        sys.stdout.flush()

    def tune_fm(self, freq_khz: int) -> None:
        resp = self.send_and_wait(f"TUNE {freq_khz}")
        if "OK" in resp:
            self.state.frequency = freq_khz
            self.state.band = "FM"
            print(f"  Tuned FM {freq_khz / 1000:.1f} MHz")
        else:
            print(f"  {resp}")

    def seek_fm(self, up: bool = True) -> None:
        direction = "UP" if up else "DOWN"
        print(f"  Seeking {direction}...")
        resp = self.send_and_wait(f"SEEK {direction}", timeout=15)
        if "SEEK_FOUND" in resp:
            parts = resp.split()
            for p in parts:
                if p.isdigit():
                    self.state.frequency = int(p)
                    print(f"  Found: {int(p) / 1000:.1f} MHz")
                    break
        else:
            print(f"  {resp}")

    def get_status(self) -> None:
        self.send("STATUS")
        time.sleep(0.2)
        self._print_status()

    def get_quality(self) -> None:
        self.send("QUALITY")
        time.sleep(0.2)
        print(f"  Level: {self.state.level:.1f} dB  SNR: {self.state.snr} dB  BW: {self.state.bandwidth} kHz")

    def get_rds(self) -> None:
        self.send("RDSDEC")
        time.sleep(0.2)
        self._print_rds()

    def toggle_mute(self) -> None:
        new = "OFF" if self.state.muted else "ON"
        self.send(f"MUTE {new}")
        self.state.muted = not self.state.muted
        print(f"  Mute: {'ON' if self.state.muted else 'OFF'}")

    def volume_change(self, delta: int) -> None:
        self.state.volume = max(0, min(30, self.state.volume + delta))
        self.send(f"VOLUME {self.state.volume}")
        print(f"  Volume: {self.state.volume}")

    def toggle_audio(self) -> None:
        new = "OFF" if self.state.audio_on else "ON"
        resp = self.send_and_wait(f"AUDIO {new}")
        if "OK" in resp:
            self.state.audio_on = not self.state.audio_on
            print(f"  Audio: {'ON' if self.state.audio_on else 'OFF'}")
        elif resp:
            print(f"  {resp}")

    def toggle_events(self) -> None:
        new = "OFF" if self.state.events_on else "ON"
        self.send(f"EVENTS {new}")
        self.state.events_on = not self.state.events_on
        print(f"  Events: {'ON' if self.state.events_on else 'OFF'}")

    def _print_status(self) -> None:
        freq = self.state.frequency
        if self.state.band == "FM":
            freq_str = f"{freq / 1000:.2f} MHz"
        else:
            freq_str = f"{freq} kHz"
        flags = []
        if self.state.tuned:
            flags.append("TUNED")
        if self.state.stereo:
            flags.append("STEREO")
        if self.state.rds_sync:
            flags.append("RDS")
        print(f"  {self.state.band} {freq_str}  {' '.join(flags)}  "
              f"Level: {self.state.level:.1f} dB  SNR: {self.state.snr} dB")

    def _print_rds(self) -> None:
        if self.state.ps or self.state.rt:
            print(f"  PI:{self.state.pi} PS:\"{self.state.ps}\" PTY:{self.state.pty} "
                  f"TP:{self.state.tp} TA:{self.state.ta}")
            if self.state.rt:
                print(f"  RT: \"{self.state.rt}\"")
        else:
            print("  No RDS data")

    def close(self) -> None:
        self.running = False
        self.ser.close()


def interactive(ctrl: TefControl) -> None:
    ctrl.get_status()
    print("\nInteractive mode. ?=help, q=quit\n")

    while True:
        try:
            sys.stdout.write("> ")
            sys.stdout.flush()
            ch = sys.stdin.readline().strip().lower()
            if not ch:
                continue

            if ch == "q" or ch == "quit":
                break
            elif ch == "f":
                ctrl.tune_fm(ctrl.state.frequency - 100)
            elif ch == "F":
                ctrl.tune_fm(ctrl.state.frequency + 100)
            elif ch == "g":
                ctrl.seek_fm(up=False)
            elif ch == "G":
                ctrl.seek_fm(up=True)
            elif ch == "s":
                ctrl.get_status()
            elif ch == "u":
                ctrl.get_quality()
            elif ch == "r":
                ctrl.get_rds()
            elif ch == "m":
                ctrl.toggle_mute()
            elif ch == "v+":
                ctrl.volume_change(1)
            elif ch == "v-":
                ctrl.volume_change(-1)
            elif ch == "a":
                ctrl.toggle_audio()
            elif ch == "e":
                ctrl.toggle_events()
            elif ch == "?":
                print(__doc__)
            elif ch.startswith("tune "):
                try:
                    freq = int(ch.split()[1])
                    ctrl.tune_fm(freq)
                except (ValueError, IndexError):
                    print("  Usage: tune <freq_khz>")
            else:
                # Pass through raw command
                resp = ctrl.send_and_wait(ch)
                if resp:
                    print(f"  {resp}")

        except KeyboardInterrupt:
            print()
            break
        except EOFError:
            break


def main():
    parser = argparse.ArgumentParser(description="TEF6686 Headless Tuner Control")
    parser.add_argument("port", help="Serial port (e.g. /dev/ttyACM0, COM3)")
    parser.add_argument("-b", "--baud", type=int, default=115200, help="Baud rate")
    parser.add_argument("-c", "--command", help="Send single command and exit")
    args = parser.parse_args()

    ctrl = TefControl(args.port, args.baud)

    try:
        if args.command:
            resp = ctrl.send_and_wait(args.command)
            print(resp)
        else:
            interactive(ctrl)
    finally:
        ctrl.close()


if __name__ == "__main__":
    main()
