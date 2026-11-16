#include "sfo.h"
#include "util.h"

#include <fcntl.h>
#include <sys/stat.h>

#include "uthash.h"
#include "utarray.h"
#include "utlist.h"
#define SFO_MAGIC "\0PSF"

#define SIZEOF_SFO_HEADER 0x14

TYPE_BEGIN(struct sfo_header, SIZEOF_SFO_HEADER);
	TYPE_FIELD(char magic[4], 0x00);
	TYPE_FIELD(uint32_t version, 0x04);
	TYPE_FIELD(uint32_t key_table_offset, 0x08);
	TYPE_FIELD(uint32_t value_table_offset, 0x0C);
	TYPE_FIELD(uint32_t entry_count, 0x10);
TYPE_END();
TYPE_CHECK_SIZE(struct sfo_header, SIZEOF_SFO_HEADER);

#define SIZEOF_SFO_TABLE_ENTRY 0x10

TYPE_BEGIN(struct sfo_table_entry, SIZEOF_SFO_TABLE_ENTRY);
	TYPE_FIELD(uint16_t key_offset, 0x00);
	TYPE_FIELD(uint16_t format, 0x02);
	TYPE_FIELD(uint32_t size, 0x04);
	TYPE_FIELD(uint32_t max_size, 0x08);
	TYPE_FIELD(uint32_t value_offset, 0x0C);
TYPE_END();
TYPE_CHECK_SIZE(struct sfo_table_entry, SIZEOF_SFO_TABLE_ENTRY);

struct sfo* sfo_alloc(void) {
	struct sfo* sfo = NULL;

	sfo = (struct sfo*)malloc(sizeof(*sfo));
	if (!sfo)
		goto error;
	memset(sfo, 0, sizeof(*sfo));

	return sfo;

error:
	if (sfo)
		free(sfo);

	return NULL;
}

void sfo_free(struct sfo* sfo) {
	struct sfo_entry* entry;
	struct sfo_entry* tmp;

	if (!sfo)
		return;

	DL_FOREACH_SAFE(sfo->entries, entry, tmp)
	{
		DL_DELETE(sfo->entries, entry);
	
		if (entry->key)
			free(entry->key);
	
		if (entry->value)
			free(entry->value);
	
		free(entry);
	}

	free(sfo);
}

static char s_sfo_error[160] = "";

const char* sfo_last_error(void) {
	return s_sfo_error;
}

#define SFO_FAIL(...) \
	do { \
		snprintf(s_sfo_error, sizeof(s_sfo_error), __VA_ARGS__); \
		EPRINTF("%s\n", s_sfo_error); \
		goto err; \
	} while (0)

bool sfo_load_from_file(struct sfo* sfo, const char* file_path) {
	uint8_t* data = NULL;
	size_t data_size;
	int64_t file_size;
	ssize_t nread;
	int fd = -1;
	bool status = false;

	assert(sfo != NULL);
	assert(file_path != NULL);

	s_sfo_error[0] = '\0';

	fd = open(file_path, O_RDONLY);
	if (fd < 0) {
		SFO_FAIL("cannot open the saved param.sfo (errno %d)", errno);
	}

	file_size = fd_size(fd);
	if (file_size <= 0) {
		SFO_FAIL("the saved param.sfo is empty or unreadable (size %lld)", (long long)file_size);
	}
	data_size = (size_t)file_size;

	data = (uint8_t*)malloc(data_size);
	if (!data) {
		SFO_FAIL("no memory for param.sfo (%llu bytes)", (unsigned long long)data_size);
	}

	nread = read(fd, data, data_size);
	if (nread < 0) {
		SFO_FAIL("cannot read the saved param.sfo (errno %d)", errno);
	}
	if ((size_t)nread != data_size) {
		SFO_FAIL("short read of param.sfo (%lld of %llu bytes)", (long long)nread, (unsigned long long)data_size);
	}

	if (!sfo_load_from_memory(sfo, data, data_size)) {
		goto err;
	}

	status = true;

err:
	if (data) {
		free(data);
	}

	if (fd >= 0) {
		close(fd);
	}

	return status;
}

