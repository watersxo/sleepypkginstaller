#include "screen.h"
#include "status.h"
#include "font8x8.h"

#include <pthread.h>
#include <orbis/libkernel.h>
#include <orbis/VideoOut.h>
#include <orbis/SystemService.h>

#define SCREEN_W 1920
#define SCREEN_H 1080
#define SCREEN_BUFFERS 2
#define SCREEN_PITCH_BYTES (SCREEN_W * 4)
#define FRAME_BYTES (SCREEN_W * SCREEN_H * 4)
#define VIDEO_MEM_ALIGNMENT 0x200000
#define VIDEO_MEM_SIZE ALIGN_UP((size_t)FRAME_BYTES * SCREEN_BUFFERS, VIDEO_MEM_ALIGNMENT)
#define SCREEN_THREAD_STACK (256 * 1024)
#define REFRESH_USEC (200 * 1000)

#define COL_BG 0x0E1621
#define COL_PANEL 0x172536
#define COL_BAR_BG 0x2A3B52
#define COL_TEXT 0xE6EDF5
#define COL_DIM 0x8FA3B8
#define COL_ACCENT 0x4FC3F7
#define COL_OK 0x66E08A
#define COL_WARN 0xFFC857
#define COL_ERR 0xFF6B6B

#define MAX_VISIBLE_TASKS 4

static int s_video = -1;
static OrbisKernelEqueue s_flip_queue;
static void* s_video_mem = NULL;
static off_t s_direct_mem_off = 0;
static size_t s_direct_mem_size = 0;
static uint32_t* s_buffers[SCREEN_BUFFERS];
static void* s_buffer_ptrs[SCREEN_BUFFERS];
static OrbisVideoOutBufferAttribute s_attr;
static int s_front = 0;
static int64_t s_flip_arg = 0;

static pthread_t s_thread;
static volatile bool s_running = false;
static bool s_started = false;

static uint32_t* s_fb; /* buffer currently being drawn into */

static inline uint32_t pixel_of(uint32_t rgb) {
	return 0x80000000u | (rgb & 0xFFFFFF);
}

static void fill_rect(int x, int y, int w, int h, uint32_t rgb) {
	uint32_t px = pixel_of(rgb);
	int xe, ye, xx;

	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	xe = x + w > SCREEN_W ? SCREEN_W : x + w;
	ye = y + h > SCREEN_H ? SCREEN_H : y + h;

	for (; y < ye; ++y) {
		uint32_t* row = s_fb + (size_t)y * SCREEN_W;
		for (xx = x; xx < xe; ++xx) {
			row[xx] = px;
		}
	}
}

static int text_width(const char* s, int scale) {
	return (int)strlen(s) * 8 * scale;
}

static void draw_text(int x, int y, int scale, uint32_t rgb, const char* s) {
	int row, col;

	for (; *s; ++s, x += 8 * scale) {
		unsigned char c = (unsigned char)*s;
		const uint8_t* glyph;

		if (c < 0x20 || c > 0x7E) {
			c = '?';
		}
		if (c == ' ') {
			continue;
		}
		if (x + 8 * scale <= 0 || x >= SCREEN_W) {
			continue;
		}
		glyph = font8x8_basic[c - 0x20];
		for (row = 0; row < 8; ++row) {
			uint8_t bits = glyph[row];
			for (col = 0; bits; ++col, bits >>= 1) {
				if (bits & 1) {
					fill_rect(x + col * scale, y + row * scale, scale, scale, rgb);
				}
			}
		}
	}
}

static void draw_textf(int x, int y, int scale, uint32_t rgb, const char* fmt, ...) {
	char buf[200];
	va_list args;

	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	draw_text(x, y, scale, rgb, buf);
}

