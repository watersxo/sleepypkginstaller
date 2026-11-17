#include "installer.h"
#include "pkg.h"
#include "util.h"
#include "module.h"
#include "KPutil.h"
#include "log.h"

#include <pthread.h>
#include <orbis/libkernel.h>
#include <orbis/UserService.h>
#include <orbis/AppInstUtil.h>
#include <orbis/Bgft.h>
#include <sys/param.h>

#define _NDBG 
#define BGFT_HEAP_SIZE (8 * 1024 * 1024)
#define BGFT_HEAP_SIZE_FALLBACK (1 * 1024 * 1024)
#define WAIT_TIME (UINT64_C(5) * 1000 * 1000) /* 5 secs */

/*
 * The system services behind these calls are used from several threads here (request handlers and the progress
 * monitor). The functions below keep their original names but run one at a time: the implementations are renamed
 * here and wrapped with a lock at the end of this file.
 */
#define app_inst_util_uninstall_game app_inst_util_uninstall_game_impl
#define app_inst_util_uninstall_ac app_inst_util_uninstall_ac_impl
#define app_inst_util_uninstall_patch app_inst_util_uninstall_patch_impl
#define app_inst_util_uninstall_theme app_inst_util_uninstall_theme_impl
#define app_inst_util_is_exists app_inst_util_is_exists_impl
#define app_inst_util_get_size app_inst_util_get_size_impl
#define bgft_download_register_package_task bgft_download_register_package_task_impl
#define bgft_download_start_task bgft_download_start_task_impl
#define bgft_download_stop_task bgft_download_stop_task_impl
#define bgft_download_pause_task bgft_download_pause_task_impl
#define bgft_download_resume_task bgft_download_resume_task_impl
#define bgft_download_unregister_task bgft_download_unregister_task_impl
#define bgft_download_reregister_task_patch bgft_download_reregister_task_patch_impl
#define bgft_download_get_task_progress bgft_download_get_task_progress_impl
#define bgft_download_find_task_by_content_id bgft_download_find_task_by_content_id_impl

static pthread_mutex_t s_service_mtx = PTHREAD_MUTEX_INITIALIZER;

#define SCE_BGFT_INVALID_TASK_ID (-1)
#define SCE_BGFT_ERROR_SAME_APPLICATION_ALREADY_INSTALLED (0x80990088)

enum bgft_task_option_t {
	BGFT_TASK_OPTION_NONE = 0x0,
	BGFT_TASK_OPTION_DELETE_AFTER_UPLOAD = 0x1,
	BGFT_TASK_OPTION_INVISIBLE = 0x2,
	BGFT_TASK_OPTION_ENABLE_PLAYGO = 0x4,
	BGFT_TASK_OPTION_FORCE_UPDATE = 0x8,
	BGFT_TASK_OPTION_REMOTE = 0x10,
	BGFT_TASK_OPTION_COPY_CRASH_REPORT_FILES = 0x20,
	BGFT_TASK_OPTION_DISABLE_INSERT_POPUP = 0x40,
	BGFT_TASK_OPTION_DISABLE_CDN_QUERY_PARAM = 0x10000,
};

static OrbisBgftInitParams s_bgft_init_params;

static bool s_app_inst_util_initialized = false;
static bool s_bgft_initialized = false;

static bool modify_download_task_for_patch_internal(const char* path, int index);
static bool modify_download_task_for_patch(OrbisBgftTaskId task_id);

bool app_inst_util_init(void) {
	int ret;

	if (s_app_inst_util_initialized) {
		goto done;
	}

	ret = sceAppInstUtilInitialize();
	log_set_anchor("sceAppInstUtilInitialize", (const void*)&sceAppInstUtilInitialize);
	if (ret) {
		EPRINTF("sceAppInstUtilInitialize failed: 0x%08X\n", ret);
		goto err;
	}

	s_app_inst_util_initialized = true;

done:
	return true;

err:
	s_app_inst_util_initialized = false;

	return false;
}

void app_inst_util_fini(void) {
	int ret;

	if (!s_app_inst_util_initialized) {
		return;
	}

	ret = sceAppInstUtilTerminate();
	if (ret) {
		EPRINTF("sceAppInstUtilTerminate failed: 0x%08X\n", ret);
	}

	s_app_inst_util_initialized = false;
}

