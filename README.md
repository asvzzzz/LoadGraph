# LoadGraph

Real-time system resource monitor tailored for visual oversight of local LLM workloads. Windows 10/11 + Linux (Ubuntu 22.04 / Linux Mint). One graph per metric, single window, no tabs, no scrollbar.

## Why this exists

Standard GPU monitoring tools fall short when you need to watch multiple cards at once — exactly the situation when running local LLMs:

| Tool | Problem |
|---|---|
| **Windows Task Manager** | Shows only the first GPU. The second card shows incorrect GPU Load value.|
| **NVIDIA App** | Separate tab per GPU. You have to switch views to see the other card. Simultaneous multi-GPU view is impossible. |
| **nvidia-smi** | Console text. Fine for scripts, useless for visual monitoring of LLM inference. |
| **HWInfo / MSI Afterburner** | Heavy utilities full of noise. Afterburner overlay covers your LLM window. |

LoadGraph solves this: **all metrics for all GPUs are visible at once in a single window**, alongside CPU and RAM. Full picture, no switching.

## What it shows

- **CPU Load (%)** — overall processor utilization (`/proc/stat` delta-method on Linux, PDH `\\Processor(_Total)\% Processor Time` on Windows)
- **RAM Usage (GB)** — used memory with adaptive Y-axis
- **GPU Load (%)** — per-GPU utilization (RTX 5060 Ti #0, RTX 5060 Ti #1, …)
- **VRAM (GB)** — video memory used on each card
- **Temperature (°C)** — GPU temperature; red alarm line above 80°C
- **Fan (%)** — fan speed (when sensor is available)

## Screenshots

### Windows (dark theme, 2x RTX 5060 Ti)

![Windows](win_scr.png)

### Linux / WSL2

Works on native Linux and inside WSL2 thanks to the GPU-PV bridge (NVML exposed at `/usr/lib/wsl/lib/libnvidia-ml.so.1`). NVAPI is Windows-only; Linux uses NVML exclusively.

![Linux](lin_src.png)

## Requirements

### Windows

- Visual Studio 2022 (MSVC) or MinGW-w64
- wxWidgets 3.2 static build (`C:/wxWidgets`). Override with: `-DWX_CONFIG_PATH=...`
- NVIDIA driver (for GPU metrics; CPU/RAM work without a GPU)

### Linux

- gcc 13+ or clang
- wxWidgets 3.2: `sudo apt install libwxgtk3.2-dev` (or set `$WX_CONFIG`)
- NVIDIA driver + `libnvidia-ml` for GPU metrics (`nvidia-smi` must work)
- WSL2: GPU-PV bridge exposes NVML automatically, no extra setup needed

## Building

### Windows (MSVC)

```powershell
cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
.\bin\Release\LoadGraph.exe
```

MinGW:

```powershell
cmake .. -G "MinGW Makefiles"
cmake --build .
```

### Linux

```bash
mkdir -p build_linux && cd build_linux
cmake ..
make -j$(nproc)
./bin/LoadGraph
```

Override wx-config path:

```bash
WX_CONFIG=/opt/wx/3.2/bin/wx-config cmake ..
```

## Architecture

```
src/
  main.cpp                  # entry point, platform guards, SIGSEGV handler (Linux)
  main_frame.{h,cpp}        # window: header + scrollable graph rows
  graph_panel.{h,cpp}       # custom wxPanel, rendering via wxGraphicsContext
  metrics_collector.{h,cpp} # base class: poll thread, 1000 ms interval
  cpu_monitor.{h,cpp}       # Windows — PDH
  cpu_monitor_linux.cpp     # Linux — /proc/stat (delta-method)
  ram_monitor.{h,cpp}       # Windows — GlobalMemoryStatusEx
  ram_monitor_linux.cpp     # Linux — /proc/meminfo
  gpu_monitor.{h,cpp}       # Windows — NVML + NVAPI fallback
  gpu_monitor_linux.cpp     # Linux — dlopen libnvidia-ml.so.1 (real symbol names)
  theme.{h,cpp}             # Windows — Registry AppsUseLightTheme
  theme_linux.cpp           # Linux — GTK_THEME, XDG_CURRENT_DESKTOP, settings.ini
  config.{h,cpp}            # Windows — Registry
  config_win.cpp / config_linux.cpp
  logger.{h,cpp}            # file logger (loadgraph.log)
  platform.h                # PLATFORM_WINDOWS / PLATFORM_LINUX, ReadFileToString
```

## Known limitations

- **AMD / Intel GPU** — not supported (NVML is NVIDIA-only).
- **macOS** — not a target platform.

## License

MIT
