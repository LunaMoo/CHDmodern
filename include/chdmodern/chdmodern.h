/*
 * CHDmodern - MAME's CHD (Compressed Hunks of Data) implementation as a
 * standalone library.
 *
 * license: BSD-3-Clause
 *
 * This is the stable C API.  It is plain C89-compatible so it can be used
 * from C, C++ (any standard), and through FFI from other languages.  C++
 * users who want the full MAME API (including CHD creation/compression)
 * can link the same library and #include "chd.h" - the CMake target
 * exports MAME's util/osd include directories (see README).
 *
 * All functions are thread-compatible: separate handles may be used from
 * separate threads, a single handle must not be used concurrently.
 */

#ifndef CHDMODERN_CHDMODERN_H
#define CHDMODERN_CHDMODERN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(CHDMODERN_SHARED)
#  if defined(_WIN32)
#    if defined(CHDMODERN_BUILDING)
#      define CHDM_API __declspec(dllexport)
#    else
#      define CHDM_API __declspec(dllimport)
#    endif
#  else
#    define CHDM_API __attribute__((visibility("default")))
#  endif
#else
#  define CHDM_API
#endif

#define CHDMODERN_VERSION_MAJOR 0
#define CHDMODERN_VERSION_MINOR 2
#define CHDMODERN_VERSION_PATCH 0

/* ---------------------------------------------------------------------- */
/*  Errors                                                                */
/* ---------------------------------------------------------------------- */

/* Values 1..99 mirror MAME's chd_file::error enumeration exactly. */
typedef enum chdm_error
{
	CHDM_OK                         = 0,

	CHDM_ERR_NO_INTERFACE           = 1,
	CHDM_ERR_NOT_OPEN               = 2,
	CHDM_ERR_ALREADY_OPEN           = 3,
	CHDM_ERR_INVALID_FILE           = 4,
	CHDM_ERR_INVALID_DATA           = 5,
	CHDM_ERR_REQUIRES_PARENT        = 6,
	CHDM_ERR_FILE_NOT_WRITEABLE     = 7,
	CHDM_ERR_CODEC_ERROR            = 8,
	CHDM_ERR_INVALID_PARENT         = 9,
	CHDM_ERR_HUNK_OUT_OF_RANGE      = 10,
	CHDM_ERR_DECOMPRESSION_ERROR    = 11,
	CHDM_ERR_COMPRESSION_ERROR      = 12,
	CHDM_ERR_CANT_VERIFY            = 13,
	CHDM_ERR_METADATA_NOT_FOUND     = 14,
	CHDM_ERR_INVALID_METADATA_SIZE  = 15,
	CHDM_ERR_UNSUPPORTED_VERSION    = 16,
	CHDM_ERR_VERIFY_INCOMPLETE      = 17,
	CHDM_ERR_INVALID_METADATA       = 18,
	CHDM_ERR_INVALID_STATE          = 19,
	CHDM_ERR_OPERATION_PENDING      = 20,
	CHDM_ERR_UNSUPPORTED_FORMAT     = 21,
	CHDM_ERR_UNKNOWN_COMPRESSION    = 22,
	CHDM_ERR_WALKING_PARENT         = 23,
	CHDM_ERR_COMPRESSING            = 24,

	/* system / generic errors */
	CHDM_ERR_IO                     = 100,
	CHDM_ERR_NOT_FOUND              = 101,
	CHDM_ERR_ACCESS_DENIED          = 102,
	CHDM_ERR_OUT_OF_MEMORY          = 103,
	CHDM_ERR_INVALID_ARGUMENT       = 104,
	CHDM_ERR_NOT_SUPPORTED          = 105,
	CHDM_ERR_CANCELLED              = 106,  /* a progress callback asked to stop */
	CHDM_ERR_UNKNOWN                = 199
} chdm_error;

/* Short static description of an error code. */
CHDM_API const char *chdm_error_string(int err);

/* Detailed message for the last error raised on the calling thread
   (includes the underlying system message where available).  Never NULL. */
CHDM_API const char *chdm_last_error_message(void);

/* ---------------------------------------------------------------------- */
/*  Diagnostics                                                           */
/* ---------------------------------------------------------------------- */

typedef enum chdm_log_level
{
	CHDM_LOG_ERROR = 0,
	CHDM_LOG_WARNING,
	CHDM_LOG_INFO,
	CHDM_LOG_DEBUG,
	CHDM_LOG_VERBOSE
} chdm_log_level;

typedef void (*chdm_log_func)(void *user, int level, const char *message);

/* Receive the diagnostic messages MAME's CHD code prints (silently dropped
   by default).  Pass NULL to stop.  Process-wide. */
CHDM_API void chdm_set_log_callback(chdm_log_func func, void *user);

/* ---------------------------------------------------------------------- */
/*  Opening                                                               */
/* ---------------------------------------------------------------------- */

typedef struct chdm_file chdm_file;