bool app_inst_util_uninstall_game(const char* title_id, int* error) {
	int ret;

	if (!s_app_inst_util_initialized) {
		ret = ORBIS_KERNEL_ERROR_ENXIO;
		if (error) {
			*error = ret;
		}
		goto err;
	}

	if (!title_id) {
		ret = ORBIS_KERNEL_ERROR_EINVAL;
		if (error) {
			*error = ret;
		}
		goto err;
	}

	ret = sceAppInstUtilAppUnInstall(title_id);
	if (ret) {
		if (error) {
			*error = ret;
		}
		EPRINTF("sceAppInstUtilAppUnInstall failed: 0x%08X\n", ret);
		goto err;
	}

	return true;

err:
	return false;
}

bool app_inst_util_uninstall_ac(const char* content_id, int* error) {
	struct pkg_content_info content_info;
	int ret;

	if (!s_app_inst_util_initialized) {
		ret = ORBIS_KERNEL_ERROR_ENXIO;
		if (error) {
			*error = ret;
		}
		goto err;
	}

	if (!content_id) {
invalid_content_id:
		ret = ORBIS_KERNEL_ERROR_EINVAL;
		if (error) {
			*error = ret;
		}
		goto err;
	}
	if (!pkg_parse_content_id(content_id, &content_info)) {
		goto invalid_content_id;
	}

	ret = sceAppInstUtilAppUnInstallAddcont(content_info.title_id, content_info.label);
	if (ret) {
		if (error) {
			*error = ret;
		}
		EPRINTF("sceAppInstUtilAppUnInstallAddcont failed: 0x%08X\n", ret);
		goto err;
	}

done:
	return true;

err:
	return false;
}

bool app_inst_util_uninstall_patch(const char* title_id, int* error) {
	int ret;

	if (!s_app_inst_util_initialized) {
		ret = ORBIS_KERNEL_ERROR_ENXIO;
		if (error) {
			*error = ret;
		}
		goto err;
	}

	if (!title_id) {
		ret = ORBIS_KERNEL_ERROR_EINVAL;
		if (error) {
			*error = ret;
		}
		goto err;
	}

	ret = sceAppInstUtilAppUnInstallPat(title_id);
	if (ret) {
		if (error) {
			*error = ret;
		}
		EPRINTF("sceAppInstUtilAppUnInstallPat failed: 0x%08X\n", ret);
		goto err;
	}

	return true;

err:
	return false;
}

bool app_inst_util_uninstall_theme(const char* content_id, int* error) {
	int ret;

	if (!s_app_inst_util_initialized) {
		ret = ORBIS_KERNEL_ERROR_ENXIO;
		if (error) {
			*error = ret;
		}
		goto err;
	}

	if (!content_id) {
		ret = ORBIS_KERNEL_ERROR_EINVAL;
		if (error) {
			*error = ret;
		}
		goto err;
	}

	ret = sceAppInstUtilAppUnInstallTheme(content_id);
	if (ret) {
		if (error) {
			*error = ret;
		}
		EPRINTF("sceAppInstUtilAppUnInstallTheme failed: 0x%08X\n", ret);
		goto err;
	}

	return true;

err:
	return false;
}

bool app_inst_util_is_exists(const char* title_id, bool* exists, int* error) {
	int flag;
	int ret;

	if (!s_app_inst_util_initialized) {
		ret = ORBIS_KERNEL_ERROR_ENXIO;
		if (error) {
			*error = ret;
		}
		goto err;
	}

	if (!title_id) {
		ret = ORBIS_KERNEL_ERROR_EINVAL;
		if (error) {
			*error = ret;
		}
		goto err;
	}

	ret = sceAppInstUtilAppExists(title_id, &flag);
	if (ret) {
		if (error) {
			*error = ret;
		}
		EPRINTF("sceAppInstUtilAppExists failed: 0x%08X\n", ret);
		goto err;
	}

	if (exists) {
		*exists = flag;
	}

	return true;

err:
	return false;
}

