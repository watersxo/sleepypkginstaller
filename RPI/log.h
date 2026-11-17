#pragma once

#include "common.h"

/* Persistent log written to /data/sleepy-installer.log (rotated to .old). Every line is written straight
 * to the file, so the last lines survive a crash and show what the app was doing when it died. */

void log_open(void);
void log_close_clean(void);

void log_write(const char* fmt, ...);

/* Copies the end of the log file into buf (NUL terminated). Returns the number of bytes. */
size_t log_read_tail(char* buf, size_t cap);

/* Gives the calling thread its own signal stack so that a stack overflow can still be reported.
 * Pass the returned value to log_thread_end() before the thread exits. */
void* log_thread_begin(void);
void log_thread_end(void* token);

/* Remembers an address so a crash address can be matched to the code that was running. */
void log_set_anchor(const char* name, const void* addr);
