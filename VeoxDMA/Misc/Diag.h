#pragma once
// Crash/behaviour trail shared by every module. Lines are appended to
// veox-diag.log beside the exe and echoed to the console.
//
// Implemented in Main.cpp so there is one writer and one mutex.

void VeoxDiag(const char* fmt, ...);

// Names the stage the process is in, so a fault line can say where it died.
void VeoxSetStage(const char* stage);

// Set true to bring back the one-shot memory probes used to find the cargo and
// item layouts. Off by default so the console stays clean.
inline constexpr bool kProbes = false;