bool app_inst_util_get_size(const char* title_id, unsigned long* size, int* error) {
	uint32_t tmp_size = 0;
	int ret;

	if (!s_app_inst_util_initialized) {
		ret = ORBIS_KERNEL_ERROR_ENXIO;
		if (error) {
			*error = ret;
		}
		goto err;
	}

	if (!title_id) {
		ret = ORBIS_KERNEL_ERROR_EINVAL;
		if (error) {
			*error = ret;
		}
		goto err;
	}

	ret = sceAppInstUtilAppGetSize(title_id, &tmp_size);
	if (ret) {
		if (error) {
			*error = ret;
		}
		EPRINTF("sceAppInstUtilAppGetSize failed: 0x%08X\n", ret);
		goto err;
	}

	if (size) {
		*size = tmp_size;
	}

	return true;

err:
	return false;
}

bool bgft_init(void) 
{
	static const size_t heap_sizes[] = { BGFT_HEAP_SIZE, BGFT_HEAP_SIZE_FALLBACK };
	size_t attempt;
	int ret = -1;

	if (s_bgft_initialized) {
		goto done;
	}

	/* The BGFT library keeps its task bookkeeping in this heap; a bigger one leaves room for many queued tasks. */
	for (attempt = 0; attempt < ARRAY_SIZE(heap_sizes); ++attempt) {
		memset(&s_bgft_init_params, 0, sizeof(s_bgft_init_params));
		s_bgft_init_params.heapSize = heap_sizes[attempt];
		s_bgft_init_params.heap = (uint8_t*)malloc(s_bgft_init_params.heapSize);
		if (!s_bgft_init_params.heap) {
			EPRINTF("No memory for BGFT heap.\n");
			log_write("BGFT: no memory for a %zu byte heap", heap_sizes[attempt]);
			continue;
		}
		memset(s_bgft_init_params.heap, 0, s_bgft_init_params.heapSize);

		ret = sceBgftServiceIntInit(&s_bgft_init_params);
		if (attempt == 0) {
			log_set_anchor("sceBgftServiceIntInit", (const void*)&sceBgftServiceIntInit);
		}
		log_write("BGFT init with a %zu byte heap: 0x%08X", heap_sizes[attempt], (unsigned)ret);
		if (ret == 0) {
			break;
		}
		EPRINTF("sceBgftServiceIntInit failed: 0x%08X\n", ret);
		free(s_bgft_init_params.heap);
		s_bgft_init_params.heap = NULL;
	}
	if (ret != 0 || !s_bgft_init_params.heap) {
		goto err_bgft_heap_free;
	}

	s_bgft_initialized = true;

done:
	return true;

err_bgft_heap_free:
	if (s_bgft_init_params.heap) {
		free(s_bgft_init_params.heap);
		s_bgft_init_params.heap = NULL;
	}

	memset(&s_bgft_init_params, 0, sizeof(s_bgft_init_params));

err:
	s_bgft_initialized = false;

	return false;

}

void bgft_fini(void) {
	int ret;

	if (!s_bgft_initialized) {
		return;
	}

	ret = sceBgftServiceIntTerm();
	if (ret) {
		EPRINTF("sceBgftServiceIntTerm failed: 0x%08X\n", ret);
	}

	if (s_bgft_init_params.heap) {
		free(s_bgft_init_params.heap);
		s_bgft_init_params.heap = NULL;
	}

	memset(&s_bgft_init_params, 0, sizeof(s_bgft_init_params));

	s_bgft_initialized = false;
}

