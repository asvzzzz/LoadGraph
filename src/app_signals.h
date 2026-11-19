#ifndef APP_SIGNALS_H
#define APP_SIGNALS_H

#include "platform.h"

// POSIX shutdown-request bridge.
//
// The application is a GUI program, so it must never terminate from a signal
// handler: sensor threads would leak, the config file would not be saved and
// the NVML/ADL contexts would not be released. Instead the signal handler only
// records the signal number, and the GUI thread turns it into a normal window
// close, which runs the regular OnClose() cleanup path.
//
// On platforms without POSIX signals (Windows GUI subsystem has no console to
// deliver SIGINT/SIGTERM) both functions are empty inline no-ops.

#ifdef PLATFORM_LINUX

// Installs handlers for SIGINT, SIGTERM and SIGHUP that request a graceful
// shutdown. Other signals (SIGSEGV and friends) keep their own handlers.
void InstallShutdownSignals();

// Returns the recorded shutdown signal, or 0 when none was received.
// Consumes the request, so a repeated call returns 0.
int PollShutdownSignal();

#else

// No-op on Windows: the caption button and the Exit menu are the only close
// requests there, both handled inside the event loop.
inline void InstallShutdownSignals() {}
inline int  PollShutdownSignal() { return 0; }

#endif

#endif // APP_SIGNALS_H
