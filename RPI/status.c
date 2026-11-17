#include "status.h"
#include "installer.h"
#include "net.h"
#include "log.h"
#include "sandbird.h"

#include <pthread.h>
#include <time.h>
#include <orbis/libkernel.h>

#define MONITOR_STACK_SIZE (256 * 1024)
#define IP_REFRESH_TICKS 5
#define STALL_TICKS 5
#define MAX_FAIL_TICKS 3
#define HEARTBEAT_TICKS 30

static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;
static struct status_snapshot s_state;
static time_t s_start_time;
static bool s_monitor_running = false;
static pthread_t s_monitor_thread;
static bool s_monitor_started = false;

static void log_line_locked(const char* msg) {
	uint32_t up = (uint32_t)(time(NULL) - s_start_time);

	log_write("%s", msg);

	if (s_state.log_count == STATUS_LOG_LINES) {
		memmove(s_state.log[0], s_state.log[1], sizeof(s_state.log[0]) * (STATUS_LOG_LINES - 1));
		s_state.log_count--;
	}
	snprintf(s_state.log[s_state.log_count], STATUS_LOG_LEN, "[%02u:%02u] %s", (up / 60) % 100, up % 60, msg);
	s_state.log_count++;
	s_state.generation++;
}

static struct task_info* find_task_locked(int task_id) {
	int i;

	for (i = 0; i < s_state.task_count; ++i) {
		if (s_state.tasks[i].task_id == task_id) {
			return &s_state.tasks[i];
		}
	}
	return NULL;
}

const char* status_task_state_name(enum task_state state) {
	switch (state) {
		case TASK_QUEUED: return "Queued";
		case TASK_PREPARING: return "Preparing";
		case TASK_DOWNLOADING: return "Downloading";
		case TASK_STALLED: return "Waiting";
		case TASK_PAUSED: return "Paused";
		case TASK_STOPPED: return "Stopped";
		case TASK_DONE: return "Complete";
		case TASK_ERROR: return "Error";
	}
	return "Unknown";
}

const char* status_server_state_name(enum server_state state) {
	switch (state) {
		case SERVER_STARTING: return "Starting";
		case SERVER_WAITING_NETWORK: return "Waiting for network";
		case SERVER_LISTENING: return "Listening";
		case SERVER_ERROR: return "Error";
	}
	return "Unknown";
}

void status_init(void) {
	pthread_mutex_lock(&s_mtx);
	memset(&s_state, 0, sizeof(s_state));
	s_state.server_state = SERVER_STARTING;
	s_start_time = time(NULL);
	pthread_mutex_unlock(&s_mtx);
}

void status_set_server(enum server_state state, const char* ip, int port, const char* msg) {
	pthread_mutex_lock(&s_mtx);
	s_state.server_state = state;
	if (ip) {
		strlcpy(s_state.ip, ip, sizeof(s_state.ip));
	}
	if (port > 0) {
		s_state.port = port;
	}
	strlcpy(s_state.server_msg, msg ? msg : "", sizeof(s_state.server_msg));
	s_state.generation++;
	pthread_mutex_unlock(&s_mtx);
}

void status_get_endpoint(char* ip, size_t ip_size, int* port) {
	pthread_mutex_lock(&s_mtx);
	if (ip && ip_size > 0) {
		strlcpy(ip, s_state.ip, ip_size);
	}
	if (port) {
		*port = s_state.port;
	}
	pthread_mutex_unlock(&s_mtx);
}

void status_log(const char* fmt, ...) {
	char msg[STATUS_LOG_LEN];
	va_list args;

	va_start(args, fmt);
	vsnprintf(msg, sizeof(msg) - 8, fmt, args);
	va_end(args);

	pthread_mutex_lock(&s_mtx);
	log_line_locked(msg);
	pthread_mutex_unlock(&s_mtx);
}

void status_note_request(const char* client_addr) {
	pthread_mutex_lock(&s_mtx);
	s_state.requests++;
	if (client_addr && *client_addr) {
		strlcpy(s_state.last_client, client_addr, sizeof(s_state.last_client));
	}
	s_state.generation++;
	pthread_mutex_unlock(&s_mtx);
}

