#include "log.h"

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <orbis/libkernel.h>

#define LOG_PATH "/data/sleepy-installer.log"
#define LOG_OLD_PATH "/data/sleepy-installer.log.old"
#define RUN_MARKER "/data/sleepy-installer.running"
#define LOG_MAX_SIZE (768 * 1024)
#define LOG_LINE_MAX 700
#define ALT_STACK_SIZE (64 * 1024)
#define MAX_ANCHORS 12

struct anchor {
	const char* name;
	uint64_t addr;
};

static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;
static bool s_enabled = false;
static size_t s_size = 0;
static int s_write_errors = 0;
static struct anchor s_anchors[MAX_ANCHORS];
static int s_anchor_count = 0;

/* The file is opened for every record instead of keeping a descriptor: a descriptor that is closed by mistake
 * elsewhere in the app (and then reused for a socket) would silently stop the log, or write into the wrong place. */
static ssize_t append_raw(const char* text, size_t len) {
	int fd;
	ssize_t w;

	fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0666);
	if (fd < 0) {
		return -1;
	}
	w = write(fd, text, len);
	close(fd);
	return w;
}

static void raw_line(const char* text, size_t len) {
	if (len > 0) {
		(void)append_raw(text, len);
	}
}

static void measure_file_locked(void) {
	int fd = open(LOG_PATH, O_RDONLY);
	off_t end;

	s_size = 0;
	if (fd < 0) {
		return;
	}
	end = lseek(fd, 0, SEEK_END);
	s_size = end > 0 ? (size_t)end : 0;
	close(fd);
}

static void rotate_locked(void) {
	unlink(LOG_OLD_PATH);
	rename(LOG_PATH, LOG_OLD_PATH);
	s_size = 0;
}

void log_write(const char* fmt, ...) {
	char line[LOG_LINE_MAX];
	uint64_t ms = sceKernelGetProcessTime() / 1000;
	va_list args;
	int n;
	int i;

	n = snprintf(line, sizeof(line), "[%5u.%03u %06x] ", (unsigned)(ms / 1000), (unsigned)(ms % 1000), (unsigned)(((uintptr_t)pthread_self() >> 4) & 0xFFFFFF));
	if (n < 0 || n >= (int)sizeof(line) - 2) {
		return;
	}

	va_start(args, fmt);
	i = vsnprintf(line + n, sizeof(line) - (size_t)n - 1, fmt, args);
	va_end(args);
	if (i < 0) {
		return;
	}
	n += i;
	if (n > (int)sizeof(line) - 2) {
		n = (int)sizeof(line) - 2;
	}
	/* One record per line: replace anything that would break that. */
	for (i = 0; i < n; ++i) {
		if (line[i] == '\n' || line[i] == '\r') {
			line[i] = ' ';
		}
	}
	line[n++] = '\n';

	pthread_mutex_lock(&s_mtx);
	if (s_enabled) {
		if (append_raw(line, (size_t)n) > 0) {
			s_size += (size_t)n;
			if (s_write_errors > 0) {
				char note[80];
				int m = snprintf(note, sizeof(note), "[log] %d earlier line(s) could not be written\n", s_write_errors);
				s_write_errors = 0;
				if (m > 0 && append_raw(note, (size_t)m) > 0) {
					s_size += (size_t)m;
				}
			}
		} else {
			s_write_errors++;
		}
		if (s_size > LOG_MAX_SIZE) {
			rotate_locked();
		}
	}
	pthread_mutex_unlock(&s_mtx);
}

void log_set_anchor(const char* name, const void* addr) {
	pthread_mutex_lock(&s_mtx);
	if (s_anchor_count < MAX_ANCHORS) {
		s_anchors[s_anchor_count].name = name;
		s_anchors[s_anchor_count].addr = (uint64_t)(uintptr_t)addr;
		s_anchor_count++;
	}
	pthread_mutex_unlock(&s_mtx);
	log_write("anchor %s = 0x%llx", name, (unsigned long long)(uintptr_t)addr);
}

/* Describes an address as "<nearest anchor below> + offset". */
static void describe_addr(uint64_t addr, char* out, size_t size) {
	int best = -1;
	int i;

	for (i = 0; i < s_anchor_count; ++i) {
		if (s_anchors[i].addr <= addr && (best < 0 || s_anchors[i].addr > s_anchors[best].addr)) {
			best = i;
		}
	}
	if (best < 0) {
		snprintf(out, size, "0x%llx", (unsigned long long)addr);
	} else {
		snprintf(out, size, "0x%llx (%s+0x%llx)", (unsigned long long)addr, s_anchors[best].name, (unsigned long long)(addr - s_anchors[best].addr));
	}
}

static const char* signal_name(int sig) {
	switch (sig) {
		case SIGSEGV: return "SIGSEGV";
		case SIGBUS: return "SIGBUS";
		case SIGILL: return "SIGILL";
		case SIGFPE: return "SIGFPE";
		case SIGABRT: return "SIGABRT";
	}
	return "signal";
}