bool bgft_download_register_package_task(const char* content_id, const char* content_url, const char* content_name, const char* icon_path, const char* package_type, const char* package_sub_type, unsigned long package_size, bool is_patch, int* out_task_id, int* error)
{
	OrbisBgftDownloadParam params;
	OrbisBgftDownloadRegisterErrorInfo error_info;
	OrbisBgftTaskId task_id;
	struct pkg_content_info content_info;
	int user_id;
	int ret;

	if (!s_app_inst_util_initialized) {
		ret = ORBIS_KERNEL_ERROR_ENXIO;
		if (error) {
			*error = ret;
		}
		goto err;
	}
	if (!s_bgft_initialized) {
		ret = ORBIS_KERNEL_ERROR_ENXIO;
		if (error) {
			*error = ret;
		}
		goto err;
	}

	if (!pkg_parse_content_id(content_id, &content_info)) {
		ret = ORBIS_KERNEL_ERROR_EINVAL;
		if (error) {
			*error = ret;
		}
		goto err;
	}

	ret = sceUserServiceGetForegroundUser(&user_id);
	if (ret) {
		if (error) {
			*error = ret;
		}
		EPRINTF("sceUserServiceGetForegroundUser failed: 0x%08X\n", ret);
		goto err;
	}

	memset(&error_info, 0, sizeof(error_info));

	memset(&params, 0, sizeof(params));
	{
		params.entitlementType = 5; /* TODO: figure out */
		params.userId = user_id;
		params.id = content_id;
		params.contentUrl = content_url;
		params.contentName = content_name;
		params.iconPath = icon_path ? icon_path : "";
		params.playgoScenarioId = "0";
		params.option = ORBIS_BGFT_TASK_OPT_DISABLE_CDN_QUERY_PARAM;
		params.packageType = package_type;
		params.packageSubType = package_sub_type ? package_sub_type : "";
		params.packageSize = package_size;
	}

	task_id = SCE_BGFT_INVALID_TASK_ID;

	if (!is_patch) {
		ret = sceBgftServiceIntDownloadRegisterTask(&params, &task_id);
	}
	else {
		ret = sceBgftServiceIntDebugDownloadRegisterPkg(&params, &task_id);
	}
	if (ret) {
		if (error) {
			*error = ret;
		}
		/* Report this as a failure: returning a task id of -1 made clients try to start an invalid task. */
		EPRINTF("sceBgftServiceIntDownloadRegisterTask failed: 0x%08X\n", ret);
		goto err;
	}

	if (out_task_id) {
		*out_task_id = (int)task_id;
	}

	return true;

err:
	return false;
}

bool bgft_download_start_task(int task_id, int* error) {
	int ret;

	if (!s_bgft_initialized) {
		ret = ORBIS_KERNEL_ERROR_ENXIO;
		if (error) {
			*error = ret;
		}
		goto err;
	}

	if (task_id < 0) {
		ret = ORBIS_KERNEL_ERROR_EINVAL;
		if (error) {
			*error = ret;
		}
		goto err;
	}

	ret = sceBgftServiceDownloadStartTask((OrbisBgftTaskId)task_id);
	if (ret) {
		if (error) {
			*error = ret;
		}
		EPRINTF("sceBgftServiceDownloadStartTask failed: 0x%08X\n", ret);
		goto err;
	}

	return true;

err:
	return false;
}

bool bgft_download_stop_task(int task_id, int* error) {
	int ret;

	if (!s_bgft_initialized) {
		ret = ORBIS_KERNEL_ERROR_ENXIO;
		if (error) {
			*error = ret;
		}
		goto err;
	}

	if (task_id < 0) {
		ret = ORBIS_KERNEL_ERROR_EINVAL;
		if (error) {
			*error = ret;
		}
		goto err;
	}

	ret = sceBgftServiceDownloadStopTask((OrbisBgftTaskId)task_id);
	if (ret) {
		if (error) {
			*error = ret;
		}
		EPRINTF("sceBgftServiceDownloadStopTask failed: 0x%08X\n", ret);
		goto err;
	}

	return true;

err:
	return false;
}

bool bgft_download_pause_task(int task_id, int* error) {
	int ret;

	if (!s_bgft_initialized) {
		ret = ORBIS_KERNEL_ERROR_ENXIO;
		if (error) {
			*error = ret;
		}
		goto err;
	}

	if (task_id < 0) {
		ret = ORBIS_KERNEL_ERROR_EINVAL;
		if (error) {
			*error = ret;
		}
		goto err;
	}

	ret = sceBgftServiceDownloadPauseTask((OrbisBgftTaskId)task_id);
	if (ret) {
		if (error) {
			*error = ret;
		}
		EPRINTF("sceBgftServiceDownloadPauseTask failed: 0x%08X\n", ret);
		goto err;
	}

	return true;

err:
	return false;
}

