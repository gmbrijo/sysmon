# System Monitor

A small Python system monitor that displays CPU, memory, disk, and network
information and shows Windows toast notifications when thresholds are exceeded.

Features
- Tkinter GUI with live CPU/memory and instantaneous network upload/download speeds
- Ping measurement (ms)
- Windows toast notifications via `win10toast` (falls back to console logging if unavailable)
- Console mode for use without a GUI

Quick start
1. Create and activate a virtual environment (Windows PowerShell):

```powershell
python -m venv .venv
.\.venv\Scripts\Activate.ps1
```

2. Install dependencies:

```powershell
pip install -r requirements.txt
```

3. Run GUI (if tkinter is available):

```powershell
python sysmon.py
```

4. Run console mode:

```powershell
python sysmon.py --console
```

CLI options
- `--interval`: sampling interval in seconds (default 1)
- `--cpu-thr`: CPU percent threshold to alert (default 90)
- `--mem-thr`: Memory percent threshold to alert (default 70)
- `--notify-interval`: repeat notification interval in seconds (default 10)
- `--ping-host`: host to ping for latency measurement (default 8.8.8.8)

Notes
- If you don't need Windows toast notifications, you can skip installing `win10toast`; notifications will be printed to console/log instead.
- Running the GUI requires a desktop session.