/* Custom read-only I/O, e.g. to read through a host application's own file
   layer, an archive, or a network stream.  read_at must be safe to call
   repeatedly with arbitrary offsets; it returns the number of bytes read
   (short reads only at end of file) or (size_t)-1 on error. */
typedef struct chdm_io
{
	void *user;
	uint64_t (*size)(void *user);
	size_t   (*read_at)(void *user, uint64_t offset, void *buffer, size_t length);
	void     (*close)(void *user);  /* optional, called once from chdm_close */
} chdm_io;

#define CHDM_OPEN_READ      0
#define CHDM_OPEN_WRITE     1

/* Open a CHD by UTF-8 path.  parent may be NULL; it must outlive the child. */
CHDM_API int chdm_open(const char *path, int mode, chdm_file *parent, chdm_file **out);

/* Open a CHD through custom read-only I/O.  The io struct is copied.  On
   failure io->close is NOT called; the caller still owns io->user. */
CHDM_API int chdm_open_io(const chdm_io *io, chdm_file *parent, chdm_file **out);

/* Called when a child CHD needs its parent.  Return the opened parent (by
   any chdm_open* call; it may itself use a find_parent callback, so whole
   chains resolve) or NULL if not found.  The child takes ownership of the
   returned file: do not close it.  Its SHA-1 is verified against the
   child's header (CHDM_ERR_INVALID_PARENT on mismatch). */
typedef chdm_file *(*chdm_find_parent_func)(void *user, const uint8_t parent_sha1[20]);

CHDM_API int chdm_open_ex(const char *path, int mode, chdm_find_parent_func find_parent, void *user, chdm_file **out);
CHDM_API int chdm_open_io_ex(const chdm_io *io, chdm_find_parent_func find_parent, void *user, chdm_file **out);

/* Close a file (NULL is ignored). */
CHDM_API void chdm_close(chdm_file *file);

/* ---------------------------------------------------------------------- */
/*  Information                                                           */
/* ---------------------------------------------------------------------- */

typedef enum chdm_kind
{
	CHDM_KIND_UNKNOWN = 0,  /* raw data or unrecognised metadata */
	CHDM_KIND_HARDDISK,
	CHDM_KIND_CDROM,
	CHDM_KIND_GDROM,
	CHDM_KIND_DVD,          /* includes PSP UMD images made with "chdman createdvd" */
	CHDM_KIND_AV            /* laserdisc A/V */
} chdm_kind;

typedef struct chdm_info
{
	uint32_t  version;          /* CHD header version (3, 4 or 5) */
	uint32_t  hunk_bytes;       /* bytes per hunk */
	uint32_t  hunk_count;       /* number of hunks */
	uint32_t  unit_bytes;       /* bytes per unit (sector) */
	uint64_t  unit_count;       /* number of units */
	uint64_t  logical_bytes;    /* uncompressed size */
	uint32_t  compression[4];   /* codec FourCCs, 0 = unused / uncompressed */
	int       compressed;       /* nonzero if any codec is in use */
	int       has_parent;       /* nonzero if this CHD is a child of another CHD */
	int       parent_missing;   /* nonzero if it is a child but no parent was
	                               supplied; hunks stored in the parent then
	                               fail with CHDM_ERR_REQUIRES_PARENT */
	chdm_kind kind;
	uint8_t   sha1[20];         /* overall SHA-1 (data + metadata) */
	uint8_t   raw_sha1[20];     /* SHA-1 of the raw data only */
	uint8_t   parent_sha1[20];  /* SHA-1 of the parent, zero if none */
} chdm_info;

CHDM_API int chdm_get_info(chdm_file *file, chdm_info *info);

/* Write the 4-character name of a codec FourCC ("zlib", "cdlz", ...) to
   out (5 bytes incl. terminator). */
CHDM_API void chdm_codec_name(uint32_t codec, char out[5]);

/* ---------------------------------------------------------------------- */
/*  Reading                                                               */
/* ---------------------------------------------------------------------- */

/* buffer must hold info.hunk_bytes bytes. */
CHDM_API int chdm_read_hunk(chdm_file *file, uint32_t hunk, void *buffer);

/* Read count whole units (sectors); buffer must hold count * unit_bytes. */
CHDM_API int chdm_read_units(chdm_file *file, uint64_t unit, void *buffer, uint32_t count);

/* Read an arbitrary byte range of the logical (uncompressed) data. */
CHDM_API int chdm_read_bytes(chdm_file *file, uint64_t offset, void *buffer, uint32_t length);

/* Build a metadata tag from four characters, e.g. CHDM_TAG('G','D','D','D'). */
#define CHDM_TAG(a, b, c, d) \
	(((uint32_t)(uint8_t)(a) << 24) | ((uint32_t)(uint8_t)(b) << 16) | ((uint32_t)(uint8_t)(c) << 8) | (uint32_t)(uint8_t)(d))
#define CHDM_TAG_WILDCARD 0u

