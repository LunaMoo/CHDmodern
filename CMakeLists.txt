cmake_minimum_required(VERSION 3.20)

# The version lives in the public header (see the comment there).
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/include/chdmodern/chdmodern.h CHDMODERN_VERSION_DEFINES
	REGEX "^#define CHDMODERN_VERSION_(MAJOR|MINOR|PATCH) [0-9]+$")
foreach(part MAJOR MINOR PATCH)
	string(REGEX REPLACE ".*#define CHDMODERN_VERSION_${part} ([0-9]+).*" "\\1" CHDMODERN_VERSION_${part} "${CHDMODERN_VERSION_DEFINES}")
endforeach()

project(CHDmodern
	VERSION ${CHDMODERN_VERSION_MAJOR}.${CHDMODERN_VERSION_MINOR}.${CHDMODERN_VERSION_PATCH}
	DESCRIPTION "MAME's CHD (Compressed Hunks of Data) code as a standalone library"
	LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------

get_directory_property(CHDMODERN_HAS_PARENT PARENT_DIRECTORY)
if(CHDMODERN_HAS_PARENT)
	set(CHDMODERN_TOPLEVEL OFF)
else()
	set(CHDMODERN_TOPLEVEL ON)
endif()

option(CHDMODERN_SHARED       "Build a shared library instead of a static one" OFF)
option(CHDMODERN_BUILD_TOOLS  "Build the chdm-info / chdm-extract example tools" ${CHDMODERN_TOPLEVEL})
option(CHDMODERN_BUILD_TESTS  "Build the self-tests" ${CHDMODERN_TOPLEVEL})
option(CHDMODERN_INSTALL      "Generate install rules" ${CHDMODERN_TOPLEVEL})
option(CHDMODERN_NO_THREADS   "Run CHD work items synchronously (targets without std::thread)" OFF)

# Third-party libraries.  By default the copies bundled in third_party/ (the
# exact versions MAME uses) are built.  A host project that already links its
# own zlib/zstd/... can avoid duplicate symbols either by switching on the
# SYSTEM option (find_package) or by setting CHDMODERN_<LIB>_TARGET to the
# name of an existing CMake target before add_subdirectory(CHDmodern).
option(CHDMODERN_SYSTEM_ZLIB  "Use the system zlib (find_package)" OFF)
option(CHDMODERN_SYSTEM_ZSTD  "Use the system zstd (find_package)" OFF)
option(CHDMODERN_SYSTEM_FLAC  "Use the system libFLAC (find_package)" OFF)
set(CHDMODERN_ZLIB_TARGET "" CACHE STRING "Existing CMake target providing zlib")
set(CHDMODERN_ZSTD_TARGET "" CACHE STRING "Existing CMake target providing zstd")
set(CHDMODERN_FLAC_TARGET "" CACHE STRING "Existing CMake target providing libFLAC")
set(CHDMODERN_LZMA_TARGET "" CACHE STRING "Existing CMake target providing the LZMA SDK (LzmaDec/LzmaEnc)")

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# The bundled third-party code is an implementation detail: keep its symbols
# out of the shared CHDm library's export table.
set(CMAKE_C_VISIBILITY_PRESET hidden)

if(CMAKE_C_BYTE_ORDER STREQUAL "BIG_ENDIAN")
	set(CHDMODERN_BIG_ENDIAN ON)
else()
	set(CHDMODERN_BIG_ENDIAN OFF)
endif()

if(MSVC)
	set(CHDMODERN_WARN_OFF /w)
else()
	set(CHDMODERN_WARN_OFF -w)
endif()

# ---------------------------------------------------------------------------
# Third-party dependencies
# ---------------------------------------------------------------------------

set(TP ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# zlib -----------------------------------------------------------------------
if(CHDMODERN_ZLIB_TARGET)
	set(CHDM_ZLIB ${CHDMODERN_ZLIB_TARGET})
elseif(CHDMODERN_SYSTEM_ZLIB)
	find_package(ZLIB REQUIRED)
	set(CHDM_ZLIB ZLIB::ZLIB)
else()
	# only the deflate/inflate stream API is used
	add_library(chdm_zlib STATIC
		${TP}/zlib/adler32.c ${TP}/zlib/crc32.c ${TP}/zlib/deflate.c
		${TP}/zlib/inffast.c ${TP}/zlib/inflate.c ${TP}/zlib/inftrees.c
		${TP}/zlib/trees.c ${TP}/zlib/zutil.c)
	target_include_directories(chdm_zlib PUBLIC $<BUILD_INTERFACE:${TP}/zlib>)
	target_compile_definitions(chdm_zlib PRIVATE $<$<NOT:$<C_COMPILER_ID:MSVC>>:HAVE_UNISTD_H>)
	target_compile_options(chdm_zlib PRIVATE ${CHDMODERN_WARN_OFF})
	set(CHDM_ZLIB chdm_zlib)
endif()

# zstd -----------------------------------------------------------------------
if(CHDMODERN_ZSTD_TARGET)
	set(CHDM_ZSTD ${CHDMODERN_ZSTD_TARGET})
elseif(CHDMODERN_SYSTEM_ZSTD)
	find_package(PkgConfig REQUIRED)
	pkg_check_modules(CHDM_PC_ZSTD REQUIRED IMPORTED_TARGET libzstd)
	set(CHDM_ZSTD PkgConfig::CHDM_PC_ZSTD)
else()
	file(GLOB CHDM_ZSTD_SOURCES
		${TP}/zstd/lib/common/*.c
		${TP}/zstd/lib/compress/*.c
		${TP}/zstd/lib/decompress/*.c)
	add_library(chdm_zstd STATIC ${CHDM_ZSTD_SOURCES})
	target_include_directories(chdm_zstd PUBLIC $<BUILD_INTERFACE:${TP}/zstd/lib>)
	target_compile_definitions(chdm_zstd PRIVATE XXH_NAMESPACE=ZSTD_ DEBUGLEVEL=0 ZSTD_DISABLE_ASM
		ZSTDLIB_VISIBLE= ZSTDERRORLIB_VISIBLE= ZDICTLIB_VISIBLE=)
	target_compile_options(chdm_zstd PRIVATE ${CHDMODERN_WARN_OFF})
	set(CHDM_ZSTD chdm_zstd)
endif()

# LZMA SDK (MAME only needs the LZMA codec, built single-threaded) -----------
if(CHDMODERN_LZMA_TARGET)
	set(CHDM_LZMA ${CHDMODERN_LZMA_TARGET})
else()
	add_library(chdm_lzma STATIC
		${TP}/lzma/C/CpuArch.c ${TP}/lzma/C/LzFind.c ${TP}/lzma/C/LzFindOpt.c
		${TP}/lzma/C/LzmaDec.c ${TP}/lzma/C/LzmaEnc.c)
	# chdcodec.cpp includes "lzma/C/LzmaDec.h", so expose the parent directory
	target_include_directories(chdm_lzma PUBLIC $<BUILD_INTERFACE:${TP}>)
	target_compile_definitions(chdm_lzma PUBLIC Z7_ST)
	target_compile_options(chdm_lzma PRIVATE ${CHDMODERN_WARN_OFF})
	set(CHDM_LZMA chdm_lzma)
endif()

# libFLAC --------------------------------------------------------------------
if(CHDMODERN_FLAC_TARGET)
	set(CHDM_FLAC ${CHDMODERN_FLAC_TARGET})
elseif(CHDMODERN_SYSTEM_FLAC)
	find_package(PkgConfig REQUIRED)
	pkg_check_modules(CHDM_PC_FLAC REQUIRED IMPORTED_TARGET flac)
	set(CHDM_FLAC PkgConfig::CHDM_PC_FLAC)
else()
	set(FLAC_SRC ${TP}/flac/src/libFLAC)
	add_library(chdm_flac STATIC
		${FLAC_SRC}/bitmath.c ${FLAC_SRC}/bitreader.c ${FLAC_SRC}/bitwriter.c
		${FLAC_SRC}/cpu.c ${FLAC_SRC}/crc.c ${FLAC_SRC}/fixed.c
		${FLAC_SRC}/fixed_intrin_avx2.c ${FLAC_SRC}/fixed_intrin_sse2.c
		${FLAC_SRC}/fixed_intrin_sse42.c ${FLAC_SRC}/fixed_intrin_ssse3.c
		${FLAC_SRC}/float.c ${FLAC_SRC}/format.c ${FLAC_SRC}/lpc.c
		${FLAC_SRC}/lpc_intrin_avx2.c ${FLAC_SRC}/lpc_intrin_fma.c
		${FLAC_SRC}/lpc_intrin_neon.c ${FLAC_SRC}/lpc_intrin_sse2.c
		${FLAC_SRC}/lpc_intrin_sse41.c ${FLAC_SRC}/md5.c ${FLAC_SRC}/memory.c
		${FLAC_SRC}/stream_decoder.c ${FLAC_SRC}/stream_encoder.c
		${FLAC_SRC}/stream_encoder_framing.c ${FLAC_SRC}/stream_encoder_intrin_avx2.c
		${FLAC_SRC}/stream_encoder_intrin_sse2.c ${FLAC_SRC}/stream_encoder_intrin_ssse3.c
		${FLAC_SRC}/window.c)
	if(WIN32)
		target_sources(chdm_flac PRIVATE ${TP}/flac/src/share/win_utf8_io/win_utf8_io.c)
	endif()
	target_include_directories(chdm_flac
		PUBLIC $<BUILD_INTERFACE:${TP}/flac/include>
		PRIVATE ${FLAC_SRC}/include)
	target_compile_definitions(chdm_flac
		PUBLIC FLAC__NO_DLL
		PRIVATE
			NDEBUG HAVE_CONFIG_H OGG_FOUND=0 FLAC__HAS_OGG=0 HAVE_LROUND=1
			HAVE_INTTYPES_H HAVE_STDBOOL_H HAVE_STDINT_H HAVE_STDIO_H HAVE_STDLIB_H HAVE_STRING_H
			# Not on Android: before API 24, bionic hides fseeko/ftello when _FILE_OFFSET_BITS=64 on
			# 32-bit ABIs.  MAME only uses FLAC's stream API, never its file I/O, so it doesn't matter.
			$<$<NOT:$<OR:$<C_COMPILER_ID:MSVC>,$<PLATFORM_ID:Android>>>:HAVE_FSEEKO _FILE_OFFSET_BITS=64 _LARGEFILE_SOURCE>
			$<$<PLATFORM_ID:Darwin,iOS>:FLAC__SYS_DARWIN>
			$<$<PLATFORM_ID:Linux,Android>:FLAC__SYS_LINUX>)
	if(CHDMODERN_BIG_ENDIAN)
		target_compile_definitions(chdm_flac PRIVATE CPU_IS_BIG_ENDIAN=1 CPU_IS_LITTLE_ENDIAN=0 WORDS_BIGENDIAN=1)
	else()
		target_compile_definitions(chdm_flac PRIVATE CPU_IS_BIG_ENDIAN=0 CPU_IS_LITTLE_ENDIAN=1 WORDS_BIGENDIAN=0)
	endif()
	if(NOT MSVC)
		target_link_libraries(chdm_flac PRIVATE m)
	endif()
	target_compile_options(chdm_flac PRIVATE ${CHDMODERN_WARN_OFF})
	set(CHDM_FLAC chdm_flac)
endif()

# ---------------------------------------------------------------------------
# The library
# ---------------------------------------------------------------------------

set(MAME_UTIL ${CMAKE_CURRENT_SOURCE_DIR}/src/mame/util)
set(MAME_OSD  ${CMAKE_CURRENT_SOURCE_DIR}/src/mame/osd)

set(CHDMODERN_SOURCES
	# MAME lib/util, unmodified
	${MAME_UTIL}/avhuff.cpp
	${MAME_UTIL}/bitmap.cpp
	${MAME_UTIL}/cdrom.cpp
	${MAME_UTIL}/chd.cpp
	${MAME_UTIL}/chdcodec.cpp
	${MAME_UTIL}/corefile.cpp
	${MAME_UTIL}/corestr.cpp
	${MAME_UTIL}/flac.cpp
	${MAME_UTIL}/hashing.cpp
	${MAME_UTIL}/huffman.cpp
	${MAME_UTIL}/ioprocs.cpp
	${MAME_UTIL}/md5.cpp
	${MAME_UTIL}/palette.cpp
	${MAME_UTIL}/path.cpp
	${MAME_UTIL}/strformat.cpp
	${MAME_UTIL}/unicode.cpp
	${MAME_UTIL}/vecstream.cpp
	# CHDmodern
	src/osd_portable.cpp
	src/chdmodern.cpp)

find_package(Threads)

# Applies the build settings shared by every variant of the library.
function(chdmodern_configure target)
	target_compile_features(${target} PUBLIC cxx_std_20)
	set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
	target_include_directories(${target}
		PUBLIC
			$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
			$<INSTALL_INTERFACE:include>
			# MAME's C++ API (chd.h, cdrom.h, ...) for C++ consumers
			$<BUILD_INTERFACE:${MAME_UTIL}>
			$<BUILD_INTERFACE:${MAME_OSD}>
			$<INSTALL_INTERFACE:include/chdmodern/mame>)
	target_compile_definitions(${target}
		PUBLIC
			$<$<NOT:$<BOOL:${CHDMODERN_BIG_ENDIAN}>>:LSB_FIRST>
		PRIVATE
			ZLIB_CONST
			$<IF:$<BOOL:${WIN32}>,CRLF=3,CRLF=2>
			$<$<BOOL:${CHDMODERN_NO_THREADS}>:CHDMODERN_NO_THREADS>
			$<$<BOOL:${WIN32}>:WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE>)
	if(MSVC)
		# same options and warning set MAME uses for its own MSVC builds
		target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_DEPRECATE _CRT_NONSTDC_NO_DEPRECATE)
		target_compile_options(${target} PRIVATE
			/utf-8 /bigobj /permissive-
			$<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus /Zc:preprocessor>
			/wd4018 /wd4100 /wd4127 /wd4146 /wd4244 /wd4245 /wd4250 /wd4267 /wd4305 /wd4324
			/wd4334 /wd4389 /wd4456 /wd4457 /wd4458 /wd4459 /wd4701 /wd4702 /wd4996)
	endif()
	target_link_libraries(${target}
		PRIVATE ${CHDM_ZLIB} ${CHDM_ZSTD} ${CHDM_LZMA} ${CHDM_FLAC}
		PUBLIC $<$<NOT:$<BOOL:${CHDMODERN_NO_THREADS}>>:Threads::Threads>)
endfunction()

if(CHDMODERN_SHARED)
	# Only the C API (chdm_*) is exported from the shared library; the MAME
	# C++ API is available when linking statically.
	add_library(chdmodern SHARED ${CHDMODERN_SOURCES})
	if(WIN32)
		# version shown in the DLL's Properties > Details
		target_sources(chdmodern PRIVATE src/chdmodern.rc)
	endif()
	target_compile_definitions(chdmodern PUBLIC CHDMODERN_SHARED PRIVATE CHDMODERN_BUILDING)
	# No VERSION/SOVERSION: the file is exactly CHDm.so / CHDm.dylib on every platform (no
	# CHDm.so.0.3.0 + symlinks), so it can be copied and dlopen()ed by that name.  The version is
	# available at runtime from chdm_version().
	set_target_properties(chdmodern PROPERTIES
		CXX_VISIBILITY_PRESET hidden
		VISIBILITY_INLINES_HIDDEN ON)
else()
	add_library(chdmodern STATIC ${CHDMODERN_SOURCES})
endif()
add_library(CHDmodern::chdmodern ALIAS chdmodern)
# CHDm.so / CHDm.dll / CHDm.dylib / CHDm.a / CHDm.lib everywhere, without the platform's "lib" prefix.
set_target_properties(chdmodern PROPERTIES OUTPUT_NAME CHDm PREFIX "" IMPORT_PREFIX "")
chdmodern_configure(chdmodern)

# A static chdmodern needs its bundled dependencies at final link time.
# Combine them so consumers only ever see one library.
if(NOT CHDMODERN_SHARED)
	foreach(dep chdm_zlib chdm_zstd chdm_lzma chdm_flac)
		if(TARGET ${dep})
			list(APPEND CHDMODERN_BUNDLED ${dep})
		endif()
	endforeach()
endif()

# ---------------------------------------------------------------------------
# Tools and tests
# ---------------------------------------------------------------------------

if(CHDMODERN_BUILD_TOOLS)
	add_executable(chdm-info tools/chdm-info.c)
	target_link_libraries(chdm-info PRIVATE chdmodern)
	set_target_properties(chdm-info PROPERTIES LINKER_LANGUAGE CXX)

	add_executable(chdm-extract tools/chdm-extract.c)
	target_link_libraries(chdm-extract PRIVATE chdmodern)
	set_target_properties(chdm-extract PROPERTIES LINKER_LANGUAGE CXX)
	# the CD path writes with stdio; allow output over 2 GiB on 32-bit POSIX
	target_compile_definitions(chdm-extract PRIVATE $<$<NOT:$<C_COMPILER_ID:MSVC>>:_FILE_OFFSET_BITS=64>)
endif()

if(CHDMODERN_BUILD_TESTS)
	enable_testing()
	add_executable(chdm-selftest tests/selftest.cpp)
	if(CHDMODERN_SHARED)
		# the test drives MAME's C++ compressor, which a shared build doesn't export
		add_library(chdmodern_testlib STATIC EXCLUDE_FROM_ALL ${CHDMODERN_SOURCES})
		chdmodern_configure(chdmodern_testlib)
		target_link_libraries(chdm-selftest PRIVATE chdmodern_testlib)
	else()
		target_link_libraries(chdm-selftest PRIVATE chdmodern)
	endif()
	add_test(NAME selftest COMMAND chdm-selftest ${CMAKE_CURRENT_BINARY_DIR})

	# C-API-only consumer; against the shared build this exercises the DLL/.so
	add_executable(chdm-dlltest tests/dlltest.c)
	target_link_libraries(chdm-dlltest PRIVATE chdmodern)
	set_target_properties(chdm-dlltest PROPERTIES LINKER_LANGUAGE CXX)
	add_test(NAME dlltest COMMAND chdm-dlltest ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/sample_dvd_zlib.chd)
endif()

# ---------------------------------------------------------------------------
# Install / package
# ---------------------------------------------------------------------------

if(CHDMODERN_INSTALL)
	include(GNUInstallDirs)
	include(CMakePackageConfigHelpers)

	install(TARGETS chdmodern ${CHDMODERN_BUNDLED}
		EXPORT CHDmodernTargets
		ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
		LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
		RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
	install(DIRECTORY include/chdmodern DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
	install(DIRECTORY ${MAME_UTIL}/ ${MAME_OSD}/
		DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/chdmodern/mame
		FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp" PATTERN "*.ipp")
	install(EXPORT CHDmodernTargets
		NAMESPACE CHDmodern::
		DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CHDmodern)
	file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/CHDmodernConfig.cmake
		"include(CMakeFindDependencyMacro)\nfind_dependency(Threads)\ninclude(\${CMAKE_CURRENT_LIST_DIR}/CHDmodernTargets.cmake)\n")
	write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/CHDmodernConfigVersion.cmake
		COMPATIBILITY SameMinorVersion)
	install(FILES
		${CMAKE_CURRENT_BINARY_DIR}/CHDmodernConfig.cmake
		${CMAKE_CURRENT_BINARY_DIR}/CHDmodernConfigVersion.cmake
		DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CHDmodern)
endif()
