#include <ctype.h>
#include <stdlib.h>
#include <orbis/libkernel.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>

#include "server.h"
#include "installer.h"
#include "pkg.h"
#include "sfo.h"
#include "http.h"
#include "util.h"
#include "dirent.h"
#include "sandbird.h"
#include "tiny-json.h"
#include "status.h"

#define CLEANUP_DAY_COUNT 3
#define MAX_JSON_TOKENS 65536
#define STATUS_JSON_SIZE (16 * 1024)

typedef bool handler_cb(sb_Stream* s, const char* method, const char* path, char* in_data);

struct handler_desc {
	const char* path;
	handler_cb* handler;
	bool need_partial_match;
	bool get_only;
	bool count_request;
};

/* A parsed JSON request body. The token pool is sized from the input so large package lists fit. */
struct request {
	sb_Stream* s;
	json_t* pool;
	const json_t* root;
};

static char* s_work_dir = NULL;
static sb_Server* s_server = NULL;
static volatile bool s_server_started = false;
static volatile bool s_stop_requested = false;
static uint32_t s_tmp_counter = 0;

static int event_handler(sb_Event* e);

static bool handle_root(sb_Stream* s, const char* method, const char* path, char* in_data);
static bool handle_api_status(sb_Stream* s, const char* method, const char* path, char* in_data);
static bool handle_api_install(sb_Stream* s, const char* method, const char* path, char* in_data);
static bool handle_api_uninstall_game(sb_Stream* s, const char* method, const char* path, char* in_data);
static bool handle_api_uninstall_ac(sb_Stream* s, const char* method, const char* path, char* in_data);
static bool handle_api_uninstall_patch(sb_Stream* s, const char* method, const char* path, char* in_data);
static bool handle_api_uninstall_theme(sb_Stream* s, const char* method, const char* path, char* in_data);
static bool handle_api_is_exists(sb_Stream* s, const char* method, const char* path, char* in_data);
static bool handle_api_start_task(sb_Stream* s, const char* method, const char* path, char* in_data);
static bool handle_api_stop_task(sb_Stream* s, const char* method, const char* path, char* in_data);
static bool handle_api_pause_task(sb_Stream* s, const char* method, const char* path, char* in_data);
static bool handle_api_resume_task(sb_Stream* s, const char* method, const char* path, char* in_data);
static bool handle_api_unregister_task(sb_Stream* s, const char* method, const char* path, char* in_data);
static bool handle_api_get_task_progress(sb_Stream* s, const char* method, const char* path, char* in_data);
static bool handle_api_find_task(sb_Stream* s, const char* method, const char* path, char* in_data);

static bool handle_static(sb_Stream* s, const char* method, const char* path, char* in_data);

static void set_cors_header(sb_Stream* s);
static void kick_error(sb_Stream* s, int code, const char* title, const char* error);
static void kick_result_header_json(sb_Stream* s);
static void kick_error_json(sb_Stream* s, int code);
static void kick_success_json(sb_Stream* s);

static void cleanup_temp_files(void);

static char* encodeURI(const char *src);

static const struct handler_desc s_handlers[] = {
	{ "/", &handle_root, false, true, false },
	{ "/api/status", &handle_api_status, false, false, false },
	{ "/static/", &handle_static, true, true, false },
	{ "/api/install", &handle_api_install, false, false, true },
	{ "/api/uninstall_game", &handle_api_uninstall_game, false, false, true },
	{ "/api/uninstall_ac", &handle_api_uninstall_ac, false, false, true },
	{ "/api/uninstall_patch", &handle_api_uninstall_patch, false, false, true },
	{ "/api/uninstall_theme", &handle_api_uninstall_theme, false, false, true },
	{ "/api/is_exists", &handle_api_is_exists, false, false, true },
	{ "/api/start_task", &handle_api_start_task, false, false, true },
	{ "/api/stop_task", &handle_api_stop_task, false, false, true },
	{ "/api/pause_task", &handle_api_pause_task, false, false, true },
	{ "/api/resume_task", &handle_api_resume_task, false, false, true },
	{ "/api/unregister_task", &handle_api_unregister_task, false, false, true },
	{ "/api/get_task_progress", &handle_api_get_task_progress, false, false, false },
	{ "/api/find_task", &handle_api_find_task, false, false, true },
};

