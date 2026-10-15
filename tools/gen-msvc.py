#!/usr/bin/env python3
"""Generates the Visual Studio solution and projects in msvc/.

Run it after tools/sync-from-mame.sh whenever the set of source files changes:

    python tools/gen-msvc.py

The source lists mirror CMakeLists.txt; build settings shared by every
project live in msvc/CHDmodern.Common.props (hand-written, not generated).
Project GUIDs are derived from the project names, so regenerating produces
identical files unless the sources changed.
"""

import os
import uuid
from xml.sax.saxutils import escape

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
MSVC = os.path.join(ROOT, 'msvc')

CONFIGS = ['Debug', 'Release']
PLATFORMS = ['Win32', 'x64', 'ARM64']

NAMESPACE = uuid.UUID('6c1d6a52-4b1e-4d0b-9a57-2f3c3a4a2b10')
VCXPROJ_TYPE = '8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942'
FOLDER_TYPE = '2150E333-8FDC-42A3-9474-1A3956D46DE8'


def guid(name):
	return str(uuid.uuid5(NAMESPACE, name)).upper()


def files(*patterns):
	"""Repository-relative paths (forward slashes) matching the given
	directory/extension pairs or explicit paths, sorted."""
	out = []
	for p in patterns:
		if isinstance(p, tuple):
			d, exts = p
			full = os.path.join(ROOT, d)
			for f in sorted(os.listdir(full)):
				if os.path.splitext(f)[1] in exts:
					out.append(f'{d}/{f}')
		else:
			if not os.path.exists(os.path.join(ROOT, p)):
				raise SystemExit(f'missing source file: {p}')
			out.append(p)
	return out


def rel(path):
	"""Path as seen from msvc/, Windows separators."""
	return '..\\' + path.replace('/', '\\')


FLAC = 'third_party/flac/src/libFLAC/'
UTIL = 'src/mame/util/'

