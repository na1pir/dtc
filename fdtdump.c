/*
 * fdtdump.c - Contributed by Pantelis Antoniou <pantelis.antoniou AT gmail.com>
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <alloca.h>
#include <dirent.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <libfdt.h>
#include <libfdt_env.h>
#include <fdt.h>

#include "util.h"

#define ALIGN(x, a)	(((x) + ((a) - 1)) & ~((a) - 1))
#define PALIGN(p, a)	((void *)(ALIGN((unsigned long)(p), (a))))
#define GET_CELL(p)	(p += 4, *((const uint32_t *)(p-4)))

static const char *tagname(uint32_t tag)
{
	static const char * const names[] = {
#define TN(t) [t] #t
		TN(FDT_BEGIN_NODE),
		TN(FDT_END_NODE),
		TN(FDT_PROP),
		TN(FDT_NOP),
		TN(FDT_END),
#undef TN
	};
	if (tag < ARRAY_SIZE(names))
		if (names[tag])
			return names[tag];
	return "FDT_???";
}

#define dumpf(fmt, args...) \
	do { if (debug) printf("// " fmt, ## args); } while (0)

static void dump_blob(void *blob, bool debug)
{
	uintptr_t blob_off = (uintptr_t)blob;
	struct fdt_header *bph = blob;
	uint32_t off_mem_rsvmap = fdt32_to_cpu(bph->off_mem_rsvmap);
	uint32_t off_dt = fdt32_to_cpu(bph->off_dt_struct);
	uint32_t off_str = fdt32_to_cpu(bph->off_dt_strings);
	struct fdt_reserve_entry *p_rsvmap =
		(struct fdt_reserve_entry *)((char *)blob + off_mem_rsvmap);
	const char *p_struct = (const char *)blob + off_dt;
	const char *p_strings = (const char *)blob + off_str;
	uint32_t version = fdt32_to_cpu(bph->version);
	uint32_t totalsize = fdt32_to_cpu(bph->totalsize);
	uint32_t tag;
	const char *p, *s, *t;
	int depth, sz, shift;
	int i;
	uint64_t addr, size;

	depth = 0;
	shift = 4;

	printf("/dts-v1/;\n");
	printf("// magic:\t\t0x%x\n", fdt32_to_cpu(bph->magic));
	printf("// totalsize:\t\t0x%x (%d)\n", totalsize, totalsize);
	printf("// off_dt_struct:\t0x%x\n", off_dt);
	printf("// off_dt_strings:\t0x%x\n", off_str);
	printf("// off_mem_rsvmap:\t0x%x\n", off_mem_rsvmap);
	printf("// version:\t\t%d\n", version);
	printf("// last_comp_version:\t%d\n",
	       fdt32_to_cpu(bph->last_comp_version));
	if (version >= 2)
		printf("// boot_cpuid_phys:\t0x%x\n",
		       fdt32_to_cpu(bph->boot_cpuid_phys));

	if (version >= 3)
		printf("// size_dt_strings:\t0x%x\n",
		       fdt32_to_cpu(bph->size_dt_strings));
	if (version >= 17)
		printf("// size_dt_struct:\t0x%x\n",
		       fdt32_to_cpu(bph->size_dt_struct));
	printf("\n");

	for (i = 0; ; i++) {
		addr = fdt64_to_cpu(p_rsvmap[i].address);
		size = fdt64_to_cpu(p_rsvmap[i].size);
		if (addr == 0 && size == 0)
			break;

		printf("/memreserve/ %#llx %#llx;\n",
		       (unsigned long long)addr, (unsigned long long)size);
	}

	p = p_struct;
	while ((tag = fdt32_to_cpu(GET_CELL(p))) != FDT_END) {

		dumpf("%04zx: tag: 0x%08x (%s)\n",
		        (uintptr_t)p - blob_off - 4, tag, tagname(tag));

		if (tag == FDT_BEGIN_NODE) {
			s = p;
			p = PALIGN(p + strlen(s) + 1, 4);

			if (*s == '\0')
				s = "/";

			printf("%*s%s {\n", depth * shift, "", s);

			depth++;
			continue;
		}

		if (tag == FDT_END_NODE) {
			depth--;

			printf("%*s};\n", depth * shift, "");
			continue;
		}

		if (tag == FDT_NOP) {
			printf("%*s// [NOP]\n", depth * shift, "");
			continue;
		}

		if (tag != FDT_PROP) {
			fprintf(stderr, "%*s ** Unknown tag 0x%08x\n", depth * shift, "", tag);
			break;
		}
		sz = fdt32_to_cpu(GET_CELL(p));
		s = p_strings + fdt32_to_cpu(GET_CELL(p));
		if (version < 16 && sz >= 8)
			p = PALIGN(p, 8);
		t = p;

		p = PALIGN(p + sz, 4);

		dumpf("%04zx: string: %s\n", (uintptr_t)s - blob_off, s);
		dumpf("%04zx: value\n", (uintptr_t)t - blob_off);
		printf("%*s%s", depth * shift, "", s);
		utilfdt_print_data(t, sz);
		printf(";\n");
	}
}

static void dump_live_internal(const char *path, bool debug, int depth)
{
	int maxsz = strlen(path) + 1 + PATH_MAX;
	char *new_path = alloca(maxsz + 1);
	struct stat sb;
	struct dirent *de;
	char *buf, *p;
	int buf_alloc, shift, chunk, left, fd, ret;
	DIR *d;

	shift = 4;
	buf_alloc = 4 * 1024;	/* 4K (maximum chunk) */
	buf = alloca(buf_alloc + sizeof(uint32_t));
	buf[buf_alloc] = '\0';	/* always terminate (just in case) */

	d = opendir(path);
	if (d == NULL)
		die("Could not open %s directory\n", path);

	/* first dump the properties (files) */
	while ((de = readdir(d)) != NULL) {
		/* properties are files */
		if (de->d_type != DT_REG)
			continue;
		snprintf(new_path, maxsz, "%s/%s", path, de->d_name);
		new_path[maxsz] = '\0';
		printf("%*s%s", depth * shift, "", de->d_name);

		if (stat(new_path, &sb) != 0)
			die("could not open: %s\n", new_path);

		fd = open(new_path, O_RDONLY);
		if (fd == -1)
			die("Could not open: %s\n", new_path);

		chunk = sb.st_size > buf_alloc ? buf_alloc : sb.st_size;
		p = buf;
		left = chunk;
		while (left > 0) {
			do {
				ret = read(fd, p, left);
			} while (ret == -1 && (errno == EAGAIN || errno == EINTR));
			if (ret == -1)
				die("Read failed on: %s\n", new_path);
			left -= ret;
			p += ret;
		}
		close(fd);

		if (chunk < sb.st_size)
			printf(" (trunc)");
		utilfdt_print_data(buf, chunk);
		printf(";\n");
	}

	/* now recurse to the directories */
	rewinddir(d);
	while ((de = readdir(d)) != NULL) {
		/* properties are files */
		if (de->d_type != DT_DIR)
			continue;
		/* skip current and parent directories */
		if (strcmp(de->d_name, ".") == 0 ||
				strcmp(de->d_name, "..") == 0)
			continue;
		snprintf(new_path, maxsz, "%s/%s", path, de->d_name);
		new_path[maxsz] = '\0';
		printf("%*s%s {\n", depth * shift, "", de->d_name);
		dump_live_internal(new_path, debug, depth + 1);
		printf("%*s};\n", depth * shift, "");
	}
}