bool server_start(int port, const char* work_dir) {
	sb_Options opts;
	char port_str[16];

	if (s_server_started) {
		return true;
	}

	if (!work_dir || strlen(work_dir) == 0) {
		EPRINTF("Empty working directory specified.\n");
		return false;
	}

	s_work_dir = strdup(work_dir);
	if (!s_work_dir) {
		return false;
	}
	s_stop_requested = false;

	cleanup_temp_files();

	memset(&opts, 0, sizeof(opts));
	snprintf(port_str, sizeof(port_str), "%d", port);
	opts.port = port_str;
	opts.handler = &event_handler;
	opts.timeout = "0";
	opts.max_lifetime = "0";
	opts.max_request_size = "0";

	s_server = sb_new_server(&opts);
	if (!s_server) {
		EPRINTF("Unable to initialize server on port %d.\n", port);
		free(s_work_dir);
		s_work_dir = NULL;
		return false;
	}

	s_server_started = true;
	return true;
}

bool server_listen(void) {
	if (!s_server_started) {
		return false;
	}

	while (!s_stop_requested) {
		sb_poll_server(s_server);
	}

	return true;
}

void server_stop(void) {
	if (!s_server_started) {
		return;
	}

	s_stop_requested = true;
	s_server_started = false;

	sb_close_server(s_server);
	s_server = NULL;

	free(s_work_dir);
	s_work_dir = NULL;
}

/* Returns the request data of a GET request as a heap string (never NULL unless out of memory). */
static char* get_query_data(sb_Stream* st, const char* name) {
	size_t size = st->recv_buf.len + 1;
	char* data = (char*)malloc(size);

	if (data) {
		sb_get_var(st, name, data, size);
	}
	return data;
}

static int event_handler(sb_Event* e) {
	const struct handler_desc* desc = NULL;
	sb_Stream* st = e->stream;
	const char* path = e->path;
	const char* method = e->method;
	char* data = NULL;
	bool is_get;
	size_t i;

	if (e->type != SB_EV_REQUEST) {
		return SB_RES_OK;
	}

	if (strcasecmp(method, "OPTIONS") == 0) {
		kick_result_header_json(st);
		return SB_RES_OK;
	}

	is_get = (strcasecmp(method, "GET") == 0);
	if (!is_get && strcasecmp(method, "POST") != 0) {
		kick_error(st, 400, "Bad request", "Unsupported method");
		return SB_RES_OK;
	}

	for (i = 0; i < ARRAY_SIZE(s_handlers); ++i) {
		const struct handler_desc* d = &s_handlers[i];
		bool match = d->need_partial_match ? (strstr(path, d->path) == path) : (strcmp(path, d->path) == 0);

		if (match) {
			desc = d;
			break;
		}
	}
	if (!desc || (desc->get_only && !is_get)) {
		kick_error(st, 400, "Bad request", "Unknown request");
		return SB_RES_OK;
	}

	if (desc->count_request) {
		status_note_request(e->address);
	}

	if (is_get) {
		data = get_query_data(st, "data");
		if (!data) {
			kick_error(st, 500, "Internal server error", "No memory.");
			return SB_RES_OK;
		}
		(*desc->handler)(st, method, path, data);
		free(data);
	} else {
		data = st->recv_buf.s + st->data_idx;
		(*desc->handler)(st, method, path, data);
	}

	return SB_RES_OK;
}

static void fail_request(sb_Stream* s, const char* message) {
	status_log("Error: %.90s", message);
	kick_error(s, 500, "Internal server error", message);
}

