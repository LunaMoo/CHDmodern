/*
 * chdm-extract - extract the logical data of a CHD to a flat file.
 *
 * DVD/UMD and hard-disk images are written as-is with chdm_extract_raw()
 * (for PSP UMD CHDs this gives back the original .iso).  For CD-ROM images the 2048-byte user data
 * of the first data track is written, i.e. an .iso of that track.
 *
 * license: BSD-3-Clause
 */

#include <chdmodern/chdmodern.h>

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[])
{
	chdm_file *file = NULL;
	chdm_info info;
	FILE *out;
	int err;

	if (argc != 3)
	{
		fprintf(stderr, "usage: %s input.chd output.iso\n", argv[0]);
		return 2;
	}

	err = chdm_open(argv[1], CHDM_OPEN_READ, NULL, &file);
	if (err != CHDM_OK)
	{
		fprintf(stderr, "%s: %s\n", argv[1], chdm_last_error_message());
		return 1;
	}
	chdm_get_info(file, &info);

	if (info.kind != CHDM_KIND_CDROM && info.kind != CHDM_KIND_GDROM)
	{
		/* the library writes through its own 64-bit-offset file layer, so
		   this works for images of any size on every platform */
		chdm_close(file);
		err = chdm_extract_raw(argv[1], argv[2], NULL, NULL);
		if (err != CHDM_OK)
		{
			fprintf(stderr, "%s: %s\n", argv[1], chdm_last_error_message());
			return 1;
		}
		return 0;
	}

	out = fopen(argv[2], "wb");
	if (!out)
	{
		perror(argv[2]);
		chdm_close(file);
		return 1;
	}

	{
		chdm_cd *cd = NULL;
		chdm_cd_track track;
		uint32_t t, count, lba;
		unsigned char sector[2352];

		if ((err = chdm_cd_open(file, &cd)) != CHDM_OK)
			goto fail;
		count = chdm_cd_track_count(cd);
		for (t = 0; t < count; t++)
		{
			chdm_cd_get_track(cd, t, &track);
			if (track.type != CHDM_CD_AUDIO)
				break;
		}
		if (t == count)
		{
			fprintf(stderr, "no data track found\n");
			chdm_cd_close(cd);
			fclose(out);
			chdm_close(file);
			remove(argv[2]);
			return 1;
		}
		for (lba = track.logical_start; lba < track.logical_start + track.frames; lba++)
		{
			if ((err = chdm_cd_read_sector(cd, lba, sector, CHDM_CD_MODE1)) != CHDM_OK)
			{
				chdm_cd_close(cd);
				goto fail;
			}
			if (fwrite(sector, 1, 2048, out) != 2048)
			{
				perror(argv[2]);
				chdm_cd_close(cd);
				fclose(out);
				chdm_close(file);
				remove(argv[2]);
				return 1;
			}
		}
		chdm_cd_close(cd);
	}

	chdm_close(file);
	if (fclose(out) != 0)
	{
		perror(argv[2]);
		remove(argv[2]);
		return 1;
	}
	return 0;

fail:
	fprintf(stderr, "%s: %s\n", argv[1], chdm_last_error_message());
	fclose(out);
	chdm_close(file);
	remove(argv[2]);
	return 1;
}