PROJECTS = {
	'chdm_zlib': dict(
		kind='StaticLibrary', lang='c', folder='third_party',
		sources=files(*[f'third_party/zlib/{f}.c' for f in
			'adler32 compress crc32 deflate infback inffast inflate inftrees trees uncompr zutil'.split()]),
		headers=files(('third_party/zlib', {'.h'})),
		defines=[],
		includes=['third_party/zlib'],
	),
	'chdm_zstd': dict(
		kind='StaticLibrary', lang='c', folder='third_party',
		sources=files(('third_party/zstd/lib/common', {'.c'}),
		              ('third_party/zstd/lib/compress', {'.c'}),
		              ('third_party/zstd/lib/decompress', {'.c'})),
		headers=files(('third_party/zstd/lib', {'.h'})),
		defines=['XXH_NAMESPACE=ZSTD_', 'DEBUGLEVEL=0', 'ZSTD_DISABLE_ASM',
		         'ZSTDLIB_VISIBLE=', 'ZSTDERRORLIB_VISIBLE=', 'ZDICTLIB_VISIBLE='],
		includes=['third_party/zstd/lib'],
	),
	'chdm_lzma': dict(
		kind='StaticLibrary', lang='c', folder='third_party',
		sources=files(*[f'third_party/lzma/C/{f}.c' for f in 'CpuArch LzFind LzFindOpt LzmaDec LzmaEnc'.split()]),
		headers=files(('third_party/lzma/C', {'.h'})),
		defines=['Z7_ST'],
		includes=['third_party/lzma/C'],
	),
	'chdm_flac': dict(
		kind='StaticLibrary', lang='c', folder='third_party',
		sources=files(*[FLAC + f + '.c' for f in (
			'bitmath bitreader bitwriter cpu crc fixed fixed_intrin_avx2 fixed_intrin_sse2 '
			'fixed_intrin_sse42 fixed_intrin_ssse3 float format lpc lpc_intrin_avx2 lpc_intrin_fma '
			'lpc_intrin_neon lpc_intrin_sse2 lpc_intrin_sse41 md5 memory stream_decoder stream_encoder '
			'stream_encoder_framing stream_encoder_intrin_avx2 stream_encoder_intrin_sse2 '
			'stream_encoder_intrin_ssse3 window').split()],
			'third_party/flac/src/share/win_utf8_io/win_utf8_io.c'),
		headers=files(('third_party/flac/include/FLAC', {'.h'})),
		defines=['FLAC__NO_DLL', 'NDEBUG', 'HAVE_CONFIG_H', 'OGG_FOUND=0', 'FLAC__HAS_OGG=0', 'HAVE_LROUND=1',
		         'HAVE_INTTYPES_H', 'HAVE_STDBOOL_H', 'HAVE_STDINT_H', 'HAVE_STDIO_H', 'HAVE_STDLIB_H',
		         'HAVE_STRING_H', 'CPU_IS_BIG_ENDIAN=0', 'CPU_IS_LITTLE_ENDIAN=1', 'WORDS_BIGENDIAN=0'],
		includes=['third_party/flac/include', 'third_party/flac/src/libFLAC/include'],
	),
	'chdm_utf8proc': dict(
		kind='StaticLibrary', lang='c', folder='third_party',
		sources=files('third_party/utf8proc/utf8proc.c'),
		headers=files('third_party/utf8proc/utf8proc.h'),
		defines=['UTF8PROC_STATIC'],
		includes=['third_party/utf8proc'],
	),
	'chdmodern': dict(
		kind='StaticLibrary', lang='cpp', folder=None,
		sources=files(*[UTIL + f + '.cpp' for f in (
			'avhuff bitmap cdrom chd chdcodec corefile corestr flac hashing huffman ioprocs md5 '
			'palette path strformat unicode vecstream').split()],
			'src/osd_portable.cpp', 'src/chdmodern.cpp'),
		headers=files('include/chdmodern/chdmodern.h', ('src/mame/util', {'.h', '.hpp', '.ipp'}),
		              ('src/mame/osd', {'.h'})),
		defines=['$(ChdModernDefines)'],
		includes=['$(ChdModernIncludes)'],
		deps=['chdm_zlib', 'chdm_zstd', 'chdm_lzma', 'chdm_flac', 'chdm_utf8proc'],
		merge_deps=True,
	),
	'chdm-info': dict(
		kind='Application', lang='c', folder='tools',
		sources=files('tools/chdm-info.c'), headers=[],
		defines=[], includes=['include'], deps=['chdmodern'],
	),
	'chdm-extract': dict(
		kind='Application', lang='c', folder='tools',
		sources=files('tools/chdm-extract.c'), headers=[],
		defines=[], includes=['include'], deps=['chdmodern'],
	),
	'chdm-selftest': dict(
		kind='Application', lang='cpp', folder='tests',
		sources=files('tests/selftest.cpp'), headers=[],
		defines=['$(ChdModernDefines)'], includes=['$(ChdModernIncludes)'], deps=['chdmodern'],
	),
}

# zlib and zstd can be supplied by a host solution instead (see the props file)
OPTIONAL_DEPS = {
	'chdm_zlib': "'$(ChdModernBundledZlib)' != 'false'",
	'chdm_zstd': "'$(ChdModernBundledZstd)' != 'false'",
}


def include_path(p):
	return p if p.startswith('$(') else '$(MSBuildThisFileDirectory)' + rel(p)