void status_task_add(int task_id, const char* title, uint64_t total) {
	struct task_info* t;
	char msg[STATUS_LOG_LEN];

	if (task_id < 0) {
		return;
	}

	pthread_mutex_lock(&s_mtx);
	t = find_task_locked(task_id);
	if (!t) {
		if (s_state.task_count == STATUS_MAX_TASKS) {
			/* Drop the oldest finished task, or the oldest one when none is finished. */
			int victim = 0, i;
			for (i = 0; i < s_state.task_count; ++i) {
				if (s_state.tasks[i].state == TASK_DONE || s_state.tasks[i].state == TASK_ERROR) {
					victim = i;
					break;
				}
			}
			memmove(&s_state.tasks[victim], &s_state.tasks[victim + 1], sizeof(s_state.tasks[0]) * (s_state.task_count - victim - 1));
			s_state.task_count--;
		}
		t = &s_state.tasks[s_state.task_count++];
	}
	memset(t, 0, sizeof(*t));
	t->task_id = task_id;
	strlcpy(t->title, title ? title : "(unknown)", sizeof(t->title));
	t->total = total;
	t->state = TASK_QUEUED;

	snprintf(msg, sizeof(msg), "Task %d registered: %.60s", task_id, t->title);
	log_line_locked(msg);
	pthread_mutex_unlock(&s_mtx);
}

void status_task_mark(int task_id, bool paused, bool stopped) {
	struct task_info* t;

	pthread_mutex_lock(&s_mtx);
	t = find_task_locked(task_id);
	if (t) {
		t->user_paused = paused;
		t->user_stopped = stopped;
		if (paused) {
			t->state = TASK_PAUSED;
		} else if (stopped) {
			t->state = TASK_STOPPED;
		} else if (t->state == TASK_PAUSED || t->state == TASK_STOPPED) {
			t->state = TASK_QUEUED;
		}
		s_state.generation++;
	}
	pthread_mutex_unlock(&s_mtx);
}

void status_task_remove(int task_id) {
	int i;
	char msg[STATUS_LOG_LEN];

	pthread_mutex_lock(&s_mtx);
	for (i = 0; i < s_state.task_count; ++i) {
		if (s_state.tasks[i].task_id == task_id) {
			memmove(&s_state.tasks[i], &s_state.tasks[i + 1], sizeof(s_state.tasks[0]) * (s_state.task_count - i - 1));
			s_state.task_count--;
			snprintf(msg, sizeof(msg), "Task %d removed", task_id);
			log_line_locked(msg);
			break;
		}
	}
	pthread_mutex_unlock(&s_mtx);
}

void status_get_snapshot(struct status_snapshot* out) {
	pthread_mutex_lock(&s_mtx);
	memcpy(out, &s_state, sizeof(*out));
	out->uptime_sec = (uint32_t)(time(NULL) - s_start_time);
	pthread_mutex_unlock(&s_mtx);
}

static void apply_progress(struct task_info* t, const struct bgft_download_task_progress_info* p) {
	uint64_t total = p->length_total ? p->length_total : p->length;
	uint64_t done = p->length_total ? p->transferred_total : p->transferred;
	char msg[STATUS_LOG_LEN];

	t->fail_ticks = 0;
	t->bits = p->bits;
	t->rest_sec = p->rest_sec_total ? p->rest_sec_total : p->rest_sec;
	t->preparing_percent = p->preparing_percent;
	t->local_copy_percent = p->local_copy_percent;
	if (total > t->total || t->total == 0) {
		t->total = total;
	}
	if (done >= t->transferred) {
		t->speed = (t->speed * 2 + (done - t->last_transferred)) / 3;
	}
	if (done == t->last_transferred) {
		t->idle_ticks++;
	} else {
		t->idle_ticks = 0;
	}
	t->last_transferred = t->transferred = done;

	if (p->error_result != 0) {
		if (t->state != TASK_ERROR) {
			snprintf(msg, sizeof(msg), "Task %d error 0x%08X", t->task_id, (unsigned int)p->error_result);
			log_line_locked(msg);
		}
		t->error_code = p->error_result;
		t->state = TASK_ERROR;
		t->speed = 0;
		return;
	}
	t->error_code = 0;

	if (t->user_stopped) {
		t->state = TASK_STOPPED;
		t->speed = 0;
	} else if (t->user_paused) {
		t->state = TASK_PAUSED;
		t->speed = 0;
	} else if (t->total > 0 && (t->transferred >= t->total ||
	           (t->transferred * 100 / t->total >= 97 && t->idle_ticks >= STALL_TICKS))) {
		/* Patches stop just short of 100% while the system installs them on its own. */
		if (t->state != TASK_DONE) {
			snprintf(msg, sizeof(msg), "Task %d download complete: %.60s", t->task_id, t->title);
			log_line_locked(msg);
		}
		t->state = TASK_DONE;
		t->speed = 0;
	} else if (t->transferred == 0 && t->preparing_percent > 0 && t->preparing_percent < 100) {
		t->state = TASK_PREPARING;
	} else if (t->transferred == 0) {
		t->state = TASK_QUEUED;
	} else if (t->idle_ticks >= STALL_TICKS) {
		t->state = TASK_STALLED;
		t->speed = 0;
	} else {
		t->state = TASK_DOWNLOADING;
	}
}

