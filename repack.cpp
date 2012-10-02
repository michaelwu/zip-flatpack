
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <endian.h>
#include <stdint.h>
#include <unistd.h>

#include <zlib.h>

#define TRACE(...)

struct local_file_header {
	uint32_t signature;
	uint16_t min_version;
	uint16_t general_flag;
	uint16_t compression;
	uint16_t lastmod_time;
	uint16_t lastmod_date;
	uint32_t crc32;
	uint32_t compressed_size;
	uint32_t uncompressed_size;
	uint16_t filename_size;
	uint16_t extra_field_size;
	char     data[0];
} __attribute__((__packed__));

struct data_descriptor {
	uint32_t crc32;
	uint32_t compressed_size;
	uint32_t uncompressed_size;
} __attribute__((__packed__));

struct cdir_entry {
	uint32_t signature;
	uint16_t creator_version;
	uint16_t min_version;
	uint16_t general_flag;
	uint16_t compression;
	uint16_t lastmod_time;
	uint16_t lastmod_date;
	uint32_t crc32;
	uint32_t compressed_size;
	uint32_t uncompressed_size;
	uint16_t filename_size;
	uint16_t extra_field_size;
	uint16_t file_comment_size;
	uint16_t disk_num;
	uint16_t internal_attr;
	uint32_t external_attr;
	uint32_t offset;
	char     data[0];
} __attribute__((__packed__));

struct cdir_end {
	uint32_t signature;
	uint16_t disk_num;
	uint16_t cdir_disk;
	uint16_t disk_entries;
	uint16_t cdir_entries;
	uint32_t cdir_size;
	uint32_t cdir_offset;
	uint16_t comment_size;
	char     comment[0];
} __attribute__((__packed__));

size_t zip_size;

static uint32_t file_header_size(struct local_file_header *file)
{
	return sizeof(*file) + 
	       le16toh(file->filename_size) +
	       le16toh(file->extra_field_size);
}

static uint32_t cdir_entry_size (struct cdir_entry *entry)
{
	return sizeof(*entry) +
	       le16toh(entry->filename_size) +
	       le16toh(entry->extra_field_size) +
	       le16toh(entry->file_comment_size);
}

static void * map_file(const char *file)
{
	int fd = open(file, O_RDONLY);
	if (fd == -1) {
		perror("open");
		return NULL;
	}

	struct stat s;
	if (fstat(fd, &s) == -1) {
		perror("fstat");
		return NULL;
	}

	zip_size = s.st_size;
	void *addr = mmap(NULL, zip_size, PROT_READ, MAP_SHARED, fd, 0);
	if (addr == MAP_FAILED) {
		perror("mmap");
		return NULL;
	}

	close(fd);
	return addr;
}


static uint32_t find_lowest_offset(struct cdir_entry *entry, int count)
{
	uint32_t lowest_offset = zip_size;
	while (count--) {
		uint32_t entry_offset = le32toh(entry->offset);
		if (lowest_offset > entry_offset)
			lowest_offset = entry_offset;

		if (le32toh(entry->signature) != 0x02014b50) {
			printf("invalid signature on cdir_entry! (count=%d)\n", count);
			exit(-1);
		}

		entry = (struct cdir_entry *)((char *)entry + cdir_entry_size(entry));
	}
	return lowest_offset;
}


static uint32_t simple_write(int fd, const char *buf, uint32_t count)
{
	uint32_t out_offset = 0;
	while (out_offset < count) {
		uint32_t written = write(fd, buf + out_offset,
					 count - out_offset);
		if (written == (uint32_t)-1) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				continue;
			else {
				printf("error \"%s\" while writing\n",
				       strerror(errno));
				exit(-1);
			}
		}

		out_offset += written;
	}
	return out_offset;
}

static int
flatten_entry(int fd,
	      struct local_file_header *file,
	      struct cdir_entry *current_entry,
	      uint32_t &out_offset)
{
	uint32_t compressed_size = le32toh(file->compressed_size);
	uint32_t uncompressed_size = le32toh(file->uncompressed_size);
	current_entry->compressed_size = current_entry->uncompressed_size;
	current_entry->offset = htole32(out_offset);
	current_entry->compression = 0;

	TRACE("writing entry for %s (%d %d)\n", current_entry->data, compressed_size, uncompressed_size);

	struct local_file_header *file_copy = (struct local_file_header *)malloc(file_header_size(file));
	if (!file_copy) {
		printf("couldn't allocate local file header copy\n");
		return -1;
	}
	memcpy(file_copy, file, file_header_size(file));

	file_copy->compressed_size = file_copy->uncompressed_size;
	file_copy->compression = 0;

	out_offset += simple_write(fd, (char *)file_copy, file_header_size(file));
	free(file_copy);

	if (!file->compression) {
		out_offset += simple_write(fd, (char *)file + file_header_size(file), le32toh(file->uncompressed_size));
		return 0;
	}

	char *buf = (char *)malloc(uncompressed_size);
	if (!buf) {
		printf("failed to malloc output buffer\n");
		return -1;
	}

	z_stream zstr;
	memset(&zstr, 0, sizeof(zstr));
	if (inflateInit2(&zstr, -MAX_WBITS) != Z_OK) {
		printf("inflateInit2 failed!\n");
		return -1;
	}

	zstr.next_in = (Bytef *)(file->data + le16toh(file->filename_size) + le16toh(file->extra_field_size));
	zstr.avail_in = compressed_size;
	zstr.avail_out = uncompressed_size;
	zstr.next_out = (Bytef *)buf;

	if (inflate(&zstr, Z_SYNC_FLUSH) != Z_STREAM_END &&
                    zstr.total_out != uncompressed_size) {
		printf("Failed to inflate everything total - %ld / %u\n", zstr.total_out, uncompressed_size);
		return -1;
	}

	out_offset += simple_write(fd, buf, uncompressed_size);
	free(buf);

	inflateEnd(&zstr);
	return 0;
}