def gen_vcxproj(name, p):
	lines = []
	w = lines.append
	w('<?xml version="1.0" encoding="utf-8"?>')
	w('<!-- Generated by tools/gen-msvc.py - do not edit by hand. -->')
	w('<Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">')
	w('  <ItemGroup Label="ProjectConfigurations">')
	for c in CONFIGS:
		for pl in PLATFORMS:
			w(f'    <ProjectConfiguration Include="{c}|{pl}">')
			w(f'      <Configuration>{c}</Configuration>')
			w(f'      <Platform>{pl}</Platform>')
			w('    </ProjectConfiguration>')
	w('  </ItemGroup>')
	w('  <PropertyGroup Label="Globals">')
	w('    <VCProjectVersion>17.0</VCProjectVersion>')
	w(f'    <ProjectGuid>{{{guid(name)}}}</ProjectGuid>')
	w(f'    <RootNamespace>{name.replace("-", "_")}</RootNamespace>')
	w(f'    <ProjectName>{name}</ProjectName>')
	w('    <WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion>')
	w('  </PropertyGroup>')
	w('  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.Default.props" />')
	w('  <PropertyGroup Label="Configuration">')
	w(f'    <ConfigurationType>{p["kind"]}</ConfigurationType>')
	w('    <PlatformToolset>$(DefaultPlatformToolset)</PlatformToolset>')
	w('    <CharacterSet>Unicode</CharacterSet>')
	w('  </PropertyGroup>')
	w('  <PropertyGroup Condition="\'$(Configuration)\'==\'Debug\'" Label="Configuration">')
	w('    <UseDebugLibraries>true</UseDebugLibraries>')
	w('  </PropertyGroup>')
	w('  <PropertyGroup Condition="\'$(Configuration)\'==\'Release\'" Label="Configuration">')
	w('    <UseDebugLibraries>false</UseDebugLibraries>')
	w('    <WholeProgramOptimization>false</WholeProgramOptimization>')
	w('  </PropertyGroup>')
	w('  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.props" />')
	w('  <ImportGroup Label="PropertySheets">')
	w('    <Import Project="$(UserRootDir)\\Microsoft.Cpp.$(Platform).user.props" Condition="exists(\'$(UserRootDir)\\Microsoft.Cpp.$(Platform).user.props\')" Label="LocalAppDataPlatform" />')
	w('    <Import Project="CHDmodern.Common.props" />')
	w('  </ImportGroup>')
	w('  <ItemDefinitionGroup>')
	w('    <ClCompile>')
	defines = ';'.join(p['defines'] + ['%(PreprocessorDefinitions)'])
	w(f'      <PreprocessorDefinitions>{escape(defines)}</PreprocessorDefinitions>')
	incs = ';'.join([include_path(i) for i in p['includes']] + ['%(AdditionalIncludeDirectories)'])
	w(f'      <AdditionalIncludeDirectories>{escape(incs)}</AdditionalIncludeDirectories>')
	if p['lang'] == 'cpp':
		w('      <LanguageStandard>stdcpp20</LanguageStandard>')
		w('      <AdditionalOptions>$(ChdModernCppOptions) %(AdditionalOptions)</AdditionalOptions>')
	else:
		w('      <AdditionalOptions>$(ChdModernCOptions) %(AdditionalOptions)</AdditionalOptions>')
	if p.get('folder') == 'third_party':
		w('      <WarningLevel>TurnOffAllWarnings</WarningLevel>')
	w('    </ClCompile>')
	if p.get('merge_deps'):
		# "Librarian > Link Library Dependencies": fold the bundled
		# dependencies into chdmodern.lib, so hosts link one library
		w('    <ProjectReference>')
		w('      <LinkLibraryDependencies>true</LinkLibraryDependencies>')
		w('    </ProjectReference>')
	if p['kind'] == 'Application':
		w('    <Link>')
		w('      <SubSystem>Console</SubSystem>')
		w('    </Link>')
	w('  </ItemDefinitionGroup>')
	w('  <ItemGroup>')
	for s in p['sources']:
		w(f'    <ClCompile Include="{rel(s)}" />')
	w('  </ItemGroup>')
	if p['headers']:
		w('  <ItemGroup>')
		for h in p['headers']:
			w(f'    <ClInclude Include="{rel(h)}" />')
		w('  </ItemGroup>')
	if p.get('deps'):
		w('  <ItemGroup>')
		for d in p['deps']:
			cond = OPTIONAL_DEPS.get(d)
			cond_attr = f' Condition="{escape(cond)}"' if cond else ''
			w(f'    <ProjectReference Include="{d}.vcxproj"{cond_attr}>')
			w(f'      <Project>{{{guid(d)}}}</Project>')
			w('    </ProjectReference>')
		w('  </ItemGroup>')
	w('  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.targets" />')
	w('</Project>')
	return '\r\n'.join(lines) + '\r\n'


