/*
 * chdm-info - print information about a CHD file using the CHDmodern C API.
 * license: BSD-3-Clause
 */

#include <chdmodern/chdmodern.h>

#include <stdio.h>
#include <string.h>

static const char *kind_name(chdm_kind kind)
{
	switch (kind)
	{
	case CHDM_KIND_HARDDISK: return "hard disk";
	case CHDM_KIND_CDROM:    return "CD-ROM";
	case CHDM_KIND_GDROM:    return "GD-ROM";
	case CHDM_KIND_DVD:      return "DVD / UMD";
	case CHDM_KIND_AV:       return "A/V (laserdisc)";
	default:                 return "raw / unknown";
	}
}

static void print_sha1(const char *label, const uint8_t *sha)
{
	int i;
	printf("%-14s", label);
	for (i = 0; i < 20; i++)
		printf("%02x", sha[i]);
	printf("\n");
}

int main(int argc, char *argv[])
{
	chdm_file *file = NULL;
	chdm_info info;
	int err, i;
	uint32_t index;

	if (argc == 2 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0))
	{
		printf("CHDmodern %s\n", chdm_version());
		return 0;
	}
	if (argc != 2)
	{
		fprintf(stderr, "CHDmodern %s\nusage: %s file.chd\n       %s --version\n", chdm_version(), argv[0], argv[0]);
		return 2;
	}

	err = chdm_open(argv[1], CHDM_OPEN_READ, NULL, &file);
	if (err != CHDM_OK)
	{
		fprintf(stderr, "%s: %s\n", argv[1], chdm_last_error_message());
		return 1;
	}

	chdm_get_info(file, &info);
	printf("File:         %s\n", argv[1]);
	printf("Version:      %u\n", info.version);
	printf("Type:         %s\n", kind_name(info.kind));
	printf("Logical size: %llu bytes\n", (unsigned long long)info.logical_bytes);
	printf("Hunks:        %u x %u bytes\n", info.hunk_count, info.hunk_bytes);
	printf("Units:        %llu x %u bytes\n", (unsigned long long)info.unit_count, info.unit_bytes);
	printf("Compression:  ");
	for (i = 0; i < 4 && (i == 0 || info.compression[i]); i++)
	{
		char name[5];
		chdm_codec_name(info.compression[i], name);
		printf("%s%s", i ? ", " : "", name);
	}
	printf("\n");
	print_sha1("SHA1:", info.sha1);
	print_sha1("Data SHA1:", info.raw_sha1);
	if (info.has_parent)
	{
		print_sha1("Parent SHA1:", info.parent_sha1);
		if (info.parent_missing)
			printf("              (parent not supplied - only info is available)\n");
	}

	for (index = 0; ; index++)
	{
		char text[256];
		uint32_t length = 0, tag = 0;
		uint8_t flags = 0;
		uint32_t j;

		if (chdm_read_metadata(file, CHDM_TAG_WILDCARD, index, text, sizeof(text) - 1, &length, &tag, &flags) != CHDM_OK)
			break;
		printf("Metadata:     '%c%c%c%c' %u bytes: ",
				(char)(tag >> 24), (char)(tag >> 16), (char)(tag >> 8), (char)tag, length);
		for (j = 0; j < length && j < sizeof(text) - 1 && text[j]; j++)
			putchar((text[j] >= 32 && text[j] < 127) ? text[j] : '.');
		printf("\n");
	}

	if (info.kind == CHDM_KIND_CDROM || info.kind == CHDM_KIND_GDROM)
	{
		chdm_cd *cd = NULL;
		if (chdm_cd_open(file, &cd) == CHDM_OK)
		{
			uint32_t t, count = chdm_cd_track_count(cd);
			for (t = 0; t < count; t++)
			{
				chdm_cd_track track;
				chdm_cd_get_track(cd, t, &track);
				printf("Track %2u:     type %u, %u bytes/sector, %u frames, starts at LBA %u\n",
						t + 1, track.type, track.data_size, track.frames, track.logical_start);
			}
			chdm_cd_close(cd);
		}
	}

	chdm_close(file);
	return 0;
}