#define THROW_ERROR(format, ...) \
	do { \
		char tmp_buf[256]; \
		snprintf(tmp_buf, sizeof(tmp_buf), format, ##__VA_ARGS__); \
		fail_request(s, tmp_buf); \
		goto err; \
	} while (0)

static bool request_open(struct request* r, sb_Stream* s, char* in_data) {
	size_t tokens = 4;
	const char* p;

	r->s = s;
	r->pool = NULL;
	r->root = NULL;

	for (p = in_data; *p; ++p) {
		if (*p == '{' || *p == '[' || *p == ',' || *p == ':') {
			++tokens;
		}
	}
	if (tokens > MAX_JSON_TOKENS) {
		fail_request(s, "Request too large.");
		return false;
	}

	r->pool = (json_t*)calloc(tokens, sizeof(*r->pool));
	if (!r->pool) {
		fail_request(s, "No memory.");
		return false;
	}

	r->root = json_create(in_data, r->pool, (unsigned int)tokens);
	if (!r->root) {
		fail_request(s, "Invalid JSON format.");
		free(r->pool);
		r->pool = NULL;
		return false;
	}

	return true;
}

static void request_close(struct request* r) {
	free(r->pool);
	r->pool = NULL;
}

static bool request_text(struct request* r, const char* name, const char** out) {
	const json_t* field = json_getProperty(r->root, name);
	char msg[128];

	if (!field) {
		snprintf(msg, sizeof(msg), "No '%s' parameter specified.", name);
		fail_request(r->s, msg);
		return false;
	}
	if (json_getType(field) != JSON_TEXT) {
		snprintf(msg, sizeof(msg), "Invalid type for parameter '%s'.", name);
		fail_request(r->s, msg);
		return false;
	}

	*out = json_getValue(field);
	return true;
}

static bool request_int(struct request* r, const char* name, int* out) {
	const json_t* field = json_getProperty(r->root, name);
	char msg[128];

	if (!field) {
		snprintf(msg, sizeof(msg), "No '%s' parameter specified.", name);
		fail_request(r->s, msg);
		return false;
	}
	if (json_getType(field) != JSON_INTEGER) {
		snprintf(msg, sizeof(msg), "Invalid type for parameter '%s'.", name);
		fail_request(r->s, msg);
		return false;
	}

	*out = (int)json_getInteger(field);
	return true;
}

static bool request_task_id(struct request* r, int* out) {
	char msg[128];

	if (!request_int(r, "task_id", out)) {
		return false;
	}
	if (*out < 0) {
		snprintf(msg, sizeof(msg), "Invalid value for '%s' parameter specified.", "task_id");
		fail_request(r->s, msg);
		return false;
	}
	return true;
}

static bool is_http_url(const char* url) {
	return starts_with(url, "http://") || starts_with(url, "https://");
}

/* Registers a download task for the given pieces and replies to the client. */
static bool install_pieces(sb_Stream* s, char** piece_urls, size_t piece_count) {
	char tmp_name[48];
	char ref_pkg_json_path[1024];
	char param_sfo_path[1024];
	char icon0_png_path[1024];
	struct sfo* sfo = NULL;
	struct sfo_entry* sfo_entry;
	char title_entry_key[16];
	char title_name[256];
	char escaped_title_name[256 * 2 + 1];
	char content_id[PKG_CONTENT_ID_SIZE + 1];
	char content_url[256];
	char icon_path[1024];
	char error_buf[256];
	char ip[16];
	enum pkg_content_type content_type;
	const char* package_type;
	const char* package_sub_type = NULL;
	uint64_t package_size;
	bool is_patch;
	bool has_icon = false;
	int port;
	int lang_id;
	int task_id = -1;
	int ret = 0;
	uint32_t counter;

	memset(ref_pkg_json_path, 0, sizeof(ref_pkg_json_path));
	memset(param_sfo_path, 0, sizeof(param_sfo_path));
	memset(icon0_png_path, 0, sizeof(icon0_png_path));

	status_log("Install request: %.80s", piece_urls[0]);

	if (!get_language_id(&lang_id)) {
		THROW_ERROR("Unable to get language id.");
	}

	status_get_endpoint(ip, sizeof(ip), &port);
	if (ip[0] == '\0') {
		THROW_ERROR("The console has no network address.");
	}

	counter = __atomic_add_fetch(&s_tmp_counter, 1, __ATOMIC_SEQ_CST);
	snprintf(tmp_name, sizeof(tmp_name), "tmp_%" PRIxMAX "_%x", (uintmax_t)time(NULL), (unsigned)counter);

	snprintf(ref_pkg_json_path, sizeof(ref_pkg_json_path), "%s/%s.json", s_work_dir, tmp_name);
	snprintf(param_sfo_path, sizeof(param_sfo_path), "%s/%s.sfo", s_work_dir, tmp_name);
	snprintf(icon0_png_path, sizeof(icon0_png_path), "%s/%s.png", s_work_dir, tmp_name);

	memset(error_buf, 0, sizeof(error_buf));
	if (!pkg_setup_prerequisites(piece_urls, piece_count, ref_pkg_json_path, param_sfo_path, icon0_png_path, &content_type, &package_size, &is_patch, &has_icon, error_buf, sizeof(error_buf))) {
		rtrim(error_buf);
		if (*error_buf != '\0')
			THROW_ERROR("Unable to set up prerequisites for package '%s': %s", piece_urls[0], error_buf);
		else
			THROW_ERROR("Unable to set up prerequisites for package '%s'.", piece_urls[0]);
	}

	switch (content_type) {
		case PKG_CONTENT_TYPE_GD: package_type = "PS4GD"; break;
		case PKG_CONTENT_TYPE_AC: package_type = "PS4AC"; break;
		case PKG_CONTENT_TYPE_AL: package_type = "PS4AL"; break;
		case PKG_CONTENT_TYPE_DP: package_type = "PS4DP"; break;
		default:
			package_type = NULL;
			THROW_ERROR("Unsupported content type for package '%s'.", piece_urls[0]);
			break;
	}

	sfo = sfo_alloc();
	if (!sfo) {
		THROW_ERROR("Unable to allocate system file object for package '%s'.", piece_urls[0]);
	}
	if (!sfo_load_from_file(sfo, param_sfo_path)) {
		THROW_ERROR("Unable to read the title information of '%s': %s.", piece_urls[0], sfo_last_error());
	}

	snprintf(title_entry_key, sizeof(title_entry_key), "TITLE_%02d", lang_id);
	sfo_entry = sfo_find_entry(sfo, title_entry_key);
	if (!sfo_entry) {
		strlcpy(title_entry_key, "TITLE", sizeof(title_entry_key));
		sfo_entry = sfo_find_entry(sfo, title_entry_key);
		if (!sfo_entry) {
			THROW_ERROR("Unable to get title for package '%s'.", piece_urls[0]);
		}
	}
	if (sfo_entry->format != SFO_FORMAT_STRING || sfo_entry->size < 1) {
		THROW_ERROR("Invalid format of '%s' entry in system file object for package '%s'.", title_entry_key, piece_urls[0]);
	}
	strlcpy(title_name, (const char*)sfo_entry->value, sizeof(title_name));

	if (!http_escape_json_string(escaped_title_name, sizeof(escaped_title_name), title_name)) {
		THROW_ERROR("Unable to escape title name.");
	}

	sfo_entry = sfo_find_entry(sfo, "CONTENT_ID");
	if (!sfo_entry) {
		THROW_ERROR("Unable to get content id for package '%s'.", piece_urls[0]);
	}
	if (sfo_entry->format != SFO_FORMAT_STRING || sfo_entry->size != sizeof(content_id)) {
		THROW_ERROR("Invalid format of '%s' entry in system file object for package '%s'.", "CONTENT_ID", piece_urls[0]);
	}
	strlcpy(content_id, (const char*)sfo_entry->value, sizeof(content_id));

	snprintf(content_url, sizeof(content_url), "http://%s:%d/static/%s.json", ip, port, tmp_name);
	snprintf(icon_path, sizeof(icon_path), "/user%s/%s.png", s_work_dir, tmp_name);

	if (bgft_download_register_package_task(content_id, content_url, title_name, has_icon ? icon_path : NULL, package_type, package_sub_type, package_size, is_patch, &task_id, &ret)) {
		status_task_add(task_id, title_name, package_size);
		kick_result_header_json(s);
		sb_writef(s, "{ \"status\": \"success\", \"task_id\": %d, \"title\": \"%s\" }\n", task_id, escaped_title_name);
	} else {
		status_log("Error: '%.50s' rejected (0x%08X)", title_name, (unsigned int)ret);
		unlink(ref_pkg_json_path);
		unlink(icon0_png_path);
		kick_error_json(s, ret);
	}

	unlink(param_sfo_path);
	sfo_free(sfo);
	return true;

err:
	if (ref_pkg_json_path[0]) {
		unlink(ref_pkg_json_path);
	}
	if (param_sfo_path[0]) {
		unlink(param_sfo_path);
	}
	if (icon0_png_path[0]) {
		unlink(icon0_png_path);
	}
	if (sfo) {
		sfo_free(sfo);
	}
	return false;
}

static void free_url_list(char** urls, size_t count) {
	size_t i;

	if (!urls) {
		return;
	}
	for (i = 0; i < count; ++i) {
		free(urls[i]);
	}
	free(urls);
}

static bool handle_api_install_direct(struct request* r) {
	sb_Stream* s = r->s;
	const json_t* field;
	const json_t* item;
	char** piece_urls = NULL;
	char* unescaped_url = NULL;
	size_t unescaped_url_size;
	size_t piece_count = 0;
	size_t i;
	bool status = false;

	field = json_getProperty(r->root, "packages");
	if (!field) {
		THROW_ERROR("No '%s' parameter specified.", "packages");
	}
	if (json_getType(field) != JSON_ARRAY) {
		THROW_ERROR("Invalid type for parameter '%s'.", "packages");
	}

	for (item = json_getChild(field); item != NULL; item = json_getSibling(item)) {
		const char* value;

		if (json_getType(item) != JSON_TEXT) {
			THROW_ERROR("Invalid type for element of parameter '%s'.", "packages");
		}
		value = json_getValue(item);
		if (strlen(value) == 0) {
			THROW_ERROR("Empty element value of parameter '%s'.", "packages");
		}
		if (!http_unescape_uri(&unescaped_url, &unescaped_url_size, value)) {
			THROW_ERROR("Unable to unescape element value of parameter '%s'.", "packages");
		}
		if (!is_http_url(unescaped_url)) {
			free(unescaped_url);
			unescaped_url = NULL;
			THROW_ERROR("Unexpected element value of parameter '%s'.", "packages");
		}
		free(unescaped_url);
		unescaped_url = NULL;

		++piece_count;
	}
	if (piece_count == 0) {
		THROW_ERROR("No packages.");
	}

	piece_urls = (char**)calloc(piece_count, sizeof(*piece_urls));
	if (!piece_urls) {
		THROW_ERROR("No memory.");
	}

	for (item = json_getChild(field), i = 0; item != NULL; item = json_getSibling(item)) {
		if (!http_unescape_uri(&unescaped_url, &unescaped_url_size, json_getValue(item))) {
			THROW_ERROR("Unable to unescape element value of parameter '%s'.", "packages");
		}

		piece_urls[i] = encodeURI(unescaped_url);
		free(unescaped_url);
		unescaped_url = NULL;
		if (!piece_urls[i]) {
			THROW_ERROR("No memory.");
		}
		++i;
	}

	status = install_pieces(s, piece_urls, piece_count);

err:
	free(unescaped_url);
	free_url_list(piece_urls, piece_count);
	return status;
}

static bool handle_api_install_ref_pkg_url(struct request* r) {
	sb_Stream* s = r->s;
	const char* url;
	char* unescaped_url = NULL;
	size_t unescaped_url_size;
	char** piece_urls = NULL;
	size_t piece_count = 0;
	bool status = false;

	if (!request_text(r, "url", &url)) {
		return false;
	}
	if (strlen(url) == 0) {
		THROW_ERROR("Empty element value of parameter '%s'.", "url");
	}
	if (!http_unescape_uri(&unescaped_url, &unescaped_url_size, url)) {
		THROW_ERROR("Unable to unescape element value of parameter '%s'.", "url");
	}
	if (!is_http_url(unescaped_url)) {
		THROW_ERROR("Unexpected element value of parameter '%s'.", "url");
	}

	piece_urls = pkg_extract_piece_urls_from_ref_pkg_json(unescaped_url, &piece_count);
	if (!piece_urls) {
		THROW_ERROR("Unable to extract pieces URLs for '%s'.", unescaped_url);
	}

	status = install_pieces(s, piece_urls, piece_count);

err:
	free(unescaped_url);
	free_url_list(piece_urls, piece_count);
	return status;
}

static bool handle_api_install(sb_Stream* s, const char* method, const char* path, char* in_data) {
	struct request r;
	const char* type;
	bool status = false;

	if (!request_open(&r, s, in_data)) {
		return false;
	}

	if (request_text(&r, "type", &type)) {
		if (strcasecmp(type, "direct") == 0) {
			status = handle_api_install_direct(&r);
		} else if (strcasecmp(type, "ref_pkg_url") == 0) {
			status = handle_api_install_ref_pkg_url(&r);
		} else {
			char msg[128];
			snprintf(msg, sizeof(msg), "Invalid type '%.40s'.", type);
			fail_request(s, msg);
		}
	}

	request_close(&r);
	return status;
}

typedef bool uninstall_fn(const char* id, int* error);

static bool handle_uninstall(sb_Stream* s, char* in_data, const char* param, size_t id_size, uninstall_fn* fn) {
	struct request r;
	const char* value;
	char id[PKG_CONTENT_ID_SIZE + 1];
	int ret = 0;
	bool status = false;

	if (!request_open(&r, s, in_data)) {
		return false;
	}

	if (request_text(&r, param, &value)) {
		strlcpy(id, value, MIN(id_size, sizeof(id)));

		if (fn(id, &ret)) {
			status_log("Uninstalled %s", id);
			kick_success_json(s);
		} else {
			status_log("Error: uninstall %.40s failed (0x%08X)", id, (unsigned int)ret);
			kick_error_json(s, ret);
		}
		status = true;
	}

	request_close(&r);
	return status;
}

static bool handle_api_uninstall_game(sb_Stream* s, const char* method, const char* path, char* in_data) {
	return handle_uninstall(s, in_data, "title_id", PKG_TITLE_ID_SIZE + 1, &app_inst_util_uninstall_game);
}

static bool handle_api_uninstall_ac(sb_Stream* s, const char* method, const char* path, char* in_data) {
	return handle_uninstall(s, in_data, "content_id", PKG_CONTENT_ID_SIZE + 1, &app_inst_util_uninstall_ac);
}

static bool handle_api_uninstall_patch(sb_Stream* s, const char* method, const char* path, char* in_data) {
	return handle_uninstall(s, in_data, "title_id", PKG_TITLE_ID_SIZE + 1, &app_inst_util_uninstall_patch);
}

static bool handle_api_uninstall_theme(sb_Stream* s, const char* method, const char* path, char* in_data) {
	return handle_uninstall(s, in_data, "content_id", PKG_CONTENT_ID_SIZE + 1, &app_inst_util_uninstall_theme);
}

static bool handle_api_is_exists(sb_Stream* s, const char* method, const char* path, char* in_data) {
	struct request r;
	const char* value;
	char title_id[PKG_TITLE_ID_SIZE + 1];
	unsigned long size;
	bool exists;
	int ret = 0;
	bool status = false;

	if (!request_open(&r, s, in_data)) {
		return false;
	}

	if (request_text(&r, "title_id", &value)) {
		strlcpy(title_id, value, sizeof(title_id));

		if (app_inst_util_is_exists(title_id, &exists, &ret)) {
			kick_result_header_json(s);
			if (exists) {
				if (!app_inst_util_get_size(title_id, &size, &ret)) {
					size = (unsigned long)-1;
				}
				sb_writef(s, "{ \"status\": \"success\", \"exists\": \"%s\", \"size\": 0x%" PRIXMAX " }\n", exists ? "true" : "false", (uintmax_t)size);
			} else {
				sb_writef(s, "{ \"status\": \"success\", \"exists\": \"%s\" }\n", exists ? "true" : "false");
			}
		} else {
			kick_error_json(s, ret);
		}
		status = true;
	}

	request_close(&r);
	return status;
}

enum task_op {
	TASK_OP_START,
	TASK_OP_STOP,
	TASK_OP_PAUSE,
	TASK_OP_RESUME,
	TASK_OP_UNREGISTER,
};

static bool handle_task_op(sb_Stream* s, char* in_data, enum task_op op) {
	struct request r;
	int task_id;
	int ret = 0;
	bool ok = false;
	bool status = false;

	if (!request_open(&r, s, in_data)) {
		return false;
	}

	if (request_task_id(&r, &task_id)) {
		switch (op) {
			case TASK_OP_START:
				ok = bgft_download_start_task(task_id, &ret);
				if (ok) {
					status_task_mark(task_id, false, false);
					status_log("Task %d started", task_id);
				}
				break;
			case TASK_OP_STOP:
				ok = bgft_download_stop_task(task_id, &ret);
				if (ok) {
					status_task_mark(task_id, false, true);
					status_log("Task %d stopped", task_id);
				}
				break;
			case TASK_OP_PAUSE:
				ok = bgft_download_pause_task(task_id, &ret);
				if (ok) {
					status_task_mark(task_id, true, false);
					status_log("Task %d paused", task_id);
				}
				break;
			case TASK_OP_RESUME:
				ok = bgft_download_resume_task(task_id, &ret);
				if (ok) {
					status_task_mark(task_id, false, false);
					status_log("Task %d resumed", task_id);
				}
				break;
			case TASK_OP_UNREGISTER:
				ok = bgft_download_unregister_task(task_id, &ret);
				if (ok) {
					status_task_remove(task_id);
				}
				break;
		}

		if (ok) {
			kick_success_json(s);
		} else {
			status_log("Error: task %d operation failed (0x%08X)", task_id, (unsigned int)ret);
			kick_error_json(s, ret);
		}
		status = true;
	}

	request_close(&r);
	return status;
}

static bool handle_api_start_task(sb_Stream* s, const char* method, const char* path, char* in_data) {
	return handle_task_op(s, in_data, TASK_OP_START);
}

static bool handle_api_stop_task(sb_Stream* s, const char* method, const char* path, char* in_data) {
	return handle_task_op(s, in_data, TASK_OP_STOP);
}

static bool handle_api_pause_task(sb_Stream* s, const char* method, const char* path, char* in_data) {
	return handle_task_op(s, in_data, TASK_OP_PAUSE);
}

static bool handle_api_resume_task(sb_Stream* s, const char* method, const char* path, char* in_data) {
	return handle_task_op(s, in_data, TASK_OP_RESUME);
}

static bool handle_api_unregister_task(sb_Stream* s, const char* method, const char* path, char* in_data) {
	return handle_task_op(s, in_data, TASK_OP_UNREGISTER);
}

static bool handle_api_get_task_progress(sb_Stream* s, const char* method, const char* path, char* in_data) {
	struct request r;
	int task_id;
	struct bgft_download_task_progress_info progress_info;
	int ret = 0;
	bool status = false;

	if (!request_open(&r, s, in_data)) {
		return false;
	}

	if (request_task_id(&r, &task_id)) {
		if (bgft_download_get_task_progress(task_id, &progress_info, &ret)) {
			kick_result_header_json(s);

			/* TODO: make bits field more user-friendly */
			sb_writef(s,
				"{ \"status\": \"success\", \"bits\": 0x%" PRIX32 ", \"error\": %d, \"length\": 0x%" PRIXMAX ", \"transferred\": 0x%" PRIXMAX ", \"length_total\": 0x%" PRIXMAX ", \"transferred_total\": 0x%" PRIXMAX ", \"num_index\": %" PRIu32 ", \"num_total\": %" PRIu32 ", \"rest_sec\": %" PRIu32 ", \"rest_sec_total\": %" PRIu32 ", \"preparing_percent\": %" PRId32 ", \"local_copy_percent\": %" PRId32 " }\n",
				progress_info.bits, progress_info.error_result, (uintmax_t)progress_info.length, (uintmax_t)progress_info.transferred, (uintmax_t)progress_info.length_total, (uintmax_t)progress_info.transferred_total, progress_info.num_index, progress_info.num_total, progress_info.rest_sec, progress_info.rest_sec_total, progress_info.preparing_percent, progress_info.local_copy_percent
			);
		} else {
			kick_error_json(s, ret);
		}
		status = true;
	}

	request_close(&r);
	return status;
}

static bool handle_api_find_task(sb_Stream* s, const char* method, const char* path, char* in_data) {
	struct request r;
	const char* value;
	char content_id[PKG_CONTENT_ID_SIZE + 1];
	int sub_type;
	int task_id = -1;
	int ret = 0;
	bool status = false;

	if (!request_open(&r, s, in_data)) {
		return false;
	}

	if (request_text(&r, "content_id", &value) && request_int(&r, "sub_type", &sub_type)) {
		strlcpy(content_id, value, sizeof(content_id));

		if (bgft_download_find_task_by_content_id(content_id, sub_type, &task_id, &ret)) {
			kick_result_header_json(s);
			sb_writef(s, "{ \"status\": \"success\", \"task_id\": %d }\n", task_id);
		} else {
			kick_error_json(s, ret);
		}
		status = true;
	}

	request_close(&r);
	return status;
}

static bool handle_api_status(sb_Stream* s, const char* method, const char* path, char* in_data) {
	char* buf = (char*)malloc(STATUS_JSON_SIZE);
	size_t len;

	if (!buf) {
		kick_error(s, 500, "Internal server error", "No memory.");
		return false;
	}

	len = status_to_json(buf, STATUS_JSON_SIZE);
	kick_result_header_json(s);
	sb_write(s, buf, len);

	free(buf);
	return true;
}

static bool handle_root(sb_Stream* s, const char* method, const char* path, char* in_data) {
	static const char page[] =
		"<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><title>Sleepy Package Installer</title>"
		"<body style='font-family:sans-serif;background:#0e1621;color:#e6edf5;margin:24px'>"
		"<h2>Sleepy Package Installer</h2><pre id=o>loading...</pre>"
		"<script>async function t(){try{const r=await(await fetch('/api/status')).json();"
		"let s=r.ip+':'+r.port+'  '+r.server_state+'\\n\\n';"
		"for(const k of r.tasks)s+='#'+k.task_id+' '+k.title+' - '+k.state+' '+k.percent+'%\\n';"
		"s+='\\n'+r.log.join('\\n');document.getElementById('o').textContent=s}catch(e){}setTimeout(t,2000)}t()</script>";

	sb_send_status(s, 200, "OK");
	sb_send_header(s, "Content-Type", "text/html; charset=utf-8");
	sb_send_header(s, "Connection", "close");
	sb_write(s, page, sizeof(page) - 1);
	return true;
}

static bool handle_static(sb_Stream* s, const char* method, const char* path, char* in_data) {
	const char* content_type;
	const char* name;
	char real_path[1024];
	int ret;

	if (!starts_with(path, "/static/") || strstr(path, ":/") || strstr(path, "..")) {
		kick_error(s, 400, "Bad request", "Invalid path");
		return false;
	}
	name = path + strlen("/static/");

	/* Only the temporary files created for the BGFT service are served, never arbitrary files from the work directory. */
	if (!starts_with(name, "tmp_") || strchr(name, '/') || strchr(name, '\\')) {
		kick_error(s, 403, "Forbidden", "Access is denied");
		return false;
	}

	snprintf(real_path, sizeof(real_path), "%s/%s", s_work_dir, name);

	if (ends_with_nocase(real_path, ".json")) {
		content_type = "application/json";
	} else if (ends_with_nocase(real_path, ".png")) {
		content_type = "image/png";
	} else {
		kick_error(s, 403, "Forbidden", "Access is denied");
		return false;
	}

	if (!is_file_exists(real_path)) {
		kick_error(s, 404, "Not found", "Page not found");
		return false;
	}

	sb_send_status(s, 200, "OK");
	sb_send_header(s, "Connection", "close");
	sb_send_header(s, "Content-Type", content_type);

	ret = sb_send_file(s, real_path);
	if (ret) {
		kick_error(s, 500, "Internal server error", sb_error_str(ret));
		return false;
	}

	return true;
}

static void set_cors_header(sb_Stream* s) {
	sb_send_header(s, "Content-Type", "application/json");
	sb_send_header(s, "Access-Control-Allow-Origin", "*");
	sb_send_header(s, "Access-Control-Allow-Methods", "*");
	sb_send_header(s, "Access-Control-Allow-Headers", "*");
}

static void kick_error(sb_Stream* s, int code, const char* title, const char* error) {
	char escaped_error[1024];

	if (!error) {
		error = "Unknown";
	}

	sb_send_status(s, code, title);
	set_cors_header(s);
	sb_send_header(s, "Connection", "close");

	if (!http_escape_json_string(escaped_error, sizeof(escaped_error), error)) {
		strlcpy(escaped_error, "Unable to escape error string.", sizeof(escaped_error));
	}

	sb_writef(s, "{ \"status\": \"fail\", \"error\": \"%s\" }\n", escaped_error);
}

static void kick_result_header_json(sb_Stream* s) {
	sb_send_status(s, 200, "OK");
	set_cors_header(s);
	sb_send_header(s, "Connection", "close");
}

static void kick_error_json(sb_Stream* s, int code) {
	kick_result_header_json(s);
	sb_writef(s, "{ \"status\": \"fail\", \"error_code\": 0x%08X }\n", code);
}

static void kick_success_json(sb_Stream* s) {
	kick_result_header_json(s);
	sb_writef(s, "{ \"status\": \"success\" }\n");
}

static void cleanup_temp_files(void) {
	char full_path[1024];
	char buf[8192];
	time_t now = time(NULL);
	int fd;
	int ret;

	fd = sceKernelOpen(s_work_dir, O_RDONLY, 0);
	if (fd < 0) {
		EPRINTF("sceKernelOpen failed: 0x%08X\n", fd);
		return;
	}

	for (;;) {
		int offset = 0;

		memset(buf, 0, sizeof(buf));

		ret = sceKernelGetdents(fd, buf, sizeof(buf));
		if (ret < 0) {
			EPRINTF("sceKernelGetdents failed: 0x%08X\n", ret);
			break;
		}
		if (ret == 0) {
			break;
		}

		while (offset < ret) {
			struct dirent* entry = (struct dirent*)(buf + offset);

			if (entry->d_fileno == 0 || entry->d_reclen == 0) {
				break;
			}

			/* DT_REG == 8 */
			if (entry->d_type == 8 && starts_with(entry->d_name, "tmp_") && (ends_with(entry->d_name, ".json") || ends_with(entry->d_name, ".sfo") || ends_with(entry->d_name, ".png"))) {
				/* The creation time is part of the file name (tmp_<hex time>_<counter>), so no stat() is needed. */
				char* end = NULL;
				unsigned long long created = strtoull(entry->d_name + 4, &end, 16);

				snprintf(full_path, sizeof(full_path), "%s/%s", s_work_dir, entry->d_name);

				if (end && *end == '_' && created > 1000000000ULL && (unsigned long long)now >= created
					&& ((unsigned long long)now - created) >= (unsigned long long)CLEANUP_DAY_COUNT * 24 * 60 * 60) {
					unlink(full_path);
				}
			}

			offset += entry->d_reclen;
		}
	}

	ret = sceKernelClose(fd);
	if (ret) {
		EPRINTF("sceKernelClose failed: 0x%08X\n", ret);
	}
}

/* Converts an integer value to its hex character*/
static char to_hex(char code) {
	static const char hex[] = "0123456789abcdef";
	return hex[code & 15];
}

static char* encodeURI(const char* str) {
	const unsigned char* pstr = (const unsigned char*)str;
	char* buf = (char*)malloc(strlen(str) * 3 + 1);
	char* pbuf = buf;

	if (!buf) {
		return NULL;
	}

	while (*pstr) {
		if (isalnum(*pstr) || *pstr == '-' || *pstr == '_' || *pstr == '.' || *pstr == '~' || *pstr == '+' || *pstr == ':' || *pstr == '/' || *pstr == '@') {
			*pbuf++ = (char)*pstr;
		} else {
			*pbuf++ = '%';
			*pbuf++ = to_hex((char)(*pstr >> 4));
			*pbuf++ = to_hex((char)(*pstr & 15));
		}
		pstr++;
	}
	*pbuf = '\0';
	return buf;
}