bool sfo_load_from_memory(struct sfo* sfo, const void* data, size_t data_size) {
	struct sfo_header* hdr;
	struct sfo_table_entry* entry_table;
	struct sfo_table_entry* entry;
	struct sfo_entry* entries = NULL;
	struct sfo_entry* new_entry = NULL;
	size_t entry_count, i;
	bool status = false;

	assert(sfo != NULL);
	assert(data != NULL);

	if (data_size < sizeof(*hdr)) {
		SFO_FAIL("param.sfo is too short (%llu bytes)", (unsigned long long)data_size);
	}

	hdr = (struct sfo_header*)data;
	if (memcmp(hdr->magic, SFO_MAGIC, sizeof(hdr->magic)) != 0) {
		SFO_FAIL("param.sfo has a wrong signature (%02X %02X %02X %02X, %llu bytes)", (unsigned)(uint8_t)hdr->magic[0], (unsigned)(uint8_t)hdr->magic[1],
			(unsigned)(uint8_t)hdr->magic[2], (unsigned)(uint8_t)hdr->magic[3], (unsigned long long)data_size);
	}

	entry_table = (struct sfo_table_entry*)(data + sizeof(*hdr));
	entry_count = LE32(hdr->entry_count);
	if (entry_count > 4096 || data_size < sizeof(*hdr) + entry_count * sizeof(*entry_table)) {
		SFO_FAIL("param.sfo has an invalid entry count (%llu entries, %llu bytes)", (unsigned long long)entry_count, (unsigned long long)data_size);
	}

	if (LE32(hdr->key_table_offset) >= data_size || LE32(hdr->value_table_offset) > data_size) {
		SFO_FAIL("param.sfo has invalid table offsets (keys 0x%X, values 0x%X, %llu bytes)", (unsigned)LE32(hdr->key_table_offset),
			(unsigned)LE32(hdr->value_table_offset), (unsigned long long)data_size);
	}

	for (i = 0; i < entry_count; ++i) {
		size_t key_off, val_off;

		entry = entry_table + i;
		key_off = LE32(hdr->key_table_offset) + LE16(entry->key_offset);
		val_off = LE32(hdr->value_table_offset) + LE32(entry->value_offset);

		new_entry = (struct sfo_entry*)malloc(sizeof(*new_entry));
		if (!new_entry) {
			SFO_FAIL("out of memory while reading param.sfo entry %llu", (unsigned long long)i);
		}
		memset(new_entry, 0, sizeof(*new_entry));

		new_entry->format = (enum sfo_value_format)LE16(entry->format);

		new_entry->size = LE32(entry->size);
		new_entry->area = LE32(entry->max_size);
		/* Some packages report a smaller maximum than the stored size; the stored size is what matters. */
		if (new_entry->area < new_entry->size) {
			new_entry->area = new_entry->size;
		}
		if (key_off >= data_size || val_off > data_size || new_entry->size > data_size - val_off) {
			SFO_FAIL("param.sfo entry %llu points outside the file (key 0x%llX, value 0x%llX, size %llu, file %llu bytes)", (unsigned long long)i,
				(unsigned long long)key_off, (unsigned long long)val_off, (unsigned long long)new_entry->size, (unsigned long long)data_size);
		}

		new_entry->key = strndup((const char*)data + key_off, data_size - key_off);
		if (!new_entry->key) {
			SFO_FAIL("out of memory while reading param.sfo entry %llu", (unsigned long long)i);
		}

		new_entry->value = (uint8_t*)malloc(new_entry->area + 1);
		if (!new_entry->value) {
			SFO_FAIL("out of memory while reading param.sfo entry %llu", (unsigned long long)i);
		}
		memset(new_entry->value, 0, new_entry->area + 1);
		memcpy(new_entry->value, (const uint8_t*)data + val_off, new_entry->size);

		DL_APPEND(entries, new_entry);
		new_entry = NULL;
	}
	new_entry = NULL;

	sfo->entries = entries;

	status = true;

err:
	if (new_entry) {
		if (new_entry->key) {
			free(new_entry->key);
		}

		if (new_entry->value) {
			free(new_entry->value);
		}

		free(new_entry);
	}

	return status;
}

struct sfo_entry* sfo_find_entry(struct sfo* sfo, const char* key) {
	struct sfo_entry* entry;

	assert(sfo != NULL);
	assert(key != NULL);

	DL_FOREACH(sfo->entries, entry)
	{
		if (strcmp(entry->key, key) == 0) {
			return entry;
		}
	}

	return NULL;
}
