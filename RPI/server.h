#pragma once

#include "common.h"

bool server_start(int port, const char* work_dir);
bool server_listen(void);
void server_stop(void);
/* Asks server_listen() to return (safe to call from a signal handler). */
void server_request_stop(void);
