/*
 * dlltest - checks that a program can use CHDmodern purely through the C
 * API, as it would through CHDm.dll / CHDm.so.
 *
 * usage: chdm-dlltest path/to/tests/data/sample_dvd_zlib.chd
 *
 * The sample holds 64 KiB of pattern data (byte i = i*7 + i/2048) as a
 * zlib-compressed DVD CHD with 2048-byte sectors.
 *
 * license: BSD-3-Clause
 */

#include <chdmodern/chdmodern.h>

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond) \
	do { if (!(cond)) { printf("FAILED line %d: %s (%s)\n", __LINE__, #cond, chdm_last_error_message()); failures++; } } while (0)

static unsigned char pattern(uint64_t i)
{
	return (unsigned char)(i * 7 + i / 2048);
}

int main(int argc, char *argv[])
{
	chdm_file *file = NULL;
	chdm_info info;
	unsigned char buf[16384];
	char codec[5];
	uint32_t i, h;

	if (argc != 2)
	{
		fprintf(stderr, "usage: %s sample_dvd_zlib.chd\n", argv[0]);
		return 2;
	}

	printf("CHDmodern %s (header %s)\n", chdm_version(), CHDMODERN_VERSION_STRING);
	CHECK(strcmp(chdm_version(), CHDMODERN_VERSION_STRING) == 0);
	CHECK(chdm_version_number() == CHDMODERN_VERSION_NUMBER);
	CHECK(strcmp(chdm_error_string(CHDM_ERR_NOT_FOUND), "file not found") == 0);
	CHECK(chdm_open("this-file-does-not-exist.chd", CHDM_OPEN_READ, NULL, &file) == CHDM_ERR_NOT_FOUND);

	CHECK(chdm_open(argv[1], CHDM_OPEN_READ, NULL, &file) == CHDM_OK);
	if (!file)
		return 1;

	CHECK(chdm_get_info(file, &info) == CHDM_OK);
	CHECK(info.kind == CHDM_KIND_DVD);
	CHECK(info.logical_bytes == 65536 && info.unit_bytes == 2048 && info.hunk_bytes == 16384);
	chdm_codec_name(info.compression[0], codec);
	CHECK(strcmp(codec, "zlib") == 0);

	for (h = 0; h < info.hunk_count; h++)
	{
		CHECK(chdm_read_hunk(file, h, buf) == CHDM_OK);
		for (i = 0; i < info.hunk_bytes; i++)
		{
			if (buf[i] != pattern((uint64_t)h * info.hunk_bytes + i))
			{
				printf("FAILED: hunk %u byte %u differs\n", h, i);
				failures++;
				break;
			}
		}
	}

	CHECK(chdm_read_units(file, 5, buf, 2) == CHDM_OK);
	CHECK(buf[0] == pattern(5 * 2048) && buf[4095] == pattern(7 * 2048 - 1));
	CHECK(chdm_read_bytes(file, 30000, buf, 100) == CHDM_OK);
	CHECK(buf[0] == pattern(30000) && buf[99] == pattern(30099));
	CHECK(chdm_read_hunk(file, info.hunk_count, buf) == CHDM_ERR_HUNK_OUT_OF_RANGE);

	chdm_close(file);

	/* whole-image conversion: CHD -> ISO -> CHD (zstd, 2048-byte hunks) */
	{
		const char *iso = "chdm-dlltest-out.iso";
		const char *chd = "chdm-dlltest-out.chd";
		const uint32_t zstd[4] = { CHDM_TAG('z','s','t','d'), 0, 0, 0 };
		FILE *f;

		CHECK(chdm_extract_raw(argv[1], iso, NULL, NULL) == CHDM_OK);
		f = fopen(iso, "rb");
		CHECK(f != NULL);
		if (f)
		{
			size_t n = 0, got;
			while ((got = fread(buf, 1, sizeof(buf), f)) > 0)
			{
				for (i = 0; i < got; i++)
				{
					if (buf[i] != pattern(n + i))
					{
						printf("FAILED: extracted byte %u differs\n", (unsigned)(n + i));
						failures++;
						break;
					}
				}
				n += got;
			}
			fclose(f);
			CHECK(n == 65536);
		}

		CHECK(chdm_create_dvd(iso, chd, 2048, zstd, NULL, NULL) == CHDM_OK);
		file = NULL;
		CHECK(chdm_open(chd, CHDM_OPEN_READ, NULL, &file) == CHDM_OK);
		if (file)
		{
			CHECK(chdm_get_info(file, &info) == CHDM_OK);
			CHECK(info.kind == CHDM_KIND_DVD && info.hunk_bytes == 2048 && info.logical_bytes == 65536);
			CHECK(chdm_read_bytes(file, 40000, buf, 10) == CHDM_OK);
			CHECK(buf[0] == pattern(40000) && buf[9] == pattern(40009));
			chdm_close(file);
		}
		remove(iso);
		remove(chd);
	}

	printf(failures ? "%d check(s) FAILED\n" : "DLL test passed\n", failures);
	return failures ? 1 : 0;
}