static void dump_live(const char *path, bool debug)
{
	char *fixed_path = alloca(strlen(path) + 1);
	char *p;

	/* strip trailing / */
	strcpy(fixed_path, path);
	p = fixed_path + strlen(fixed_path) - 1;
	while (*p == '/' && p > fixed_path)
		*p-- = '\0';
	printf("/* dump of live tree at %s */\n", fixed_path);
	printf("/ {\n");
	dump_live_internal(fixed_path, debug, 1);
	printf("};\n");
}

/* Usage related data. */
static const char usage_synopsis[] = "fdtdump [options] <file>";
static const char usage_short_opts[] = "ds" USAGE_COMMON_SHORT_OPTS;
static struct option const usage_long_opts[] = {
	{"debug",            no_argument, NULL, 'd'},
	{"scan",             no_argument, NULL, 's'},
	USAGE_COMMON_LONG_OPTS
};
static const char * const usage_opts_help[] = {
	"Dump debug information while decoding the file",
	"Scan for an embedded fdt in file",
	USAGE_COMMON_OPTS_HELP
};

int main(int argc, char *argv[])
{
	int opt;
	const char *file;
	char *buf;
	bool debug = false;
	bool scan = false;
	off_t len;
	struct stat sb;

	while ((opt = util_getopt_long()) != EOF) {
		switch (opt) {
		case_USAGE_COMMON_FLAGS

		case 'd':
			debug = true;
			break;
		case 's':
			scan = true;
			break;
		}
	}
	if (optind != argc - 1)
		usage("missing input filename");
	file = argv[optind];

	if (stat(file, &sb) != 0)
		die("could not open: %s\n", file);

	/* dump live tree if it's a directory */
	if (S_ISDIR(sb.st_mode)) {
		dump_live(file, debug);
		return 0;
	}

	buf = utilfdt_read_len(file, &len);
	if (!buf)
		die("could not read: %s\n", file);

	/* try and locate an embedded fdt in a bigger blob */
	if (scan) {
		unsigned char smagic[4];
		char *p = buf;
		char *endp = buf + len;

		fdt_set_magic(smagic, FDT_MAGIC);

		/* poor man's memmem */
		while (true) {
			p = memchr(p, smagic[0], endp - p - 4);
			if (!p)
				break;
			if (fdt_magic(p) == FDT_MAGIC) {
				/* try and validate the main struct */
				off_t this_len = endp - p;
				fdt32_t max_version = 17;
				if (fdt_version(p) <= max_version &&
				    fdt_last_comp_version(p) < max_version &&
				    fdt_totalsize(p) < this_len &&
				    fdt_off_dt_struct(p) < this_len &&
					fdt_off_dt_strings(p) < this_len)
					break;
				if (debug)
					printf("%s: skipping fdt magic at offset %#zx\n",
						file, p - buf);
			}
			++p;
		}
		if (!p)
			die("%s: could not locate fdt magic\n", file);
		printf("%s: found fdt at offset %#zx\n", file, p - buf);
		buf = p;
	}

	dump_blob(buf, debug);

	return 0;
}
