"""Simple System Monitor

Monitors CPU, memory, disk and network. Shows a small Tkinter GUI
and sends Windows toast notifications (via `win10toast`) when
thresholds are exceeded. Can run in console mode with `--console`.

Usage examples:
    python sysmon.py            # GUI (if tkinter available)
    python sysmon.py --console  # console mode

Configuration is available via CLI flags (see --help).
"""

import os
import psutil
import time
import sys
import subprocess
import re
import threading
from datetime import datetime
import argparse

try:
    import tkinter as tk
    from tkinter import ttk, messagebox, simpledialog
    TK_AVAILABLE = True
except Exception:
    TK_AVAILABLE = False

# Disable win10toast by default to avoid Windows message-loop crashes.
# If you want toast notifications, install and enable them manually.
_win_toaster = None
ENABLE_TOASTS = False

def send_windows_notification(title, message, duration=10):
    global _win_toaster
    # Lazily import/instantiate win10toast only if enabled by the user
    # If toasts are enabled, spawn a separate Python process to show the
    # toast using win10toast. Running win10toast in a separate process
    # avoids interfering with the main process' message loop (which has
    # caused crashes on some Windows setups).
    if ENABLE_TOASTS:
        try:
            # Build a small Python command to show the toast. Use repr() to
            # safely quote the title/message.
            cmd_py = (
                "from win10toast import ToastNotifier; "
                "ToastNotifier().show_toast(%r, %r, duration=%d, threaded=False)"
            ) % (title, message, int(duration))
            # Use the same Python executable to ensure environment matches.
            creationflags = 0
            try:
                creationflags = subprocess.CREATE_NO_WINDOW
            except Exception:
                creationflags = 0
            subprocess.Popen([sys.executable, '-u', '-c', cmd_py],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                             creationflags=creationflags)
            return True, 'Launched external toaster'
        except Exception as e:
            try:
                print(f"[NOTIFICATION LAUNCH FAIL] {e}")
            except Exception:
                pass

    # Final fallback: print to console
    try:
        print(f"[NOTIFICATION] {title}: {message}")
    except Exception:
        pass
    return False, 'Toast unavailable; printed to console'


def _set_tk_window_icon_to_python(root):
    """On Windows, load the icon from the current Python executable and apply
    it to the Tk root window. Returns True on success, False otherwise."""
    try:
        if os.name != 'nt':
            return False
        import ctypes
        # Ensure we have a valid window id
        try:
            hwnd = int(root.winfo_id())
        except Exception:
            return False
        exe_path = sys.executable
        # Constants
        IMAGE_ICON = 1
        LR_LOADFROMFILE = 0x00000010
        # Try LoadImageW from user32 to load icon from the exe file
        LoadImage = ctypes.windll.user32.LoadImageW
        try:
            LoadImage.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_uint, ctypes.c_int, ctypes.c_int, ctypes.c_uint]
            LoadImage.restype = ctypes.c_void_p
        except Exception:
            pass
        hicon = None
        try:
            hicon = LoadImage(0, exe_path, IMAGE_ICON, 0, 0, LR_LOADFROMFILE)
        except Exception:
            hicon = None
        if not hicon:
            # Fallback: try ExtractIcon from shell32
            try:
                hicon = ctypes.windll.shell32.ExtractIconW(0, exe_path, 0)
            except Exception:
                hicon = None
        if not hicon:
            return False
        try:
            WM_SETICON = 0x80
            ICON_SMALL = 0
            ICON_BIG = 1
            # Ensure arguments are integers to avoid ctypes type errors
            ctypes.windll.user32.SendMessageW(hwnd, WM_SETICON, ICON_SMALL, int(hicon))
            ctypes.windll.user32.SendMessageW(hwnd, WM_SETICON, ICON_BIG, int(hicon))
            return True
        except Exception:
            return False
    except Exception:
        pass
    return False


