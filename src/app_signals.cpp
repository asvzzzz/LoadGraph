// app_signals.h includes platform.h, so PLATFORM_LINUX is already known here.
#include "app_signals.h"

#ifdef PLATFORM_LINUX

#include <atomic>
#include <csignal>

// Written by the signal handler, read by the GUI thread. std::atomic<int> is
// always lock-free, so a plain store is async-signal-safe and there is no data
// race with the polling reader (acquire/release pairing).
static std::atomic<int> g_shutdownSignal(0);

// Async-signal-safe: no allocation, no stdio, no wxWidgets/GTK calls.
static void ShutdownSignalHandler(int sig)
{
    g_shutdownSignal.store(sig, std::memory_order_release);
}

void InstallShutdownSignals()
{
    // SIGINT  - Ctrl+C in the terminal that started the GUI;
    // SIGTERM - "pkill LoadGraphGUI", systemd/weston teardown;
    // SIGHUP  - controlling terminal or WSL session closed.
    //
    // A non-default handler is also what makes "kill <pid>" a graceful close:
    // without it the kernel terminates the process immediately and the cleanup
    // in MainFrame::OnClose() never runs.
    if (std::signal(SIGINT,  ShutdownSignalHandler) == SIG_ERR) return;
    if (std::signal(SIGTERM, ShutdownSignalHandler) == SIG_ERR) return;
    std::signal(SIGHUP, ShutdownSignalHandler);
}

int PollShutdownSignal()
{
    return g_shutdownSignal.exchange(0, std::memory_order_acq_rel);
}

#endif // PLATFORM_LINUX