def gen_filters(name, p):
	def folder_of(path):
		d = os.path.dirname(path)
		for prefix in ('src/mame/', 'third_party/', 'src/', ''):
			if d.startswith(prefix):
				return d[len(prefix):].replace('/', '\\') or 'src'
		return d

	folders = set()
	items = []
	for kind, paths in (('ClCompile', p['sources']), ('ClInclude', p['headers'])):
		for s in paths:
			f = folder_of(s)
			parts = f.split('\\')
			for i in range(1, len(parts) + 1):
				folders.add('\\'.join(parts[:i]))
			items.append((kind, s, f))

	lines = []
	w = lines.append
	w('<?xml version="1.0" encoding="utf-8"?>')
	w('<Project ToolsVersion="4.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">')
	w('  <ItemGroup>')
	for f in sorted(folders):
		w(f'    <Filter Include="{f}">')
		w(f'      <UniqueIdentifier>{{{guid(name + "/" + f)}}}</UniqueIdentifier>')
		w('    </Filter>')
	w('  </ItemGroup>')
	w('  <ItemGroup>')
	for kind, s, f in items:
		w(f'    <{kind} Include="{rel(s)}">')
		w(f'      <Filter>{f}</Filter>')
		w(f'    </{kind}>')
	w('  </ItemGroup>')
	w('</Project>')
	return '\r\n'.join(lines) + '\r\n'


def gen_sln():
	lines = []
	w = lines.append
	w('')
	w('Microsoft Visual Studio Solution File, Format Version 12.00')
	w('# Visual Studio Version 17')
	w('VisualStudioVersion = 17.0.31903.59')
	w('MinimumVisualStudioVersion = 10.0.40219.1')
	for name in PROJECTS:
		w(f'Project("{{{VCXPROJ_TYPE}}}") = "{name}", "{name}.vcxproj", "{{{guid(name)}}}"')
		w('EndProject')
	folders = sorted({p['folder'] for p in PROJECTS.values() if p['folder']})
	for f in folders:
		w(f'Project("{{{FOLDER_TYPE}}}") = "{f}", "{f}", "{{{guid("folder:" + f)}}}"')
		w('EndProject')
	w('Global')
	w('\tGlobalSection(SolutionConfigurationPlatforms) = preSolution')
	for c in CONFIGS:
		for pl in PLATFORMS:
			sp = 'x86' if pl == 'Win32' else pl
			w(f'\t\t{c}|{sp} = {c}|{sp}')
	w('\tEndGlobalSection')
	w('\tGlobalSection(ProjectConfigurationPlatforms) = postSolution')
	for name in PROJECTS:
		for c in CONFIGS:
			for pl in PLATFORMS:
				sp = 'x86' if pl == 'Win32' else pl
				w(f'\t\t{{{guid(name)}}}.{c}|{sp}.ActiveCfg = {c}|{pl}')
				w(f'\t\t{{{guid(name)}}}.{c}|{sp}.Build.0 = {c}|{pl}')
	w('\tEndGlobalSection')
	w('\tGlobalSection(SolutionProperties) = preSolution')
	w('\t\tHideSolutionNode = FALSE')
	w('\tEndGlobalSection')
	w('\tGlobalSection(NestedProjects) = preSolution')
	for name, p in PROJECTS.items():
		if p['folder']:
			w(f'\t\t{{{guid(name)}}} = {{{guid("folder:" + p["folder"])}}}')
	w('\tEndGlobalSection')
	w('\tGlobalSection(ExtensibilityGlobals) = postSolution')
	w(f'\t\tSolutionGuid = {{{guid("solution")}}}')
	w('\tEndGlobalSection')
	w('EndGlobal')
	return '﻿' + '\r\n'.join(lines) + '\r\n'


def write(path, text):
	with open(path, 'w', encoding='utf-8', newline='') as f:
		f.write(text)


def main():
	os.makedirs(MSVC, exist_ok=True)
	for name, p in PROJECTS.items():
		write(os.path.join(MSVC, f'{name}.vcxproj'), gen_vcxproj(name, p))
		write(os.path.join(MSVC, f'{name}.vcxproj.filters'), gen_filters(name, p))
	write(os.path.join(MSVC, 'CHDmodern.sln'), gen_sln())
	print(f'wrote {len(PROJECTS)} projects and CHDmodern.sln to {MSVC}')


if __name__ == '__main__':
	main()