/* The layout of the FreeBSD amd64 ucontext is read by offset because the toolchain headers do not describe it. */
static void crash_handler(int sig, siginfo_t* si, void* ctx) {
	char line[300];
	char where[120];
	const uint64_t* r = (const uint64_t*)ctx;
	uint64_t ms = sceKernelGetProcessTime() / 1000;
	uint64_t bp, sp, next, ret;
	int n, i;

	n = snprintf(line, sizeof(line), "[%5u.%03u] CRASH %s (%d) code=%d fault_addr=0x%llx\n", (unsigned)(ms / 1000), (unsigned)(ms % 1000), signal_name(sig), sig,
		si ? si->si_code : -1, si ? (unsigned long long)(uintptr_t)si->si_addr : 0ULL);
	raw_line(line, (size_t)n);

	if (r) {
		describe_addr(r[22], where, sizeof(where));
		n = snprintf(line, sizeof(line), "  rip=%s rsp=0x%llx rbp=0x%llx rax=0x%llx rdi=0x%llx rsi=0x%llx rdx=0x%llx rcx=0x%llx\n", where,
			(unsigned long long)r[25], (unsigned long long)r[11], (unsigned long long)r[9], (unsigned long long)r[3], (unsigned long long)r[4], (unsigned long long)r[5], (unsigned long long)r[6]);
		raw_line(line, (size_t)n);

		bp = r[11];
		sp = r[25];
		for (i = 0; i < 14 && bp > sp && (bp - sp) < (4u << 20) && (bp & 7) == 0; ++i) {
			next = ((const uint64_t*)(uintptr_t)bp)[0];
			ret = ((const uint64_t*)(uintptr_t)bp)[1];
			describe_addr(ret, where, sizeof(where));
			n = snprintf(line, sizeof(line), "  #%d %s\n", i, where);
			raw_line(line, (size_t)n);
			if (next <= bp) {
				break;
			}
			bp = next;
		}
	}
}

static void install_crash_handlers(void) {
	struct sigaction sa;
	static const int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
	size_t i;

	memset(&sa, 0, sizeof(sa));
	sa.__sa_handler.__sa_sigaction = &crash_handler;
	/* SA_RESETHAND: after the report the default action runs, so the system still handles the crash as usual. */
	sa.sa_flags = SA_SIGINFO | SA_RESETHAND | SA_ONSTACK;
	sigemptyset(&sa.sa_mask);

	for (i = 0; i < ARRAY_SIZE(sigs); ++i) {
		if (sigaction(sigs[i], &sa, NULL) != 0) {
			log_write("could not install handler for signal %d", sigs[i]);
		}
	}
}

void* log_thread_begin(void) {
	stack_t ss;
	void* mem = malloc(ALT_STACK_SIZE);

	if (!mem) {
		return NULL;
	}
	memset(&ss, 0, sizeof(ss));
	ss.ss_sp = mem;
	ss.ss_size = ALT_STACK_SIZE;
	ss.ss_flags = 0;
	if (sigaltstack(&ss, NULL) != 0) {
		free(mem);
		return NULL;
	}
	return mem;
}

void log_thread_end(void* token) {
	stack_t ss;

	if (!token) {
		return;
	}
	memset(&ss, 0, sizeof(ss));
	ss.ss_flags = SS_DISABLE;
	sigaltstack(&ss, NULL);
	free(token);
}

void log_open(void) {
	bool crashed;
	int fd;
	time_t now = time(NULL);

	/* The marker exists while the app runs and is removed on a normal exit. */
	crashed = (access(RUN_MARKER, F_OK) == 0);

	pthread_mutex_lock(&s_mtx);
	s_enabled = true;
	measure_file_locked();
	if (s_size > LOG_MAX_SIZE) {
		rotate_locked();
	}
	pthread_mutex_unlock(&s_mtx);

	log_write("==================== Sleepy Package Installer starting (epoch %lld) ====================", (long long)now);
	if (crashed) {
		log_write("!!! The previous run did not exit cleanly (crash, forced close or power loss). Its last lines are above. !!!");
	}

	fd = open(RUN_MARKER, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd >= 0) {
		close(fd);
	}

	log_set_anchor("log_write", (const void*)&log_write);
	log_set_anchor("malloc", (const void*)&malloc);
	log_set_anchor("sceKernelUsleep", (const void*)&sceKernelUsleep);

	install_crash_handlers();
}

void log_close_clean(void) {
	log_write("clean exit");
	unlink(RUN_MARKER);
}

size_t log_read_tail(char* buf, size_t cap) {
	int fd;
	off_t end;
	off_t start;
	ssize_t got;
	size_t total = 0;

	if (!buf || cap < 2) {
		return 0;
	}
	buf[0] = '\0';

	fd = open(LOG_PATH, O_RDONLY);
	if (fd < 0) {
		return 0;
	}
	end = lseek(fd, 0, SEEK_END);
	start = end > (off_t)(cap - 1) ? end - (off_t)(cap - 1) : 0;
	lseek(fd, start, SEEK_SET);

	while (total < cap - 1) {
		got = read(fd, buf + total, cap - 1 - total);
		if (got <= 0) {
			break;
		}
		total += (size_t)got;
	}
	close(fd);
	buf[total] = '\0';
	return total;
}