static void format_bytes(char* out, size_t size, uint64_t bytes) {
	if (bytes >= (uint64_t)1 << 30) {
		snprintf(out, size, "%.2f GB", (double)bytes / (double)((uint64_t)1 << 30));
	} else if (bytes >= (uint64_t)1 << 20) {
		snprintf(out, size, "%.1f MB", (double)bytes / (double)(1 << 20));
	} else if (bytes >= 1024) {
		snprintf(out, size, "%.0f KB", (double)bytes / 1024.0);
	} else {
		snprintf(out, size, "%u B", (unsigned)bytes);
	}
}

static void format_duration(char* out, size_t size, uint32_t sec) {
	snprintf(out, size, "%02u:%02u:%02u", (unsigned)(sec / 3600), (unsigned)((sec / 60) % 60), (unsigned)(sec % 60));
}

static const char* error_hint(int code) {
	switch ((uint32_t)code) {
		case 0x80990088: return "already installed";
		case 0x80990004: return "install error";
		case 0x80990015: return "not enough space";
		case 0x80990016: return "not enough space";
	}
	return "";
}

static uint32_t task_color(enum task_state state) {
	switch (state) {
		case TASK_DOWNLOADING: return COL_ACCENT;
		case TASK_DONE: return COL_OK;
		case TASK_ERROR: return COL_ERR;
		case TASK_PAUSED:
		case TASK_STOPPED:
		case TASK_STALLED: return COL_WARN;
		default: return COL_DIM;
	}
}

static void draw_task(int x, int y, int w, const struct task_info* t) {
	char done[32], total[32], speed[32], eta[24], title[72];
	int percent = t->total ? (int)(t->transferred * 100 / t->total) : 0;
	int bar_w = w - 40;
	uint32_t col = task_color(t->state);

	if (percent > 100) {
		percent = 100;
	}

	strlcpy(title, t->title, sizeof(title));
	draw_textf(x + 20, y + 10, 2, COL_TEXT, "#%d  %s", t->task_id, title);

	format_bytes(done, sizeof(done), t->transferred);
	format_bytes(total, sizeof(total), t->total);

	if (t->state == TASK_ERROR) {
		draw_textf(x + 20, y + 40, 2, col, "ERROR 0x%08X %s", (unsigned)t->error_code, error_hint(t->error_code));
	} else {
		format_bytes(speed, sizeof(speed), t->speed);
		if (t->state == TASK_DOWNLOADING && t->rest_sec > 0) {
			format_duration(eta, sizeof(eta), t->rest_sec);
		} else {
			strlcpy(eta, "--:--:--", sizeof(eta));
		}
		if (t->state == TASK_PREPARING) {
			draw_textf(x + 20, y + 40, 2, col, "%s %d%%", status_task_state_name(t->state), t->preparing_percent);
		} else if (t->state == TASK_DOWNLOADING) {
			draw_textf(x + 20, y + 40, 2, col, "%s  %d%%  %s / %s  %s/s  ETA %s", status_task_state_name(t->state), percent, done, total, speed, eta);
		} else {
			draw_textf(x + 20, y + 40, 2, col, "%s  %d%%  %s / %s", status_task_state_name(t->state), percent, done, total);
		}
	}

	fill_rect(x + 20, y + 72, bar_w, 14, COL_BAR_BG);
	fill_rect(x + 20, y + 72, bar_w * percent / 100, 14, col);
}