static void poll_tasks(void) {
	int ids[STATUS_MAX_TASKS];
	int count = 0, i, ret;
	struct bgft_download_task_progress_info progress;
	struct task_info* t;
	char msg[STATUS_LOG_LEN];

	pthread_mutex_lock(&s_mtx);
	for (i = 0; i < s_state.task_count; ++i) {
		if (s_state.tasks[i].state != TASK_DONE) {
			ids[count++] = s_state.tasks[i].task_id;
		}
	}
	pthread_mutex_unlock(&s_mtx);

	for (i = 0; i < count; ++i) {
		ret = 0;
		memset(&progress, 0, sizeof(progress));
		bool ok = bgft_download_get_task_progress(ids[i], &progress, &ret);

		pthread_mutex_lock(&s_mtx);
		t = find_task_locked(ids[i]);
		if (t) {
			if (ok) {
				apply_progress(t, &progress);
			} else if (++t->fail_ticks == MAX_FAIL_TICKS) {
				if (t->total > 0 && t->transferred * 100 / t->total >= 95) {
					/* the system removes the task once it has taken over the install */
					t->state = TASK_DONE;
				} else {
					t->state = TASK_ERROR;
					t->error_code = ret;
				}
				t->speed = 0;
				snprintf(msg, sizeof(msg), "Task %d unavailable (0x%08X)", t->task_id, (unsigned int)ret);
				log_line_locked(msg);
			}
			s_state.generation++;
		}
		pthread_mutex_unlock(&s_mtx);
	}
}

static void refresh_ip(void) {
	char ip[16];
	char msg[STATUS_LOG_LEN];

	memset(ip, 0, sizeof(ip));
	if (net_get_ipv4(ip, sizeof(ip)) != 0 || ip[0] == '\0') {
		return;
	}

	pthread_mutex_lock(&s_mtx);
	if (strcmp(ip, s_state.ip) != 0) {
		snprintf(msg, sizeof(msg), "IP address changed to %s", ip);
		strlcpy(s_state.ip, ip, sizeof(s_state.ip));
		log_line_locked(msg);
	}
	pthread_mutex_unlock(&s_mtx);
}

static void log_heartbeat(void) {
	struct status_snapshot snap;
	int i;

	status_get_snapshot(&snap);
	log_write("heartbeat: conns=%d requests=%llu tasks=%d", sb_active_connections(), (unsigned long long)snap.requests, snap.task_count);
	for (i = 0; i < snap.task_count; ++i) {
		const struct task_info* t = &snap.tasks[i];
		log_write("  task %d %s %llu/%llu bits=0x%x err=0x%x idle=%d", t->task_id, status_task_state_name(t->state), (unsigned long long)t->transferred, (unsigned long long)t->total, t->bits, (unsigned)t->error_code, t->idle_ticks);
	}
}

