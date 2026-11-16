#pragma once

#include "common.h"

/* Starts the on-screen status display in its own thread. Failure is not fatal. */
bool screen_start(void);
void screen_stop(void);