static void render(const struct status_snapshot* snap) {
	char tbuf[32];
	int y, i, shown;
	uint32_t state_col;
	int margin = 60;
	int content_w = SCREEN_W - margin * 2;

	fill_rect(0, 0, SCREEN_W, SCREEN_H, COL_BG);

	/* Header */
	draw_textf(margin, 36, 4, COL_ACCENT, "Sleepy Package Installer v%s", RPI_VERSION);

	/* Connection panel */
	fill_rect(margin, 130, content_w, 190, COL_PANEL);
	switch (snap->server_state) {
		case SERVER_LISTENING: state_col = COL_OK; break;
		case SERVER_ERROR: state_col = COL_ERR; break;
		default: state_col = COL_WARN; break;
	}
	if (snap->ip[0] && snap->port > 0) {
		draw_text(margin + 24, 150, 2, COL_DIM, "SERVER ADDRESS");
		draw_textf(margin + 24, 180, 6, COL_TEXT, "%s:%d", snap->ip, snap->port);
	} else {
		draw_text(margin + 24, 150, 2, COL_DIM, "SERVER ADDRESS");
		draw_text(margin + 24, 180, 6, COL_WARN, "no network");
	}
	draw_textf(margin + 24, 250, 2, state_col, "Status: %s%s%s", status_server_state_name(snap->server_state), snap->server_msg[0] ? " - " : "", snap->server_msg);
	format_duration(tbuf, sizeof(tbuf), snap->uptime_sec);
	draw_textf(margin + 24, 284, 2, COL_DIM, "Uptime %s   Requests %u   Last client %s", tbuf, (unsigned)snap->requests, snap->last_client[0] ? snap->last_client : "-");

	/* Downloads */
	y = 350;
	draw_textf(margin, y, 3, COL_TEXT, "Downloads (%d)", snap->task_count);
	y += 44;

	if (snap->task_count == 0) {
		draw_text(margin + 20, y + 10, 2, COL_DIM, "No downloads yet.");
		y += 60;
	}

	shown = snap->task_count < MAX_VISIBLE_TASKS ? snap->task_count : MAX_VISIBLE_TASKS;
	for (i = 0; i < shown; ++i) {
		/* newest tasks first */
		const struct task_info* t = &snap->tasks[snap->task_count - 1 - i];
		fill_rect(margin, y, content_w, 100, COL_PANEL);
		draw_task(margin, y, content_w, t);
		y += 108;
	}
	if (snap->task_count > shown) {
		draw_textf(margin + 20, y, 2, COL_DIM, "+ %d more", snap->task_count - shown);
	}

	/* Event log */
	{
		const int log_rows = 8;
		int first = snap->log_count > log_rows ? snap->log_count - log_rows : 0;

		y = SCREEN_H - 40 - log_rows * 20;
		fill_rect(margin, y - 10, content_w, log_rows * 20 + 20, COL_PANEL);
		for (i = first; i < snap->log_count; ++i) {
			const char* line = snap->log[i];
			uint32_t c = COL_DIM;
			if (strstr(line, "rror") || strstr(line, "ailed") || strstr(line, "nable")) {
				c = COL_ERR;
			}
			draw_text(margin + 14, y + (i - first) * 20, 2, c, line);
		}
	}
}

static bool init_video(void) {
	int ret;
	int i;

	s_video = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN, 0, 0);
	if (s_video < 0) {
		EPRINTF("sceVideoOutOpen failed: 0x%08X\n", s_video);
		return false;
	}

	ret = sceKernelCreateEqueue(&s_flip_queue, "rpi flip queue");
	if (ret < 0) {
		EPRINTF("sceKernelCreateEqueue failed: 0x%08X\n", ret);
		goto err_close;
	}
	sceVideoOutAddFlipEvent(s_flip_queue, s_video, NULL);

	s_direct_mem_size = VIDEO_MEM_SIZE;
	ret = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), s_direct_mem_size, VIDEO_MEM_ALIGNMENT, 3, &s_direct_mem_off);
	if (ret < 0) {
		EPRINTF("sceKernelAllocateDirectMemory failed: 0x%08X\n", ret);
		goto err_queue;
	}

	ret = sceKernelMapDirectMemory(&s_video_mem, s_direct_mem_size, 0x33, 0, s_direct_mem_off, VIDEO_MEM_ALIGNMENT);
	if (ret < 0) {
		EPRINTF("sceKernelMapDirectMemory failed: 0x%08X\n", ret);
		sceKernelReleaseDirectMemory(s_direct_mem_off, s_direct_mem_size);
		goto err_queue;
	}

	for (i = 0; i < SCREEN_BUFFERS; ++i) {
		s_buffers[i] = (uint32_t*)((uint8_t*)s_video_mem + (size_t)i * FRAME_BYTES);
		s_buffer_ptrs[i] = s_buffers[i];
		memset(s_buffers[i], 0, FRAME_BYTES);
	}

	sceVideoOutSetBufferAttribute(&s_attr, 0x80000000, 1, 0, SCREEN_W, SCREEN_H, SCREEN_W);
	ret = sceVideoOutRegisterBuffers(s_video, 0, s_buffer_ptrs, SCREEN_BUFFERS, &s_attr);
	if (ret != 0) {
		EPRINTF("sceVideoOutRegisterBuffers failed: 0x%08X\n", ret);
		goto err_queue;
	}

	sceVideoOutSetFlipRate(s_video, 0);
	return true;

