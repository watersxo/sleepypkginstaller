#pragma once

#include "common.h"

#define RPI_VERSION "1.09"

#define STATUS_MAX_TASKS 12
#define STATUS_LOG_LINES 10
#define STATUS_LOG_LEN 112

enum server_state {
	SERVER_STARTING,
	SERVER_WAITING_NETWORK,
	SERVER_LISTENING,
	SERVER_ERROR,
};

enum task_state {
	TASK_QUEUED,
	TASK_PREPARING,
	TASK_DOWNLOADING,
	TASK_STALLED,
	TASK_PAUSED,
	TASK_STOPPED,
	TASK_DONE,
	TASK_ERROR,
};

struct task_info {
	int task_id;
	char title[64];
	enum task_state state;
	uint64_t total;
	uint64_t transferred;
	uint64_t speed; /* bytes per second */
	uint32_t rest_sec;
	uint32_t bits;
	int error_code;
	int preparing_percent;
	int local_copy_percent;
	bool user_paused;
	bool user_stopped;
	uint64_t last_transferred;
	int idle_ticks;
	int fail_ticks;
};

struct status_snapshot {
	char ip[16];
	int port;
	enum server_state server_state;
	char server_msg[96];
	uint32_t uptime_sec;
	uint64_t requests;
	char last_client[46];
	int task_count;
	struct task_info tasks[STATUS_MAX_TASKS];
	int log_count;
	char log[STATUS_LOG_LINES][STATUS_LOG_LEN]; /* oldest first */
	uint32_t generation;
};

void status_init(void);
void status_fini(void);

/* Starts the thread that polls BGFT for the progress of tracked tasks. */
bool status_start_monitor(void);

void status_set_server(enum server_state state, const char* ip, int port, const char* msg);
void status_get_endpoint(char* ip, size_t ip_size, int* port);

void status_log(const char* fmt, ...);
void status_note_request(const char* client_addr);

void status_task_add(int task_id, const char* title, uint64_t total);
void status_task_mark(int task_id, bool paused, bool stopped);
void status_task_remove(int task_id);

void status_get_snapshot(struct status_snapshot* out);

const char* status_task_state_name(enum task_state state);
const char* status_server_state_name(enum server_state state);

/* Writes the whole status as JSON into buf. Returns the length (truncated to buf_size - 1). */
size_t status_to_json(char* buf, size_t buf_size);