bool bgft_download_resume_task(int task_id, int* error) {
	int ret;

	if (!s_bgft_initialized) {
		ret = ORBIS_KERNEL_ERROR_ENXIO;
		if (error) {
			*error = ret;
		}
		goto err;
	}

	if (task_id < 0) {
		ret = ORBIS_KERNEL_ERROR_EINVAL;
		if (error) {
			*error = ret;
		}
		goto err;
	}

	ret = sceBgftServiceDownloadResumeTask((OrbisBgftTaskId)task_id);
	if (ret) {
		if (error) {
			*error = ret;
		}
		EPRINTF("sceBgftServiceDownloadResumeTask failed: 0x%08X\n", ret);
		goto err;
	}

	return true;

err:
	return false;
}

bool bgft_download_unregister_task(int task_id, int* error) {
	int ret;

	if (!s_bgft_initialized) {
		ret = ORBIS_KERNEL_ERROR_ENXIO;
		if (error) {
			*error = ret;
		}
		goto err;
	}

	if (task_id < 0) {
		ret = ORBIS_KERNEL_ERROR_EINVAL;
		if (error) {
			*error = ret;
		}
		goto err;
	}

	ret = sceBgftServiceIntDownloadUnregisterTask((OrbisBgftTaskId)task_id);
	if (ret) {
		if (error) {
			*error = ret;
		}
		EPRINTF("sceBgftServiceIntDownloadUnregisterTask failed: 0x%08X\n", ret);
		goto err;
	}

	return true;

err:
	return false;
}

bool bgft_download_reregister_task_patch(int old_task_id, int* new_task_id, int* error) {
	OrbisBgftTaskId tmp_id;
	int ret;

	if (!s_bgft_initialized) {
		ret = ORBIS_KERNEL_ERROR_ENXIO;
		if (error) {
			*error = ret;
		}
		goto err;
	}

	if (old_task_id < 0) {
		ret = ORBIS_KERNEL_ERROR_EINVAL;
		if (error) {
			*error = ret;
		}
		goto err;
	}

	tmp_id = SCE_BGFT_INVALID_TASK_ID;
	ret = sceBgftServiceIntDownloadReregisterTaskPatch((OrbisBgftTaskId)old_task_id, &tmp_id);
	if (ret) {
		if (error) {
			*error = ret;
		}
		EPRINTF("sceBgftServiceIntDownloadReregisterTaskPatch failed: 0x%08X\n", ret);
		goto err;
	}

	if (new_task_id) {
		*new_task_id = (int)tmp_id;
	}

	return true;

err:
	return false;
}

bool bgft_download_get_task_progress(int task_id, struct bgft_download_task_progress_info* progress_info, int* error) {
	union {
		struct bgft_download_task_progress_info info;
		uint8_t bytes[256];
	} raw;
	int ret;

	if (!s_bgft_initialized) {
		ret = ORBIS_KERNEL_ERROR_ENXIO;
		if (error) {
			*error = ret;
		}
		goto err;
	}

	if (task_id < 0) {
		ret = ORBIS_KERNEL_ERROR_EINVAL;
		if (error) {
			*error = ret;
		}
		goto err;
	}
	if (!progress_info) {
		ret = ORBIS_KERNEL_ERROR_EINVAL;
		if (error) {
			*error = ret;
		}
		goto err;
	}

	/* The toolchain header declares OrbisBgftTaskProgress with 32-bit sizes (48 bytes), but the real service writes
	   64-bit sizes (64 bytes). Use our own correctly laid out structure inside a larger zeroed buffer. */
	memset(&raw, 0, sizeof(raw));
	ret = sceBgftServiceDownloadGetProgress((OrbisBgftTaskId)task_id, (OrbisBgftTaskProgress*)(void*)raw.bytes);
	if (ret) {
		if (error) {
			*error = ret;
		}
		EPRINTF("sceBgftServiceDownloadGetProgress failed: 0x%08X\n", ret);
		goto err;
	}

	*progress_info = raw.info;

	return true;

err:
	return false;
}

