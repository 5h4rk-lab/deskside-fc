// One-line serial logging with a replayable copy of the boot log.
//
// The native USB serial port only delivers data once the PC has the port
// open, so early lines can be lost if the monitor starts late. Every line
// logged before bringupLogFreeze() is also kept in PSRAM and can be printed
// again by sending 'r' in the serial monitor (see main.cpp).
#pragma once

void bringupLogBegin();
void logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void bringupLogFreeze();   // stop recording (call after the summary line)
void bringupLogReplay();   // print the recorded boot log again