/* Read the index-th metadata entry with the given tag (or any tag with
   CHDM_TAG_WILDCARD).  Up to buffer_size bytes are copied; *out_length (if
   not NULL) receives the full entry length so callers can detect
   truncation.  out_tag / out_flags may be NULL. */
CHDM_API int chdm_read_metadata(chdm_file *file, uint32_t tag, uint32_t index,
		void *buffer, uint32_t buffer_size, uint32_t *out_length,
		uint32_t *out_tag, uint8_t *out_flags);

/* ---------------------------------------------------------------------- */
/*  CD-ROM / GD-ROM helper                                                */
/* ---------------------------------------------------------------------- */

typedef struct chdm_cd chdm_cd;

/* Matches MAME's cdrom_file track types. */
typedef enum chdm_cd_track_type
{
	CHDM_CD_MODE1 = 0,          /* 2048 bytes/sector */
	CHDM_CD_MODE1_RAW,          /* 2352 */
	CHDM_CD_MODE2,              /* 2336 */
	CHDM_CD_MODE2_FORM1,        /* 2048 */
	CHDM_CD_MODE2_FORM2,        /* 2324 */
	CHDM_CD_MODE2_FORM_MIX,     /* 2336 */
	CHDM_CD_MODE2_RAW,          /* 2352 */
	CHDM_CD_AUDIO,              /* 2352 */
	CHDM_CD_RAW_DONTCARE        /* read whatever is stored */
} chdm_cd_track_type;

typedef struct chdm_cd_track
{
	uint32_t type;              /* chdm_cd_track_type */
	uint32_t subcode_type;      /* 0 normal, 1 raw, 2 none */
	uint32_t data_size;         /* bytes of data per sector */
	uint32_t subcode_size;      /* bytes of subcode per sector */
	uint32_t frames;            /* frames in this track */
	uint32_t pregap;            /* pregap frames */
	uint32_t postgap;           /* postgap frames */
	uint32_t session;           /* session number */
	uint32_t logical_start;     /* first logical frame (LBA) */
	uint32_t physical_start;    /* first physical frame */
	uint32_t control_flags;
} chdm_cd_track;

/* The chdm_file must stay open for the lifetime of the chdm_cd. */
CHDM_API int  chdm_cd_open(chdm_file *file, chdm_cd **out);
CHDM_API void chdm_cd_close(chdm_cd *cd);

CHDM_API uint32_t chdm_cd_track_count(chdm_cd *cd);
CHDM_API int      chdm_cd_get_track(chdm_cd *cd, uint32_t track, chdm_cd_track *out);
/* Track (0-based) containing a logical frame. */
CHDM_API uint32_t chdm_cd_track_for_lba(chdm_cd *cd, uint32_t lba);

/* Read one sector converted to the given type (e.g. CHDM_CD_MODE1 for the
   2048-byte user data of a raw sector).  buffer needs 2352 bytes. */
CHDM_API int chdm_cd_read_sector(chdm_cd *cd, uint32_t lba, void *buffer, uint32_t type);

/* Read 96 bytes of subcode for one sector. */
CHDM_API int chdm_cd_read_subcode(chdm_cd *cd, uint32_t lba, void *buffer);

/* ---------------------------------------------------------------------- */
/*  Converting whole images                                               */
/* ---------------------------------------------------------------------- */

/* Progress callback for the conversion functions below, called from the
   calling thread with progress in [0, 1].  Return nonzero to cancel; the
   function then returns CHDM_ERR_CANCELLED and deletes the partial output. */
typedef int (*chdm_progress_func)(void *user, double progress);

/* chdman createdvd's default hunk size. */
#define CHDM_DVD_HUNK_BYTES_DEFAULT 4096u

/* Compress a 2048-byte-sector image (a DVD or PSP UMD ISO) into a DVD CHD,
   like "chdman createdvd".  hunk_bytes must be a multiple of 2048 (0 means
   CHDM_DVD_HUNK_BYTES_DEFAULT).  compression lists up to four codec
   FourCCs (CHDM_TAG('z','s','t','d'), ...; unused entries 0), or NULL for
   chdman's defaults (LZMA, zlib, Huffman, FLAC).  An existing output file
   is overwritten.  progress may be NULL. */
CHDM_API int chdm_create_dvd(const char *input_path, const char *output_path, uint32_t hunk_bytes,
		const uint32_t compression[4], chdm_progress_func progress, void *user);

/* Write the logical (uncompressed) data of a CHD to a file, e.g. a DVD CHD
   back to its ISO, like "chdman extractdvd".  CD-ROM, GD-ROM and A/V CHDs
   are refused with CHDM_ERR_UNSUPPORTED_FORMAT, since their raw data isn't
   a usable image.  An existing output file is overwritten.  progress may
   be NULL. */
CHDM_API int chdm_extract_raw(const char *input_path, const char *output_path,
		chdm_progress_func progress, void *user);

#ifdef __cplusplus
}
#endif

#endif /* CHDMODERN_CHDMODERN_H */
