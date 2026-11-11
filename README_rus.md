# LoadGraph

Монитор системных ресурсов в реальном времени, созданный для визуального контроля локальных LLM-нагрузок. Windows 10/11 + Linux (Ubuntu 22.04 / Linux Mint). Одна диаграмма на метрику, одно окно, без вкладок и полосы прокрутки.

[English version](README.md)

**Версия:** 1.0.0

## Скачать

Предсобранный 64-битный Windows-бинарь доступен по адресу [andyk.ru/files/LoadGraph.zip](https://andyk.ru/files/LoadGraph.zip). Распакуйте и запустите `LoadGraph.exe` — установка не требуется. Для мониторинга температуры CPU поместите совместимый с WinRing0 файл `.sys` рядом с исполняемым (см. [Драйвер](#драйвер)).

## Зачем это нужно

Стандартные утилиты мониторинга GPU не подходят, когда нужно одновременно следить за несколькими картами — а именно такая ситуация возникает при запуске локальных LLM:

| Утилита | Проблема |
|---|---|
| **Диспетчер задач Windows** | Показывает только первую GPU. На второй карте значение GPU Load неверное.|
| **NVIDIA App** | Отдельная вкладка на каждую GPU. Нельзя видеть все карты одновременно. |
| **nvidia-smi** | Консольный текст. Годится для скриптов, бесполезен для визуального мониторинга LLM-инференса. |
| **HWInfo / MSI Afterburner** | Тяжёлые утилиты с шумом. Оверлей Afterburner перекрывает окно с LLM. |

LoadGraph решает эту проблему: **все метрики всех GPU видны одновременно в одном окне**, вместе с CPU и RAM. Полная картина, без переключения.

## Что показывает

- **CPU Load (%)** — общая загрузка процессора (дельта-метод `/proc/stat` на Linux, PDH `\\Processor(_Total)\\% Processor Time` на Windows)
- **RAM Usage (GB)** — использованная память с адаптивной осью Y
- **GPU Load (%)** — загрузка каждой GPU (один раздел на каждую обнаруженную NVIDIA карту; каждый раздел содержит графики Load, VRAM, Temperature, Fan и Power)
- **VRAM (GB)** — объём видеопамяти на каждой карте
- **Temperature (°C)** — температура GPU; температура CPU на Windows через MSR (IA32_THERM_STATUS 0x19C, Tjmax по семейству: 84 для Haswell-E/EP, 85 для Broadwell-EP, 100 для Skylake и новее); красная линия тревоги выше 80°C
- **Fan (% / RPM)** — скорость вентилятора GPU в процентах (или fallback в RPM), вентилятор CPU в RPM (когда датчик доступен)
- **GPU Power (W)** — энергопотребление каждой карты через NVML `nvmlDeviceGetPowerUsage`
- **CPU Power (W)** — реальный счётчик RAPL на Linux, или WMI-сенсор LHM/OpenHardwareMonitor на Windows, или оценка idle-floor как последний резерв
- **Total Power (W)** — сумма CPU + всех GPU; фиксированная шкала 0..1000 W с 5 делениями
- **Uptime** — время работы системы с момента загрузки, отображается как `YY:MM:DD HH:MI:SS` или `<N> s` для свежих перезагрузок

## Скриншоты

### Windows

![Windows](img/lgwin.png)

### Linux / WSL2

Работает на нативном Linux и внутри WSL2 благодаря GPU-PV bridge (NVML доступен по `/usr/lib/wsl/lib/libnvidia-ml.so.1`). NVAPI — только для Windows; на Linux используется исключительно NVML.

![Linux](img/lglin.png)

### Веб-дашборд (HTTP-сервер)

Включите `"http_enabled": true` в `loadgraph.cfg`. Сервер запускается на заданном порту (по умолчанию `9115`) в отдельном потоке:

- **`GET /`** — адаптивный HTML-дашборд с живыми графиками, повторяет компоновку GUI. Чистый vanilla JS + Canvas, без зависимостей.
- **`GET /api/metrics`** — JSON-эндпоинт со всеми текущими метриками (CPU загрузка/температура/мощность, RAM, данные по каждой GPU, накопители).
- **`GET /favicon.ico`** — встроенный фавикон.

Работает на Windows и Linux. Дашборд обновляется каждую секунду через polling.

![Веб-дашборд](img/loadgraph_http.png)

## Требования

### Windows

- Visual Studio 2022 (MSVC) — обязателен на Windows (используются SEH-расширения и флаги компилятора MSVC; MinGW не поддерживается)
- wxWidgets 3.2 статическая сборка (`C:/wxWidgets`). Переопределить: `-DWX_CONFIG_PATH=...`
- Драйвер NVIDIA (для метрик GPU; CPU/RAM работают без GPU)
- **Температура CPU** требует одного из:
  - Ядерного драйвера, совместимого с WinRing0 (`.sys`), размещённого рядом с исполняемым файлом — см. [Драйвер](#драйвер) ниже
  - Или прямого доступа `rdmsr` (работает на некоторых системах, где HVCI / Integrity памяти выключен и CPU не блокирует пользовательское чтение MSR)

### Linux

- gcc 13+ или clang
- wxWidgets 3.2: `sudo apt install libwxgtk3.2-dev` (или установите `$WX_CONFIG`)
- Драйвер NVIDIA + `libnvidia-ml` для метрик GPU (`nvidia-smi` должен работать)
- WSL2: GPU-PV bridge автоматически предоставляет NVML, дополнительная настройка не нужна

## Драйвер

Температура CPU на Windows считывается из IA32_THERM_STATUS (MSR 0x19C). Для этого требуется драйвер ядра, потому что пользовательский `rdmsr` по умолчанию заблокирован на современных процессорах Intel.

### Распространение драйвера

Перед распространением поместите файл `.sys`, совместимый с WinRing0, рядом с исполняемым файлом. Поддерживаемые имена (проверяются по порядку):

```
hwinterfacex64.sys
inpoutx64.sys
WinRing0x64.sys
```

Приложение также обнаружит любую из трёх уже зарегистрированных известных служб и использует их без необходимости в `.sys`-файле: `WinRing0_1_2_0` (RealTemp / LibreHardwareMonitor), `hwinterfacex64` или `inpoutx64`.

### Установка драйвера через UAC

Если бэкенд недоступен, но `.sys`-файл присутствует, LoadGraph показывает однократный запрос:

> «Установить ядерный драйвер для мониторинга температуры CPU?»

Принятие запроса повышает привилегии процесса через `ShellExecute(runas)`, запускает встроенную вспомогательную программу (`--install-driver`), которая регистрирует и запускает службу — оставляя её установленной. При типе `SERVICE_DEMAND_START` служба сохраняется в рамках текущей сессии без дополнительных запросов UAC, но первый запуск после перезагрузки снова потребует повышения привилегий. Установите службе `AUTO_START`, если хотите автоматическую загрузку при старте системы. Отказ или отмена gracefully переключает на WMI / N/A для температуры.

> **Примечание:** WinRing0 находится в списке уязвимых драйверов Microsoft. Windows Defender может автоматически удалить `.sys`-файл. Если это произойдёт, добавьте исключение для директории с LoadGraph в Параметры защиты Windows → Защита от вирусов и угроз → Управление настройками → Исключения. Это тот же обходной путь, который используют сообщества RealTemp и HWiNFO.

### Прямой fallback на rdmsr

На системах, где HVCI / Integrity памяти выключен и CPU не блокирует пользовательский доступ к MSR, LoadGraph использует `__readmsr()` без какого-либо драйвера. Температура будет показана корректно. Если встроенная функция вызовет исключение (нарушение привилегий), приложение молча переключится на WMI.

## Сборка

### Windows (MSVC)

```powershell
cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
.\bin\Release\LoadGraph.exe
```

### Linux

```bash
mkdir -p build_linux && cd build_linux
cmake ..
make -j$(nproc)
./bin/LoadGraph
```

Переопределить путь к wx-config:

```bash
WX_CONFIG=/opt/wx/3.2/bin/wx-config cmake ..
```

## Конфигурация

Все настройки хранятся в одном файле `loadgraph.cfg` формата JSON:

- **Windows**: рядом с исполняемым файлом (`.<директория exe>/loadgraph.cfg`)
- **Linux**: `$XDG_CONFIG_HOME/loadgraph/loadgraph.cfg` (по умолчанию `~/.config/loadgraph/loadgraph.cfg`)

```jsonc
{
  "http_port": 9115,
  "http_enabled": false,
  "log_file": false,
  // Позиция и размер окна — (0,0,0,0 = дать ОС решить)
  "window_x": 100,
  "window_y": 100,
  "window_w": 640,
  "window_h": 720,
  "colors": {
    "cpu_load": "76B900", "cpu_temp": "00bcd4", "cpu_power": "ffc107",
    "ram": "AEB4E1", "gpu_load": "e91e63", "gpu_temp": "00bcd4",
    "gpu_vram": "AEB4E1", "gpu_fan": "3154E1", "gpu_power": "ffc107",
    "total_power": "ff5722"
  },
  "backgrounds": {
    "page": "1a1a2e", "section": "16213e", "chart": "0f3460"
  },
  // Состояние видимости меню View (сохраняется между сессиями)
  "view_menu": { "cpu_load": true, "ram": true, "gpu_temp": false, ... }
}
```

## Архитектура

```
src/
  main.cpp                  # точка входа, платформенные guards, обработчик SIGSEGV (Linux),
                            # режим elevated helper (--install-driver), размещение crash.log
  main_frame.{h,cpp}        # окно: заголовок + пропорционально растянутые строки графиков,
                            # переключение через меню View
  graph_panel.{h,cpp}       # пользовательский wxPanel, отрисовка через wxGraphicsContext
  metrics_collector.{h,cpp} # базовый класс: поток опроса, интервал 1000 мс, остановка через
                            # condition_variable
  cpu_monitor.{h,cpp}       # Windows — PDH загрузка, WMI-зонд температуры, инициализация MSR,
                            # LHM/WMI сенсор мощности
  cpu_monitor_linux.cpp     # Linux — /proc/stat (дельта-метод), /sys/class/thermal, RAPL мощность
  ram_monitor.{h,cpp}       # Windows — GlobalMemoryStatusEx
  ram_monitor_linux.cpp     # Linux — /proc/meminfo
  gpu_monitor.{h,cpp}       # Windows — NVML основной + NVAPI fallback (GetDynamicPstates)
  gpu_monitor_linux.cpp     # Linux — dlopen libnvidia-ml.so.1 (реальные имена символов)
  ring0_msr.{h,cpp}         # WinRing0 DLL / ядерный драйвер бэкенд, помощники повышения UAC
  msr_sensor.{h,cpp}        # считыватель температуры MSR: декодирование Tjmax, запрос EnsureDriver
  power_table.{h,cpp}       # статическая таблица TDP для CPU Intel/AMD и GPU NVIDIA (match по подстроке)
  theme.{h,cpp}             # Windows — Registry AppsUseLightTheme, кэш тёмной палитры
  theme_linux.cpp           # Linux — GTK_THEME, XDG_CURRENT_DESKTOP, кэш gsettings на 30 с
  app_config.h/cpp          # структура AppConfig + парсер/эмиттер JSON для loadgraph.cfg
  http_server.{h,cpp}       # кроссплатформенный HTTP-сервер + HTML веб-дашборд
  logger.{h,cpp}            # файловый логгер (loadgraph.log в CWD; fallback в stderr при ошибке fopen)
  platform.h                # PLATFORM_WINDOWS / PLATFORM_LINUX, ReadFileToString, помощник mkdir -p
```

### Ключевые архитектурные решения

- **MetricsCollector** использует `condition_variable` для остановки — поток завершается за миллисекунды, а не за секунды.
- **SEH-защита** вокруг вызовов `rdmsr` и WMI предотвращает падения на системах, где эти API ведут себя нестабильно.
- **Единый файл конфигурации**: позиция/размер окна, состояние меню View, цвета, настройки HTTP — всё в одном `loadgraph.cfg`. Никаких отдельных файлов.
- **Жизненный цикл драйвера**: `Open()` сначала пробует DLL, затем ядерное устройство, затем известную службу, затем регистрирует из `.sys`. `Close()` *не* останавливает и не удаляет службы — драйвер остаётся установленным между запусками (служба типа DEMAND_START сохраняется без необходимости повторной установки; сам драйвер загружается при первом использовании).
- **Повышение UAC** выполняется однократно: вспомогательный процесс устанавливает/запускает службу и завершается; последующие запуски без повышения используют уже запущенную службу.
- **Таблица TDP GPU** сопоставляет по подстроке, причём более длинные шаблоны идут раньше (например, `"RTX A4000"` перед `"A40"`), чтобы избежать перекрытия.

## Известные ограничения

- **AMD / Intel GPU** — не поддерживаются (NVML только для NVIDIA).
- **macOS** — не является целевой платформой.
- **Температура CPU на Linux** — читается из `/sys/class/thermal/thermal_zone0/temp`; на многих настольных платах тепловая зона отсутствует, поэтому N/A — обычное дело.
- **HVCI / Integrity памяти** — если включён в Параметрах защиты Windows, пользовательский `rdmsr` заблокирован и требуется ядерный драйвер.
- **Список WinRing0** — Microsoft активно блокирует драйверы WinRing0 на Windows 11 22H2+. Defender может удалить `.sys`-файл; при необходимости добавьте исключение для директории LoadGraph.
- **Виртуализированные системы (Hyper-V / VM)** — температура CPU обычно показывает N/A. Стандартные WMI-классы тепловых зон (`MSAcpi_ThermalZoneTemperature`) недоступны из пользовательского режима гостевой ОС, а `rdmsr` блокируется гипервизором. NVIDIA App обходит это, запуская свой бэкенд (`nvcontainer.exe`) от имени LOCALSYSTEM, который имеет другие WMI-разрешения — воспроизведение этого паттерна требует повышения привилегий.

## Лицензия

MIT