static int
squeeze_entry(int fd,
	      struct local_file_header *file,
	      struct cdir_entry *current_entry,
	      uint32_t &out_offset)
{
	uint32_t uncompressed_size = le32toh(file->uncompressed_size);
	if (current_entry->compression) {
		printf("unexpected compressed entry. aborting.\n");
		return -1;
	}

	char *buf = (char *)malloc(uncompressed_size);
	if (!buf) {
		printf("failed to malloc output buffer\n");
		return -1;
	}

	z_stream zstr;
	memset(&zstr, 0, sizeof(zstr));
	if (deflateInit2(&zstr, 9, Z_DEFLATED, -MAX_WBITS, MAX_MEM_LEVEL, Z_DEFAULT_STRATEGY) != Z_OK) {
		printf("deflateInit2 failed!\n");
		return -1;
	}

	zstr.next_in = (Bytef *)(file->data + le16toh(file->filename_size) + le16toh(file->extra_field_size));
	zstr.avail_in = uncompressed_size;
	zstr.avail_out = uncompressed_size;
	zstr.next_out = (Bytef *)buf;

	uint16_t compression = 8; // Deflate
	if (deflate(&zstr, Z_FINISH) != Z_STREAM_END) {
		compression = 0;
	}

	uint32_t compressed_size = zstr.total_out;
	deflateEnd(&zstr);

	current_entry->compressed_size = htole32(compressed_size);
	current_entry->offset = htole32(out_offset);
	current_entry->compression = compression;

	TRACE("writing entry for %s (%d %d)\n", current_entry->data, current_entry->compressed_size, uncompressed_size);

	struct local_file_header *file_copy = (struct local_file_header *)malloc(file_header_size(file));
	if (!file_copy) {
		printf("couldn't allocate local file header copy\n");
		return -1;
	}
	memcpy(file_copy, file, file_header_size(file));

	file_copy->compressed_size = htole32(compressed_size);
	file_copy->compression = htole16(compression);

	out_offset += simple_write(fd, (char *)file_copy, file_header_size(file));
	free(file_copy);

	if (!compression) {
		out_offset += simple_write(fd, (char *)file + file_header_size(file), uncompressed_size);
	} else {
		out_offset += simple_write(fd, buf, compressed_size);
	}

	free(buf);
	return 0;
}

static int
repack(bool flatten, const char *dstpath, const char *srcpath)
{
	char *src_zip = (char *)map_file(srcpath);
	if (!src_zip) {
		printf("Could not open zip file.\n");
		return -1;
	}

	int fd = creat(dstpath, 0777);
	if (fd == -1) {
		printf("can't open output file\n");
		return -1;
	}

	struct cdir_end *dirend = (struct cdir_end *)(src_zip + zip_size - sizeof(*dirend));
	while ((void *)dirend > src_zip &&
	       le32toh(dirend->signature) != 0x06054b50)
		dirend = (struct cdir_end *)((char *)dirend - 1);

	if (le32toh(dirend->signature) != 0x06054b50) {
		printf("couldn't find end of central directory record!\n");
		return -1;
	}

	uint32_t cdir_offset = le32toh(dirend->cdir_offset);
	uint16_t cdir_entries = le16toh(dirend->cdir_entries);
	uint32_t cdir_size = le32toh(dirend->cdir_size);

	TRACE("Found %d entries. cdir offset at %d\n",
	      cdir_entries, cdir_offset);

	struct cdir_entry *cdir_start = (struct cdir_entry *)(src_zip + cdir_offset);
	struct cdir_entry *new_cdir_start = (struct cdir_entry *)malloc(cdir_size);
	if (!new_cdir_start) {
		TRACE("couldn't allocate central directory copy\n");
		return -1;
	}

	memcpy(new_cdir_start, cdir_start, cdir_size);

	uint32_t lowest_offset = find_lowest_offset(cdir_start, cdir_entries);	
	uint32_t out_offset = simple_write(fd, src_zip, lowest_offset);

	struct cdir_entry *current_entry = new_cdir_start;
	uint16_t i = cdir_entries;
	while (i--) {
		struct local_file_header *file = (struct local_file_header *)(src_zip + le32toh(current_entry->offset));

		int rc;
		if (flatten)
			rc = flatten_entry(fd, file, current_entry, out_offset);
		else
			rc = squeeze_entry(fd, file, current_entry, out_offset);
		if (rc)
			return rc;

		current_entry = (struct cdir_entry *)((char *)current_entry + cdir_entry_size(current_entry));
	}

	uint32_t new_cdir_offset;
	if (cdir_offset < lowest_offset) {
		TRACE("Doing in place cdir replacement at %d\n", cdir_offset);
		new_cdir_offset = cdir_offset;
		lseek(fd, SEEK_SET, cdir_offset);
		simple_write(fd, (char *)new_cdir_start, cdir_size);
		lseek(fd, SEEK_END, 0);
	} else {
		new_cdir_offset = out_offset;
		TRACE("Appending cdir at %d\n", new_cdir_offset);
		simple_write(fd, (char *)new_cdir_start, cdir_size);
	}

	struct cdir_end end;
	memcpy(&end, dirend, sizeof(end));
	end.cdir_offset = htole32(new_cdir_offset);
	simple_write(fd, (char *)&end, sizeof(end));
	close(fd);
	munmap(src_zip, zip_size);

	return 0;
}

int flatten(const char *dstpath, const char *srcpath)
{
	return repack(true, dstpath, srcpath);
}

int squeeze(const char *dstpath, const char *srcpath)
{
	return repack(false, dstpath, srcpath);
}