static void* monitor_thread(void* arg) {
	int tick = 0;
	int slept;
	void* altstack = log_thread_begin();

	(void)arg;

	while (__atomic_load_n(&s_monitor_running, __ATOMIC_SEQ_CST)) {
		/* Sleep in short slices so shutting down never waits a full second. */
		for (slept = 0; slept < 10 && __atomic_load_n(&s_monitor_running, __ATOMIC_SEQ_CST); ++slept) {
			sceKernelUsleep(100 * 1000);
		}
		if (!__atomic_load_n(&s_monitor_running, __ATOMIC_SEQ_CST)) {
			break;
		}

		poll_tasks();
		if (++tick % IP_REFRESH_TICKS == 0) {
			refresh_ip();
		}
		if (tick % HEARTBEAT_TICKS == 0) {
			log_heartbeat();
		}
	}

	log_thread_end(altstack);
	return NULL;
}

bool status_start_monitor(void) {
	pthread_attr_t attr;
	int ret;

	if (s_monitor_started) {
		return true;
	}

	__atomic_store_n(&s_monitor_running, true, __ATOMIC_SEQ_CST);

	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, MONITOR_STACK_SIZE);
	ret = pthread_create(&s_monitor_thread, &attr, &monitor_thread, NULL);
	pthread_attr_destroy(&attr);
	if (ret) {
		EPRINTF("Unable to start status monitor: %d\n", ret);
		__atomic_store_n(&s_monitor_running, false, __ATOMIC_SEQ_CST);
		return false;
	}

	s_monitor_started = true;
	return true;
}

/* Stops the monitor and waits for it, so BGFT and the network can be shut down safely afterwards. */
void status_fini(void) {
	if (!s_monitor_started) {
		return;
	}

	__atomic_store_n(&s_monitor_running, false, __ATOMIC_SEQ_CST);
	pthread_join(s_monitor_thread, NULL);
	s_monitor_started = false;
}

static size_t json_escape(char* dst, size_t dst_size, const char* src) {
	size_t n = 0;

	while (*src && n + 7 < dst_size) {
		unsigned char c = (unsigned char)*src++;
		if (c == '"' || c == '\\') {
			dst[n++] = '\\';
			dst[n++] = (char)c;
		} else if (c < 0x20) {
			n += snprintf(dst + n, dst_size - n, "\\u%04x", c);
		} else {
			dst[n++] = (char)c;
		}
	}
	dst[n] = '\0';
	return n;
}

size_t status_to_json(char* buf, size_t buf_size) {
	struct status_snapshot snap;
	char esc[STATUS_LOG_LEN * 2];
	size_t n = 0;
	int i;

	status_get_snapshot(&snap);

#define APPEND(...) \
	do { \
		if (n < buf_size) { \
			int w = snprintf(buf + n, buf_size - n, __VA_ARGS__); \
			if (w > 0) n += (size_t)w; \
		} \
	} while (0)

	json_escape(esc, sizeof(esc), snap.server_msg);
	APPEND("{ \"status\": \"success\", \"ip\": \"%s\", \"port\": %d, \"server_state\": \"%s\", \"message\": \"%s\", \"uptime\": %u, \"requests\": %" PRIu64 ", \"tasks\": [",
		snap.ip, snap.port, status_server_state_name(snap.server_state), esc, (unsigned)snap.uptime_sec, snap.requests);

	for (i = 0; i < snap.task_count; ++i) {
		const struct task_info* t = &snap.tasks[i];
		json_escape(esc, sizeof(esc), t->title);
		APPEND("%s{ \"task_id\": %d, \"title\": \"%s\", \"state\": \"%s\", \"transferred\": %" PRIu64 ", \"total\": %" PRIu64 ", \"percent\": %d, \"speed\": %" PRIu64 ", \"rest_sec\": %u, \"error_code\": %u }",
			i ? ", " : " ", t->task_id, esc, status_task_state_name(t->state), t->transferred, t->total,
			t->total ? (int)(t->transferred * 100 / t->total) : 0, t->speed, (unsigned)t->rest_sec, (unsigned)t->error_code);
	}

	APPEND(" ], \"log\": [");
	for (i = 0; i < snap.log_count; ++i) {
		json_escape(esc, sizeof(esc), snap.log[i]);
		APPEND("%s\"%s\"", i ? ", " : " ", esc);
	}
	APPEND(" ] }\n");

#undef APPEND

	if (n >= buf_size) {
		n = buf_size - 1;
	}
	return n;
}