bool bgft_download_find_task_by_content_id(const char* content_id, int sub_type, int* task_id, int* error) {
	OrbisBgftTaskId tmp_id;
	int ret;

	if (!s_bgft_initialized) {
		ret = ORBIS_KERNEL_ERROR_ENXIO;
		if (error) {
			*error = ret;
		}
		goto err;
	}

	if (!content_id) {
		ret = ORBIS_KERNEL_ERROR_EINVAL;
		if (error) {
			*error = ret;
		}
		goto err;
	}
	if (!((OrbisBgftTaskSubType)sub_type > ORBIS_BGFT_TASK_SUB_TYPE_UNKNOWN && (OrbisBgftTaskSubType)sub_type < ORBIS_BGFT_TASK_SUB_TYPE_MAX)) {
		ret = ORBIS_KERNEL_ERROR_EINVAL;
		if (error) {
			*error = ret;
		}
		goto err;
	}

	tmp_id = SCE_BGFT_INVALID_TASK_ID;
	ret = sceBgftServiceDownloadFindTaskByContentId(content_id, (OrbisBgftTaskSubType)sub_type, &tmp_id);
	if (ret) {
		if (error) {
			*error = ret;
		}
		EPRINTF("sceBgftServiceDownloadFindTaskByContentId failed: 0x%08X\n", ret);
		goto err;
	}

	if (task_id) {
		*task_id = (int)tmp_id;
	}

	return true;

err:
	return false;
}

#undef app_inst_util_uninstall_game
#undef app_inst_util_uninstall_ac
#undef app_inst_util_uninstall_patch
#undef app_inst_util_uninstall_theme
#undef app_inst_util_is_exists
#undef app_inst_util_get_size
#undef bgft_download_register_package_task
#undef bgft_download_start_task
#undef bgft_download_stop_task
#undef bgft_download_pause_task
#undef bgft_download_resume_task
#undef bgft_download_unregister_task
#undef bgft_download_reregister_task_patch
#undef bgft_download_get_task_progress
#undef bgft_download_find_task_by_content_id

#define SERVICE_LOCK() pthread_mutex_lock(&s_service_mtx)
#define SERVICE_UNLOCK() pthread_mutex_unlock(&s_service_mtx)

bool app_inst_util_uninstall_game(const char* title_id, int* error) {
	bool ok;
	int err = 0;

	log_write("AppInstUtil uninstall game %s", title_id ? title_id : "(null)");
	SERVICE_LOCK();
	ok = app_inst_util_uninstall_game_impl(title_id, &err);
	SERVICE_UNLOCK();
	log_write("AppInstUtil uninstall game -> %s 0x%08X", ok ? "ok" : "failed", (unsigned)err);
	if (error) {
		*error = err;
	}
	return ok;
}

bool app_inst_util_uninstall_ac(const char* content_id, int* error) {
	bool ok;
	int err = 0;

	log_write("AppInstUtil uninstall add-on %s", content_id ? content_id : "(null)");
	SERVICE_LOCK();
	ok = app_inst_util_uninstall_ac_impl(content_id, &err);
	SERVICE_UNLOCK();
	log_write("AppInstUtil uninstall add-on -> %s 0x%08X", ok ? "ok" : "failed", (unsigned)err);
	if (error) {
		*error = err;
	}
	return ok;
}

bool app_inst_util_uninstall_patch(const char* title_id, int* error) {
	bool ok;
	int err = 0;

	log_write("AppInstUtil uninstall patch %s", title_id ? title_id : "(null)");
	SERVICE_LOCK();
	ok = app_inst_util_uninstall_patch_impl(title_id, &err);
	SERVICE_UNLOCK();
	log_write("AppInstUtil uninstall patch -> %s 0x%08X", ok ? "ok" : "failed", (unsigned)err);
	if (error) {
		*error = err;
	}
	return ok;
}

bool app_inst_util_uninstall_theme(const char* content_id, int* error) {
	bool ok;
	int err = 0;

	log_write("AppInstUtil uninstall theme %s", content_id ? content_id : "(null)");
	SERVICE_LOCK();
	ok = app_inst_util_uninstall_theme_impl(content_id, &err);
	SERVICE_UNLOCK();
	log_write("AppInstUtil uninstall theme -> %s 0x%08X", ok ? "ok" : "failed", (unsigned)err);
	if (error) {
		*error = err;
	}
	return ok;
}

bool app_inst_util_is_exists(const char* title_id, bool* exists, int* error) {
	bool ok;
	int err = 0;

	SERVICE_LOCK();
	ok = app_inst_util_is_exists_impl(title_id, exists, &err);
	SERVICE_UNLOCK();
	if (!ok) {
		log_write("AppInstUtil exists %s failed 0x%08X", title_id ? title_id : "(null)", (unsigned)err);
	}
	if (error) {
		*error = err;
	}
	return ok;
}