def get_system_info_dict():
    info = {}
    info['timestamp'] = datetime.now()
    info['cpu_percent'] = psutil.cpu_percent(interval=None)
    info['cpu_count'] = psutil.cpu_count()
    memory = psutil.virtual_memory()
    # store memory object and a safe 0-100% value
    info['memory'] = memory
    info['memory_percent'] = min(max(getattr(memory, 'percent', 0.0) or 0.0, 0.0), 100.0)
    # Collect usage for all mounted partitions/drives
    partitions = []
    try:
        for part in psutil.disk_partitions(all=False):
            try:
                usage = psutil.disk_usage(part.mountpoint)
            except Exception:
                continue
            partitions.append({'device': part.device, 'mountpoint': part.mountpoint, 'fstype': part.fstype, 'opts': part.opts, 'usage': usage})
    except Exception:
        # Fallback: single root path
        root_path = '/' if os.name != 'nt' else 'C:\\'
        try:
            disk = psutil.disk_usage(root_path)
            partitions.append({'device': root_path, 'mountpoint': root_path, 'fstype': '', 'opts': '', 'usage': disk})
        except Exception:
            partitions = []
    info['disk_partitions'] = partitions
    # (top processes removed) keep minimal process info out of monitoring
    net_io = psutil.net_io_counters()
    info['net_io'] = net_io

    # Per-interface stats and simple type classification (ethernet/wireless)
    try:
        if_addrs = psutil.net_if_addrs()
        if_stats = psutil.net_if_stats()
        net_per_nic = []
        import platform
        system = platform.system().lower()
        for ifname, addrs in if_addrs.items():
            stats = if_stats.get(ifname)
            is_up = stats.isup if stats is not None else False
            speed = stats.speed if stats is not None else 0
            # bytes for this nic (if available)
            try:
                pernic = psutil.net_io_counters(pernic=True).get(ifname)
            except Exception:
                pernic = None

            # Heuristic to detect wireless vs ethernet
            lname = ifname.lower()
            nic_type = 'unknown'
            if lname.startswith('lo') or 'loopback' in lname:
                nic_type = 'loopback'
            elif system == 'windows':
                if 'wi-fi' in lname or 'wifi' in lname or 'wireless' in lname or 'wlan' in lname:
                    nic_type = 'wireless'
                else:
                    nic_type = 'ethernet'
            else:
                # linux / darwin heuristics
                if lname.startswith('wl') or 'wifi' in lname or 'wlan' in lname:
                    nic_type = 'wireless'
                elif lname.startswith('en') or lname.startswith('eth'):
                    nic_type = 'ethernet'
            net_per_nic.append({'name': ifname, 'is_up': is_up, 'speed': speed, 'type': nic_type, 'io': pernic})
    except Exception:
        net_per_nic = []
    info['net_interfaces'] = net_per_nic
    return info


def print_system_info(info):
    ts = info.get('timestamp', datetime.now()).strftime('%Y-%m-%d %H:%M:%S')
    print('\n' + '=' * 60)
    print(f"System Monitor - {ts}")
    print('=' * 60)
    print(f"\nCPU Usage: {info['cpu_percent']:.1f}%")
    print(f"CPU Count: {info['cpu_count']} cores")
    memory = info['memory']
    print('\nMemory:')
    print(f"  Total: {memory.total / (1024**3):.2f} GB")
    print(f"  Used: {memory.used / (1024**3):.2f} GB")
    print(f"  Available: {memory.available / (1024**3):.2f} GB")
    mem_percent = info.get('memory_percent', getattr(memory, 'percent', 0.0))
    mem_percent = min(max(mem_percent or 0.0, 0.0), 100.0)
    print(f"  Percent: {mem_percent}%")
    print('\nDisk partitions:')
    partitions = info.get('disk_partitions', [])
    if partitions:
        for part in partitions:
            usage = part.get('usage')
            dev = part.get('device') or part.get('mountpoint')
            mp = part.get('mountpoint')
            fst = part.get('fstype')
            if usage:
                print(f"  {dev} mounted on {mp} ({fst}): Total {usage.total / (1024**3):.2f} GB, Used {usage.used / (1024**3):.2f} GB, Free {usage.free / (1024**3):.2f} GB, Percent {usage.percent}%")
            else:
                print(f"  {dev} mounted on {mp} ({fst}): usage unavailable")
    else:
        print('  No partition information available')
    # top processes removed
    net_io = info.get('net_io')
    print('\nNetwork (total):')
    if net_io:
        print(f"  Bytes Sent: {net_io.bytes_sent / (1024**2):.2f} MB")
        print(f"  Bytes Received: {net_io.bytes_recv / (1024**2):.2f} MB")
    else:
        print('  Overall network IO unavailable')