err_queue:
	sceKernelDeleteEqueue(s_flip_queue);

err_close:
	sceVideoOutClose(s_video);
	s_video = -1;
	return false;
}

static void release_video(void) {
	if (s_video < 0) {
		return;
	}

	sceVideoOutUnregisterBuffers(s_video, 0);
	sceKernelDeleteEqueue(s_flip_queue);
	sceVideoOutClose(s_video);
	s_video = -1;

	if (s_video_mem) {
		sceKernelMunmap(s_video_mem, s_direct_mem_size);
		s_video_mem = NULL;
	}
	if (s_direct_mem_size) {
		sceKernelReleaseDirectMemory(s_direct_mem_off, s_direct_mem_size);
		s_direct_mem_size = 0;
	}
}

static void present(int index) {
	OrbisKernelEvent evt;
	OrbisVideoOutFlipStatus flip_status;
	uint32_t timeout = 200 * 1000;
	int count, tries;

	s_flip_arg++;
	sceVideoOutSubmitFlip(s_video, index, ORBIS_VIDEO_OUT_FLIP_VSYNC, s_flip_arg);

	for (tries = 0; tries < 5; ++tries) {
		sceVideoOutGetFlipStatus(s_video, &flip_status);
		if (flip_status.flipArg == s_flip_arg) {
			break;
		}
		if (sceKernelWaitEqueue(s_flip_queue, &evt, 1, &count, &timeout) != 0) {
			break;
		}
	}
}

static void* screen_thread(void* arg) {
	struct status_snapshot* snap;
	uint32_t last_generation = 0;
	uint32_t last_uptime = (uint32_t)-1;
	bool first = true;

	(void)arg;

	snap = (struct status_snapshot*)malloc(sizeof(*snap));
	if (!snap) {
		return NULL;
	}

	while (s_running) {
		status_get_snapshot(snap);

		if (first || snap->generation != last_generation || snap->uptime_sec != last_uptime) {
			int back = (s_front + 1) % SCREEN_BUFFERS;

			s_fb = s_buffers[back];
			render(snap);
			present(back);
			s_front = back;

			last_generation = snap->generation;
			last_uptime = snap->uptime_sec;
			if (first) {
				/* The launch splash (pic0/pic1) stays on top of our frames until it is hidden. */
				sceSystemServiceHideSplashScreen();
			}
			first = false;
		}

		sceKernelUsleep(REFRESH_USEC);
	}

	free(snap);
	return NULL;
}

bool screen_start(void) {
	pthread_attr_t attr;
	int ret;

	if (s_started) {
		return true;
	}

	if (!init_video()) {
		return false;
	}

	s_running = true;

	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, SCREEN_THREAD_STACK);
	ret = pthread_create(&s_thread, &attr, &screen_thread, NULL);
	pthread_attr_destroy(&attr);
	if (ret) {
		EPRINTF("Unable to start screen thread: %d\n", ret);
		s_running = false;
		release_video();
		return false;
	}

	s_started = true;
	return true;
}

/* Stops drawing and gives the display resources back to the system. Safe to call more than once. */
void screen_stop(void) {
	if (!s_started) {
		return;
	}

	s_running = false;
	pthread_join(s_thread, NULL);
	s_started = false;

	release_video();
}