bool app_inst_util_get_size(const char* title_id, unsigned long* size, int* error) {
	bool ok;
	int err = 0;

	SERVICE_LOCK();
	ok = app_inst_util_get_size_impl(title_id, size, &err);
	SERVICE_UNLOCK();
	if (!ok) {
		log_write("AppInstUtil size %s failed 0x%08X", title_id ? title_id : "(null)", (unsigned)err);
	}
	if (error) {
		*error = err;
	}
	return ok;
}

bool bgft_download_register_package_task(const char* content_id, const char* content_url, const char* content_name, const char* icon_path, const char* package_type, const char* package_sub_type, unsigned long package_size, bool is_patch, int* out_task_id, int* error) {
	bool ok;
	int err = 0;
	int id = -1;

	log_write("BGFT register: id=%s type=%s patch=%d size=%lu url=%s", content_id ? content_id : "(null)", package_type ? package_type : "(null)", is_patch ? 1 : 0, package_size, content_url ? content_url : "(null)");
	SERVICE_LOCK();
	ok = bgft_download_register_package_task_impl(content_id, content_url, content_name, icon_path, package_type, package_sub_type, package_size, is_patch, &id, &err);
	SERVICE_UNLOCK();
	log_write("BGFT register -> %s task=%d 0x%08X", ok ? "ok" : "failed", id, (unsigned)err);
	if (out_task_id) {
		*out_task_id = id;
	}
	if (error) {
		*error = err;
	}
	return ok;
}

#define DEFINE_TASK_OP(name, label) \
	bool name(int task_id, int* error) { \
		bool ok; \
		int err = 0; \
		log_write("BGFT " label " task %d", task_id); \
		SERVICE_LOCK(); \
		ok = name##_impl(task_id, &err); \
		SERVICE_UNLOCK(); \
		log_write("BGFT " label " task %d -> %s 0x%08X", task_id, ok ? "ok" : "failed", (unsigned)err); \
		if (error) { \
			*error = err; \
		} \
		return ok; \
	}

DEFINE_TASK_OP(bgft_download_start_task, "start")
DEFINE_TASK_OP(bgft_download_stop_task, "stop")
DEFINE_TASK_OP(bgft_download_pause_task, "pause")
DEFINE_TASK_OP(bgft_download_resume_task, "resume")
DEFINE_TASK_OP(bgft_download_unregister_task, "unregister")

bool bgft_download_reregister_task_patch(int old_task_id, int* new_task_id, int* error) {
	bool ok;
	int err = 0;
	int id = -1;

	log_write("BGFT reregister patch task %d", old_task_id);
	SERVICE_LOCK();
	ok = bgft_download_reregister_task_patch_impl(old_task_id, &id, &err);
	SERVICE_UNLOCK();
	log_write("BGFT reregister patch -> %s task=%d 0x%08X", ok ? "ok" : "failed", id, (unsigned)err);
	if (new_task_id) {
		*new_task_id = id;
	}
	if (error) {
		*error = err;
	}
	return ok;
}

bool bgft_download_get_task_progress(int task_id, struct bgft_download_task_progress_info* progress_info, int* error) {
	bool ok;
	int err = 0;

	SERVICE_LOCK();
	ok = bgft_download_get_task_progress_impl(task_id, progress_info, &err);
	SERVICE_UNLOCK();
	if (error) {
		*error = err;
	}
	return ok;
}

bool bgft_download_find_task_by_content_id(const char* content_id, int sub_type, int* task_id, int* error) {
	bool ok;
	int err = 0;
	int id = -1;

	SERVICE_LOCK();
	ok = bgft_download_find_task_by_content_id_impl(content_id, sub_type, &id, &err);
	SERVICE_UNLOCK();
	log_write("BGFT find task %s -> %s task=%d 0x%08X", content_id ? content_id : "(null)", ok ? "ok" : "failed", id, (unsigned)err);
	if (task_id) {
		*task_id = id;
	}
	if (error) {
		*error = err;
	}
	return ok;
}