class ResourceMonitor:
    def __init__(self, cpu_threshold=90, mem_threshold=70, interval=1, rate_limit_min=5, notify_interval_sec=10, email_cfg=None, on_update=None, on_log=None, ping_host='8.8.8.8'):
        self.cpu_threshold = cpu_threshold
        self.mem_threshold = mem_threshold
        # monitoring sample interval (seconds) for updates
        self.interval = max(1, int(interval))
        # legacy rate limit in minutes (kept for compatibility, not used for notifications)
        self.rate_limit_sec = max(0, int(rate_limit_min) * 60)
        # how often to repeat notifications while condition persists
        self.notify_interval_sec = max(1, int(notify_interval_sec))
        self.email_cfg = email_cfg or {}
        self.on_update = on_update
        self.on_log = on_log
        self._running = False
        self._thread = None
        self._last_alert = None
        self._alerting = False
        # track last network counters to compute instantaneous bandwidth
        self._last_net_io = None
        self._last_net_time = None
        self.ping_host = ping_host

    def start(self):
        if self._running:
            return
        self._running = True
        self._thread = threading.Thread(target=self._loop, daemon=True)
        self._thread.start()
        self._log('Monitor started')

    def stop(self):
        self._running = False
        self._log('Monitor stopped')

    def _log(self, msg):
        if self.on_log:
            try:
                self.on_log(msg)
            except Exception:
                pass
        else:
            print(f"[{datetime.now().isoformat()}] {msg}")

    def _loop(self):
        clear_cpu = max(0, self.cpu_threshold - 15)
        clear_mem = max(0, self.mem_threshold - 15)
        while self._running:
            info = get_system_info_dict()
            # compute instantaneous network bandwidth (bytes/sec) and ping
            now_ts = time.time()
            net_io = info.get('net_io')
            up_bps = down_bps = 0.0
            if net_io:
                if self._last_net_io is not None and self._last_net_time is not None:
                    elapsed = max(1e-6, now_ts - self._last_net_time)
                    try:
                        delta_sent = float(net_io.bytes_sent - self._last_net_io.bytes_sent)
                        delta_recv = float(net_io.bytes_recv - self._last_net_io.bytes_recv)
                        up_bps = delta_sent / elapsed
                        down_bps = delta_recv / elapsed
                    except Exception:
                        up_bps = down_bps = 0.0
                # save for next sample
                try:
                    self._last_net_io = net_io
                    self._last_net_time = now_ts
                except Exception:
                    self._last_net_io = None
                    self._last_net_time = None
            info['net_upload_bps'] = up_bps
            info['net_download_bps'] = down_bps
            # ping the configured host (non-blocking-ish, but okay on monitor thread)
            ping_ms = None
            try:
                import platform
                system = platform.system().lower()
                if system == 'windows':
                    proc = subprocess.run(['ping', '-n', '1', '-w', '1000', self.ping_host], capture_output=True, text=True, timeout=2)
                else:
                    proc = subprocess.run(['ping', '-c', '1', '-W', '1', self.ping_host], capture_output=True, text=True, timeout=2)
                out = proc.stdout if proc.stdout else proc.stderr
                m = re.search(r'time[=<]\s*([0-9]+(?:\.[0-9]+)?)\s*ms', out)
                if m:
                    ping_ms = float(m.group(1))
                else:
                    # try alternative pattern (Windows sometimes uses 'time=1ms')
                    m2 = re.search(r'time=\s*([0-9]+)ms', out)
                    if m2:
                        ping_ms = float(m2.group(1))
            except Exception:
                ping_ms = None
            info['ping_ms'] = ping_ms
            cpu = info['cpu_percent']
            mem = info['memory'].percent
            if self.on_update:
                try:
                    self.on_update(info)
                except Exception:
                    pass

            now = datetime.now()
            trigger_reasons = []
            # detect threshold breaches regardless of current alerting state
            if cpu >= self.cpu_threshold:
                trigger_reasons.append(f"CPU {cpu:.1f}% >= {self.cpu_threshold}%")
            if mem >= self.mem_threshold:
                trigger_reasons.append(f"Memory {mem:.1f}% >= {self.mem_threshold}%")

            if trigger_reasons:
                # suppress alerts until we have non-zero readings to avoid false positives at startup
                if cpu <= 0.0 and mem <= 0.0:
                    self._log('Alert suppressed: metrics zero (waiting for real readings)')
                else:
                    can_send = (self._last_alert is None) or ((now - self._last_alert).total_seconds() >= self.notify_interval_sec)
                    if can_send:
                        subject = 'Resource alert: ' + ', '.join(trigger_reasons)
                        # build a simple body without per-process details (removed)
                        body_lines = [f"Timestamp: {now.isoformat()}", f"CPU: {cpu:.1f}%", f"Memory: {mem:.1f}%"]
                        # include current upload/download speeds and ping if available
                        up_bps = info.get('net_upload_bps', 0.0)
                        down_bps = info.get('net_download_bps', 0.0)
                        if up_bps is not None:
                            body_lines.append(f"Upload: {up_bps/1024:.2f} KB/s")
                            body_lines.append(f"Download: {down_bps/1024:.2f} KB/s")
                        if info.get('ping_ms') is not None:
                            body_lines.append(f"Ping: {info.get('ping_ms'):.0f} ms")
                        body = '\n'.join(body_lines)
                        # Use Windows notifications instead of SMTP for alerts
                        success, msg = send_windows_notification(subject, body)
                        if success:
                            self._last_alert = now
                            self._alerting = True
                            self._log(f'Notification shown: {subject}')
                        else:
                            # still set last alert to avoid spamming; log fallback
                            self._last_alert = now
                            self._alerting = True
                            self._log(f'Notification failed (fallback): {msg}')
                    else:
                        self._log('Alert suppressed by rate limit')

            if self._alerting and cpu < clear_cpu and mem < clear_mem:
                self._alerting = False
                self._log('Recovered: metrics below clear thresholds')

            time.sleep(self.interval)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='System Monitor (GUI + Windows notifications)')
    parser.add_argument('--console', action='store_true', help='Run in console mode (no GUI)')
    parser.add_argument('--interval', type=int, default=1, help='Sampling interval in seconds')
    parser.add_argument('--cpu-thr', type=int, default=90, help='CPU percent threshold to alert')
    parser.add_argument('--mem-thr', type=int, default=70, help='Memory percent threshold to alert')
    parser.add_argument('--notify-interval', type=int, default=10, help='Notification repeat interval (seconds)')
    parser.add_argument('--ping-host', type=str, default='8.8.8.8', help='Host to ping for latency measurement')
    parser.add_argument('--enable-toast', action='store_true', help='Enable Windows toast notifications (opt-in; may be unstable)')
    args = parser.parse_args()
    # Apply opt-in toast flag
    try:
        ENABLE_TOASTS = bool(args.enable_toast)
    except Exception:
        ENABLE_TOASTS = False

    if not args.console and TK_AVAILABLE:
        root = tk.Tk()
        root.title('System Monitor')
        # Try set the Tk window icon. Prefer a bundled PNG (python.png) first
        # because it's the most portable and avoids ctypes/windows calls.
        try:
            base = os.path.dirname(__file__)
            png_path = os.path.join(base, 'python.png')
            ico_path = os.path.join(base, 'python.ico')
            used_icon = False
            if os.path.exists(png_path):
                try:
                    img = tk.PhotoImage(file=png_path)
                    root.iconphoto(False, img)
                    used_icon = True
                except Exception:
                    used_icon = False
            if not used_icon and os.path.exists(ico_path):
                try:
                    root.iconbitmap(ico_path)
                    used_icon = True
                except Exception:
                    used_icon = False
            # Last resort: try to set from the python executable icon (ctypes)
            if not used_icon:
                try:
                    _set_tk_window_icon_to_python(root)
                except Exception:
                    pass
        except Exception:
            pass

        frm = ttk.Frame(root, padding=10)
        frm.grid(row=0, column=0, sticky='nsew')

        ttk.Label(frm, text='CPU:').grid(row=0, column=0, sticky='w')
        cpu_var = tk.StringVar(value='--%')
        ttk.Label(frm, textvariable=cpu_var).grid(row=0, column=1, sticky='w')

        ttk.Label(frm, text='Memory:').grid(row=1, column=0, sticky='w')
        mem_var = tk.StringVar(value='--%')
        ttk.Label(frm, textvariable=mem_var).grid(row=1, column=1, sticky='w')

        ttk.Label(frm, text='Upload:').grid(row=2, column=0, sticky='w')
        net_up_var = tk.StringVar(value='-- KB/s')
        ttk.Label(frm, textvariable=net_up_var).grid(row=2, column=1, sticky='w')

        ttk.Label(frm, text='Download:').grid(row=3, column=0, sticky='w')
        net_down_var = tk.StringVar(value='-- KB/s')
        ttk.Label(frm, textvariable=net_down_var).grid(row=3, column=1, sticky='w')

        ttk.Label(frm, text='Ping:').grid(row=4, column=0, sticky='w')
        ping_var = tk.StringVar(value='-- ms')
        ttk.Label(frm, textvariable=ping_var).grid(row=4, column=1, sticky='w')

        thr_frame = ttk.LabelFrame(frm, text='Alert thresholds')
        thr_frame.grid(row=5, column=0, columnspan=2, pady=(8,0), sticky='ew')
        ttk.Label(thr_frame, text='CPU %:').grid(row=0, column=0)
        cpu_thr = tk.IntVar(value=90)
        ttk.Entry(thr_frame, textvariable=cpu_thr, width=6).grid(row=0, column=1)
        ttk.Label(thr_frame, text='Mem %:').grid(row=0, column=2)
        mem_thr = tk.IntVar(value=70)
        ttk.Entry(thr_frame, textvariable=mem_thr, width=6).grid(row=0, column=3)

        # SMTP UI removed — using Windows notifications only

        log = tk.Text(frm, height=10, width=80)
        log.grid(row=6, column=0, columnspan=2, pady=(8,0))

        def ui_log(line):
            ts = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
            log.insert('end', f'[{ts}] {line}\n')
            log.see('end')

        def ui_update(info):
                    cpu_var.set(f"{info['cpu_percent']:.1f}%")
                    mem_pct = info.get('memory_percent', info['memory'].percent)
                    mem_pct = min(max(mem_pct or 0.0, 0.0), 100.0)
                    mem_var.set(f"{mem_pct:.1f}%")
                    # update instantaneous network speeds and ping
                    up_bps = info.get('net_upload_bps')
                    down_bps = info.get('net_download_bps')
                    if up_bps is not None:
                        net_up_var.set(f"{up_bps/1024:.2f} KB/s")
                    else:
                        net_up_var.set('-- KB/s')
                    if down_bps is not None:
                        net_down_var.set(f"{down_bps/1024:.2f} KB/s")
                    else:
                        net_down_var.set('-- KB/s')
                    ping_ms = info.get('ping_ms')
                    if ping_ms is not None:
                        ping_var.set(f"{ping_ms:.0f} ms")
                    else:
                        ping_var.set('-- ms')

        monitor = ResourceMonitor(cpu_threshold=args.cpu_thr, mem_threshold=args.mem_thr, interval=args.interval, notify_interval_sec=args.notify_interval, ping_host=args.ping_host, on_update=ui_update, on_log=ui_log)

        def start():
            # Start monitor with current thresholds from UI
            monitor.cpu_threshold = int(cpu_thr.get())
            monitor.mem_threshold = int(mem_thr.get())
            monitor.start()

        def stop():
            monitor.stop()

        def open_task_manager():
            """Open Windows Task Manager and log the result to the UI log."""
            try:
                # 'taskmgr' should be available on Windows PATH
                subprocess.Popen(['taskmgr'])
                ui_log('Opened Task Manager')
            except Exception as e:
                ui_log(f'Failed to open Task Manager: {e}')

        btn_frame = ttk.Frame(frm)
        btn_frame.grid(row=7, column=0, columnspan=2, pady=(8,0))
        ttk.Button(btn_frame, text='Start', command=start).grid(row=0, column=0, padx=4)
        ttk.Button(btn_frame, text='Stop', command=stop).grid(row=0, column=1, padx=4)
        ttk.Button(btn_frame, text='Task Manager', command=open_task_manager).grid(row=0, column=2, padx=4)

        root.protocol('WM_DELETE_WINDOW', lambda: (monitor.stop(), root.destroy()))
        # Auto-start monitor on GUI open
        try:
            ui_log('Auto-starting monitor')
            start()
        except Exception:
            pass
        root.mainloop()
    else:
        try:
            while True:
                info = get_system_info_dict()
                print_system_info(info)
                cpu = info['cpu_percent']
                mem = info['memory'].percent
                if cpu >= args.cpu_thr or mem >= args.mem_thr:
                    print('[ALERT] High resource usage detected')
                time.sleep(max(1, args.interval))
        except KeyboardInterrupt:
            print('\n\nMonitoring stopped.')
