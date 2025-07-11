cmake_minimum_required(VERSION 3.2...3.10)

project(WavPack VERSION 5.8.1)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/configure.ac" CONFIGURE_AC)
string(REGEX MATCH "LT_CURRENT=([0-9]+)" LT_CURRENT "${CONFIGURE_AC}")
set(LT_CURRENT "${CMAKE_MATCH_1}")
string(REGEX MATCH "LT_REVISION=([0-9]+)" LT_REVISION "${CONFIGURE_AC}")
set(LT_REVISION "${CMAKE_MATCH_1}")
string(REGEX MATCH "LT_AGE=([0-9]+)" LT_AGE "${CONFIGURE_AC}")
set(LT_AGE "${CMAKE_MATCH_1}")

math(EXPR SOVERSION_MAJOR "${LT_CURRENT}-${LT_AGE}")
math(EXPR SOVERSION_MINOR "${LT_AGE}")
math(EXPR SOVERSION_MICRO "${LT_REVISION}")

math(EXPR DYLIB_CURRENT_VERSION_MAJOR "${LT_CURRENT} + 1")
math(EXPR DYLIB_CURRENT_VERSION_MINOR "${LT_REVISION}")
math(EXPR DYLIB_CURRENT_VERSION_MICRO "0")
math(EXPR DYLIB_COMPATIBILITY_VERSION_MAJOR "${LT_CURRENT} + 1")
math(EXPR DYLIB_COMPATIBILITY_VERSION_MINOR "0")
math(EXPR DYLIB_COMPATIBILITY_VERSION_MICRO "0")
set(DYLIB_CURRENT_VERSION "${DYLIB_CURRENT_VERSION_MAJOR}.${DYLIB_CURRENT_VERSION_MINOR}.${DYLIB_CURRENT_VERSION_MICRO}")
set(DYLIB_COMPATIBILITY_VERSION "${DYLIB_COMPATIBILITY_VERSION_MAJOR}.${DYLIB_COMPATIBILITY_VERSION_MINOR}.${DYLIB_COMPATIBILITY_VERSION_MICRO}")

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(WAVPACK_ROOTPROJECT ON)
else()
    set(WAVPACK_ROOTPROJECT OFF)
endif()

# Languages

include(CheckLanguage)

check_language(ASM)
if(CMAKE_ASM_COMPILER)
  enable_language(ASM)
endif()

if(MSVC)
  if(WavPack_CPU_X86 OR WavPack_CPU_X64)
    enable_language(ASM_MASM)
  elseif(WavPack_CPU_ARM32 OR WavPack_CPU_ARM64)
    enable_language(ASM_MARMASM)
  endif()
else()
  check_language(ASM-ATT)
  if(CMAKE_ASM-ATT_COMPILER)
    enable_language(ASM-ATT)
  endif()
endif()


# Policies

# CMake 3.11 warning: Include file check macros honor CMAKE_REQUIRED_LIBRARIES.
if(POLICY CMP0075)
  cmake_policy(SET CMP0075 NEW)
endif()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

if(CMAKE_VERSION VERSION_LESS 3.12)
	set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
	set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
	set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
	set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
endif()

include(CMakePushCheckState)
include(CheckCCompilerFlag)
include(CheckCLinkerFlag)
include(TestLargeFiles)
include(CheckLanguage)
include(CheckLibraryExists)
include(CheckFunctionExists)
include(CMakeDependentOption)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(FeatureSummary)
include(CPack)

# Options. See also dependent options below

option(WAVPACK_BUILD_PROGRAMS "Build programs" ${WAVPACK_ROOTPROJECT})
option(WAVPACK_ENABLE_LIBICONV "Prefer iconv() from libiconv, if available, over libc version" OFF)
option(WAVPACK_ENABLE_LEGACY "Decode legacy (< 4.0) WavPack files" OFF)
option(WAVPACK_ENABLE_THREADS "Enable support for threading in libwavpack" ON)
option(WAVPACK_ENABLE_DSD "Enable support for WavPack DSD files" ON)
option(WAVPACK_INSTALL_CMAKE_MODULE "Generate and install CMake package configuration module" ${WAVPACK_ROOTPROJECT})
option(WAVPACK_INSTALL_DOCS "Install documentation" ${WAVPACK_ROOTPROJECT})
option(WAVPACK_INSTALL_PKGCONFIG_MODULE "Generate and install wavpack.pc" ${WAVPACK_ROOTPROJECT})
option(BUILD_SHARED_LIBS "Build the library as a shared library" OFF)
option(BUILD_TESTING "Build tests" OFF)

# Configuration tests

check_library_exists(m cos "" HAVE_LIBM)
if(HAVE_LIBM)
    list(APPEND CMAKE_REQUIRED_LIBRARIES m)
endif()

if(NOT WIN32)
  find_package(Threads)
endif()

check_c_source_compiles(
        "int main()
        {
            return __builtin_clz(1);
        }"
        HAVE___BUILTIN_CLZ
)

test_large_files(LARGE_FILES_SUPPORTED)
if(LARGE_FILES_SUPPORTED)
    add_definitions(${LARGE_FILES_DEFINITIONS})
endif()
check_function_exists(fseeko HAVE_FSEEKO)

if(NOT MSVC AND (CMAKE_COMPILER_IS_GNUCC OR CMAKE_C_COMPILER_ID MATCHES "Clang"))
    add_compile_options(-Wall)
endif()

include(cmake/wavpackcpu.cmake)
WavPack_DetectTargetCPUArchitectures(WAVPACK_CPUS)

if(WIN32 AND NOT MINGW)
    if(CMAKE_ASM_MASM_COMPILER)
        set(HAVE_MASM 1)
    endif()
else()
    if(CMAKE_ASM-ATT_COMPILER)
        if(WavPack_CPU_X86 OR WavPack_CPU_X64 OR WavPack_CPU_ARM32)
            set(HAVE_ASM 1)
        endif()
    endif()
endif()

# Dependent options

cmake_dependent_option(WAVPACK_ENABLE_ASM "Enable assembly optimizations" ON "HAVE_ASM OR HAVE_MASM" OFF)
cmake_dependent_option(WAVPACK_BUILD_COOLEDIT_PLUGIN "Build CoolEdit plugin" OFF "WIN32" OFF)
cmake_dependent_option(WAVPACK_BUILD_WINAMP_PLUGIN "Build WinAmp plugin" OFF "WIN32" OFF)

# Targets

add_library(wavpack
    src/common_utils.c
    src/decorr_utils.c
    src/entropy_utils.c
    src/extra1.c
    src/extra2.c
    src/open_utils.c
    src/open_filename.c
    src/open_legacy.c
    src/open_raw.c
    src/pack.c
    src/pack_dns.c
    src/pack_floats.c
    src/pack_utils.c
    src/read_words.c
    src/tags.c
    src/tag_utils.c
    src/unpack.c
    src/unpack_floats.c
    src/unpack_seek.c
    src/unpack_utils.c
    src/write_words.c
    src/decorr_tables.h
    src/unpack3.h
    src/wavpack_local.h
    src/wavpack_version.h
    $<$<BOOL:${WAVPACK_ENABLE_LEGACY}>:src/unpack3.c>
    $<$<BOOL:${WAVPACK_ENABLE_LEGACY}>:src/unpack3_open.c>
    $<$<BOOL:${WAVPACK_ENABLE_LEGACY}>:src/unpack3_seek.c>
    $<$<BOOL:${WAVPACK_ENABLE_DSD}>:src/pack_dsd.c>
    $<$<BOOL:${WAVPACK_ENABLE_DSD}>:src/unpack_dsd.c>
    $<$<AND:$<BOOL:${WAVPACK_ENABLE_ASM}>,$<BOOL:${HAVE_MASM}>,$<BOOL:${WavPack_CPU_X86}>>:src/pack_x86.asm>
    $<$<AND:$<BOOL:${WAVPACK_ENABLE_ASM}>,$<BOOL:${HAVE_MASM}>,$<BOOL:${WavPack_CPU_X86}>>:src/unpack_x86.asm>
    $<$<AND:$<BOOL:${WAVPACK_ENABLE_ASM}>,$<BOOL:${HAVE_MASM}>,$<BOOL:${WavPack_CPU_X64}>>:src/pack_x64.asm>
    $<$<AND:$<BOOL:${WAVPACK_ENABLE_ASM}>,$<BOOL:${HAVE_MASM}>,$<BOOL:${WavPack_CPU_X64}>>:src/unpack_x64.asm>
    $<$<AND:$<BOOL:${WAVPACK_ENABLE_ASM}>,$<BOOL:${HAVE_ASM}>,$<BOOL:${WavPack_CPU_X86}>>:src/pack_x86.S>
    $<$<AND:$<BOOL:${WAVPACK_ENABLE_ASM}>,$<BOOL:${HAVE_ASM}>,$<BOOL:${WavPack_CPU_X86}>>:src/unpack_x86.S>
    $<$<AND:$<BOOL:${WAVPACK_ENABLE_ASM}>,$<BOOL:${HAVE_ASM}>,$<BOOL:${WavPack_CPU_X64}>>:src/pack_x64.S>
    $<$<AND:$<BOOL:${WAVPACK_ENABLE_ASM}>,$<BOOL:${HAVE_ASM}>,$<BOOL:${WavPack_CPU_X64}>>:src/unpack_x64.S>
    $<$<AND:$<BOOL:${WAVPACK_ENABLE_ASM}>,$<BOOL:${HAVE_ASM}>,$<BOOL:${WavPack_CPU_ARM32}>>:src/unpack_armv7.S>
    $<$<AND:$<BOOL:${WIN32}>,$<BOOL:${BUILD_SHARED_LIBS}>>:wavpackdll/wavpackdll.rc>
)
target_include_directories(wavpack
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/wavpack>
)
target_link_libraries(wavpack
    PRIVATE
        $<$<AND:$<NOT:$<BOOL:${WIN32}>>,$<BOOL:${WAVPACK_ENABLE_THREADS}>>:Threads::Threads>
        $<$<BOOL:${HAVE_LIBM}>:m>
)
add_library(WavPack::WavPack ALIAS wavpack)

target_compile_definitions(wavpack
    PRIVATE
        $<$<BOOL:${WAVPACK_ENABLE_LEGACY}>:ENABLE_LEGACY>
        $<$<BOOL:${WAVPACK_ENABLE_THREADS}>:ENABLE_THREADS>
        $<$<BOOL:${WAVPACK_ENABLE_DSD}>:ENABLE_DSD>
        $<$<BOOL:${MSVC}>:_CRT_SECURE_NO_WARNINGS>
        $<$<BOOL:${HAVE___BUILTIN_CLZ}>:HAVE___BUILTIN_CLZ>
        $<$<BOOL:${HAVE_FSEEKO}>:HAVE_FSEEKO>
        $<$<AND:$<BOOL:${WAVPACK_ENABLE_ASM}>,$<BOOL:${WavPack_CPU_X86}>>:OPT_ASM_X86>
        $<$<AND:$<BOOL:${WAVPACK_ENABLE_ASM}>,$<BOOL:${WavPack_CPU_X64}>>:OPT_ASM_X64>
        $<$<AND:$<BOOL:${WAVPACK_ENABLE_ASM}>,$<BOOL:${WavPack_CPU_ARM32}>>:OPT_ASM_ARM32>
)

if(WAVPACK_ENABLE_ASM AND HAVE_MASM AND WavPack_CPU_X86)
	set_source_files_properties(src/pack_x86.asm src/unpack_x86.asm PROPERTIES COMPILE_FLAGS "/safeseh")
endif()

if(WIN32)
    if(MINGW)
        if(BUILD_SHARED_LIBS)
            set_target_properties(wavpack PROPERTIES OUTPUT_NAME "wavpack-${SOVERSION_MAJOR}")
            set_target_properties(wavpack PROPERTIES ARCHIVE_OUTPUT_NAME "wavpack")
        endif()
    else()
        if(BUILD_SHARED_LIBS)
            set_target_properties(wavpack PROPERTIES OUTPUT_NAME wavpackdll)
        else()
            set_target_properties(wavpack PROPERTIES OUTPUT_NAME libwavpack)
        endif()
    endif()
endif()

# Symbols files

set(WAVPACK_EXPORT_SYMBOLS
    WavpackAddWrapper
    WavpackAppendBinaryTagItem
    WavpackAppendTagItem
    WavpackBigEndianToNative
    WavpackCloseFile
    WavpackDeleteTagItem
    WavpackFloatNormalize
    WavpackFlushSamples
    WavpackFreeWrapper
    WavpackGetAverageBitrate
    WavpackGetBinaryTagItem
    WavpackGetBinaryTagItemIndexed
    WavpackGetBitsPerSample
    WavpackGetBytesPerSample
    WavpackGetChannelIdentities
    WavpackGetChannelLayout
    WavpackGetChannelMask
    WavpackGetEncodedNoise
    WavpackGetErrorMessage
    WavpackGetFileExtension
    WavpackGetFileFormat
    WavpackGetFileSize
    WavpackGetFileSize64
    WavpackGetFloatNormExp
    WavpackGetInstantBitrate
    WavpackGetLibraryVersion
    WavpackGetLibraryVersionString
    WavpackGetMD5Sum
    WavpackGetMode
    WavpackGetNativeSampleRate
    WavpackGetNumBinaryTagItems
    WavpackGetNumChannels
    WavpackGetNumErrors
    WavpackGetNumSamples
    WavpackGetNumSamples64
    WavpackGetNumSamplesInFrame
    WavpackGetNumTagItems
    WavpackGetProgress
    WavpackGetQualifyMode
    WavpackGetRatio
    WavpackGetReducedChannels
    WavpackGetSampleIndex
    WavpackGetSampleIndex64
    WavpackGetSampleRate
    WavpackGetTagItem
    WavpackGetTagItemIndexed
    WavpackGetVersion
    WavpackGetWrapperBytes
    WavpackGetWrapperData
    WavpackGetWrapperLocation
    WavpackLittleEndianToNative
    WavpackLossyBlocks
    WavpackNativeToBigEndian
    WavpackNativeToLittleEndian
    WavpackOpenFileInput
    WavpackOpenFileInputEx
    WavpackOpenFileInputEx64
    WavpackOpenFileOutput
    WavpackOpenRawDecoder
    WavpackPackInit
    WavpackPackSamples
    WavpackSeekSample
    WavpackSeekSample64
    WavpackSeekTrailingWrapper
    WavpackSetChannelLayout
    WavpackSetConfiguration
    WavpackSetConfiguration64
    WavpackSetFileInformation
    WavpackStoreMD5Sum
    WavpackUnpackSamples
    WavpackUpdateNumSamples
    WavpackVerifySingleBlock
    WavpackWriteTag
)

if(BUILD_SHARED_LIBS)
  if(WIN32 OR CYGWIN)
    set(FILE_CONTENTS "EXPORTS\n")
    foreach(EXPORT_SYMBOL ${WAVPACK_EXPORT_SYMBOLS})
        list(APPEND FILE_CONTENTS "    ${EXPORT_SYMBOL}\n")
    endforeach()

    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/libwavpack.def ${FILE_CONTENTS})
    target_sources(wavpack PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/libwavpack.def)
    if(MINGW AND NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        target_link_libraries(wavpack PRIVATE -static-libgcc)
    endif()
  elseif(CMAKE_SYSTEM_NAME MATCHES "Darwin")
    set(FILE_CONTENTS "")
    foreach(EXPORT_SYMBOL ${WAVPACK_EXPORT_SYMBOLS})
        list(APPEND FILE_CONTENTS "_${EXPORT_SYMBOL}\n")
    endforeach()
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/libwavpack.sym ${FILE_CONTENTS})
    if(CMAKE_VERSION VERSION_LESS 3.13)
        set_target_properties(wavpack PROPERTIES LINK_FLAGS "-Wl,-exported_symbols_list,'${CMAKE_CURRENT_BINARY_DIR}/libwavpack.sym'")
    else()
        target_link_directories(wavpack PRIVATE "-Wl,-exported_symbols_list,${CMAKE_CURRENT_BINARY_DIR}/libwavpack.sym")
    endif()
    if(BUILD_SHARED_LIBS)
        set_target_properties(wavpack PROPERTIES SOVERSION "${SOVERSION_MAJOR}")
        target_link_libraries(wavpack PRIVATE "-Wl,-compatibility_version,${DYLIB_COMPATIBILITY_VERSION}")
        target_link_libraries(wavpack PRIVATE "-Wl,-current_version,${DYLIB_CURRENT_VERSION}")
    endif()
  else()
    set_target_properties(wavpack PROPERTIES SOVERSION "${SOVERSION_MAJOR}")
    set_target_properties(wavpack PROPERTIES VERSION "${SOVERSION_MAJOR}.${SOVERSION_MINOR}.${SOVERSION_MICRO}")
    set(CONFTTEST_CONTENTS "VERS_1 {\n    global: sym;\n};\n\nVERS_2 {\n    global: sym;\n} VERS_1;\n")
    file(WRITE ${PROJECT_BINARY_DIR}/${CMAKE_FILES_DIRECTORY}/conftest.map "${CONFTTEST_CONTENTS}")
    check_c_linker_flag("-Wl,--version-script=${PROJECT_BINARY_DIR}/${CMAKE_FILES_DIRECTORY}/conftest.map" COMPILER_SUPPORTS_SYMBOL_MAPS)

    if(COMPILER_SUPPORTS_SYMBOL_MAPS)
        set(FILE_CONTENTS "{\n global:\n")
        foreach(EXPORT_SYMBOL ${WAVPACK_EXPORT_SYMBOLS})
            set(FILE_CONTENTS "${FILE_CONTENTS}  ${EXPORT_SYMBOL};\n")
        endforeach()
        set(FILE_CONTENTS "${FILE_CONTENTS} local: *;\n};\n")
        file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/libwavpack.map "${FILE_CONTENTS}")
        if(CMAKE_VERSION VERSION_LESS 3.13)
            set_target_properties(wavpack PROPERTIES LINK_FLAGS "-Wl,--version-script='${CMAKE_CURRENT_BINARY_DIR}/libwavpack.map' -Wl,-no-undefined")
        else()
            target_link_options(wavpack PRIVATE "-Wl,--version-script=${CMAKE_CURRENT_BINARY_DIR}/libwavpack.map;-Wl,-no-undefined")
        endif()
    endif()
  endif()
endif()

set_target_properties(wavpack PROPERTIES EXPORT_NAME WavPack)
set_target_properties(wavpack PROPERTIES PUBLIC_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/include/wavpack.h")

if(WAVPACK_BUILD_PROGRAMS)
	if(NOT WIN32)
	  check_c_source_compiles("
	    #define LIBICONV_PLUG 1 /* in case libiconv header is in include path */
	    #include <stddef.h>
	    #include <iconv.h>
	    int main(int argc, char **argv) {
	        return !iconv_open(NULL,NULL);
	    }" ICONV_IN_LIBC)

	  cmake_push_check_state()
	  list(APPEND CMAKE_REQUIRED_LIBRARIES iconv)
	  check_c_source_compiles("
	    #include <stddef.h>
	    #include <iconv.h>
	    int main(int argc, char **argv) {
	        return !iconv_open(NULL,NULL);
	    }" ICONV_IN_LIBICONV)
	  cmake_pop_check_state()

	  if(ICONV_IN_LIBC OR ICONV_IN_LIBICONV)
	    if(ICONV_IN_LIBICONV AND (WAVPACK_ENABLE_LIBICONV OR (NOT ICONV_IN_LIBC)))
	      set(USE_LIBICONV TRUE)
	    endif()
	  else()
	    message(FATAL_ERROR "iconv is required for apps, use -DWAVPACK_BUILD_PROGRAMS=OFF to build only libwavpack")
	  endif()
	endif()

	add_executable(wavpackapp
		cli/wavpack.c
		cli/riff.c
		cli/wave64.c
		cli/aiff.c
		cli/caff.c
		cli/dsdiff.c
		cli/dsf.c
		cli/utils.c
		cli/md5.c
		cli/import_id3.c
		$<$<BOOL:${WIN32}>:cli/win32_unicode_support.h>
		$<$<BOOL:${WIN32}>:cli/win32_unicode_support.c>
	)
	target_compile_definitions(wavpackapp
		PRIVATE
			$<$<BOOL:${USE_LIBICONV}>:ENABLE_LIBICONV>
			$<$<BOOL:${MSVC}>:_CRT_SECURE_NO_WARNINGS>
			$<$<BOOL:${WAVPACK_ENABLE_THREADS}>:ENABLE_THREADS>
			"PACKAGE_VERSION=\"${PROJECT_VERSION}\""
			"VERSION_OS=\"${CMAKE_SYSTEM_NAME}\""
	)
	set_target_properties(wavpackapp PROPERTIES OUTPUT_NAME wavpack)
	target_link_libraries(wavpackapp
		PRIVATE
			wavpack
			$<$<BOOL:${USE_LIBICONV}>:iconv>
			$<$<BOOL:${HAVE_LIBM}>:m>
	)

	add_executable(wvunpack
		cli/wvunpack.c
		cli/riff_write.c
		cli/wave64_write.c
		cli/aiff_write.c
		cli/caff_write.c
		cli/dsdiff_write.c
		cli/dsf_write.c
		cli/utils.c
		cli/md5.c
		$<$<BOOL:${WIN32}>:cli/win32_unicode_support.h>
		$<$<BOOL:${WIN32}>:cli/win32_unicode_support.c>
	)
	target_compile_definitions(wvunpack
		PRIVATE
			$<$<BOOL:${USE_LIBICONV}>:ENABLE_LIBICONV>
			$<$<BOOL:${MSVC}>:_CRT_SECURE_NO_WARNINGS>
			$<$<BOOL:${WAVPACK_ENABLE_THREADS}>:ENABLE_THREADS>
			"PACKAGE_VERSION=\"${PROJECT_VERSION}\""
			"VERSION_OS=\"${CMAKE_SYSTEM_NAME}\""
	)
	set_target_properties(wvunpack PROPERTIES OUTPUT_NAME wvunpack)
	target_link_libraries(wvunpack
		PRIVATE
			wavpack
			$<$<BOOL:${USE_LIBICONV}>:iconv>
			$<$<BOOL:${HAVE_LIBM}>:m>
	)

	add_executable(wvgain
		cli/wvgain.c
		cli/utils.h
		cli/utils.c
		$<$<BOOL:${WIN32}>:cli/win32_unicode_support.h>
		$<$<BOOL:${WIN32}>:cli/win32_unicode_support.c>
	)
	target_compile_definitions(wvgain
		PRIVATE
			$<$<BOOL:${MSVC}>:_CRT_SECURE_NO_WARNINGS>
			$<$<BOOL:${WAVPACK_ENABLE_THREADS}>:ENABLE_THREADS>
			"PACKAGE_VERSION=\"${PROJECT_VERSION}\""
			"VERSION_OS=\"${CMAKE_SYSTEM_NAME}\""
	)
	set_target_properties(wvgain PROPERTIES OUTPUT_NAME wvgain)
	target_link_libraries(wvgain
		PRIVATE
			wavpack
			$<$<BOOL:${HAVE_LIBM}>:m>
	)

	add_executable(wvtag
		cli/wvtag.c
		cli/utils.c
		cli/import_id3.c
		$<$<BOOL:${WIN32}>:cli/win32_unicode_support.h>
		$<$<BOOL:${WIN32}>:cli/win32_unicode_support.c>
	)
	target_compile_definitions(wvtag
		PRIVATE
			$<$<BOOL:${USE_LIBICONV}>:ENABLE_LIBICONV>
			$<$<BOOL:${MSVC}>:_CRT_SECURE_NO_WARNINGS>
			"PACKAGE_VERSION=\"${PROJECT_VERSION}\""
			"VERSION_OS=\"${CMAKE_SYSTEM_NAME}\""
	)
	target_link_libraries(wvtag
		PRIVATE
			wavpack
			$<$<BOOL:${USE_LIBICONV}>:iconv>
			$<$<BOOL:${HAVE_LIBM}>:m>
	)
endif()

if(WAVPACK_BUILD_COOLEDIT_PLUGIN)

    set(WAVPACK_COOLEDIT_PLUGIN_EXPORT_SYMBOLS
        QueryCoolFilter
        FilterUnderstandsFormat
        GetSuggestedSampleType
        OpenFilterInput
        FilterGetFileSize
        ReadFilterInput
        CloseFilterInput
        FilterOptions
        FilterOptionsString
        OpenFilterOutput
        CloseFilterOutput
        WriteFilterOutput
        FilterGetOptions
        FilterWriteSpecialData
        FilterGetFirstSpecialData
        FilterGetNextSpecialData
    )

    set(FILE_CONTENTS "EXPORTS\n")
    foreach(EXPORT_SYMBOL ${WAVPACK_COOLEDIT_PLUGIN_EXPORT_SYMBOLS})
        list(APPEND FILE_CONTENTS "    ${EXPORT_SYMBOL}\n")
    endforeach()

    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/audition/cool_wv4.def ${FILE_CONTENTS})

    add_library(cool_wv4 MODULE
        audition/cool_wv4.c
        audition/filters.h
        audition/resource.h
        audition/wavpack.rc
        ${CMAKE_CURRENT_BINARY_DIR}/audition/cool_wv4.def
    )
    target_include_directories(cool_wv4 PRIVATE include)
    target_compile_definitions(cool_wv4 PRIVATE $<$<BOOL:${MSVC}>:_CRT_SECURE_NO_WARNINGS>)
    set_target_properties(cool_wv4 PROPERTIES
        DEFINE_SYMBOL AUDITION_EXPORTS
        SUFFIX ".flt"
        PREFIX ""
    )
    target_link_libraries(cool_wv4 PRIVATE wavpack)
    if(MINGW)
        target_link_libraries(cool_wv4 PRIVATE -mwindows -static-libgcc "-Wl,--enable-stdcall-fixup")
    endif()

endif()

if(WAVPACK_BUILD_WINAMP_PLUGIN)
    add_library(in_wv MODULE
        winamp/in_wv.c
        winamp/in2.h
        winamp/out.h
        winamp/resource.h
        winamp/wavpack.rc
        winamp/wasabi/wasabi.cpp
        winamp/wasabi/wasabi.h
    )
    target_include_directories(in_wv PRIVATE include)
    target_compile_definitions(in_wv PRIVATE $<$<BOOL:${MSVC}>:_CRT_SECURE_NO_DEPRECATE>)
    set_target_properties(in_wv PROPERTIES
        DEFINE_SYMBOL WINAMP_EXPORTS
        PREFIX ""
    )
    target_link_libraries(in_wv PRIVATE wavpack)
    if(MINGW)
        target_link_libraries(in_wv PRIVATE -mwindows -static-libgcc -static-libstdc++)
    endif()

    add_library(in_wv_lng MODULE
        winamp/wavpack.rc
    )
    target_link_libraries(in_wv_lng PRIVATE $<$<BOOL:${MSVC}>:-NOENTRY>)
    set_target_properties(in_wv_lng PROPERTIES
        OUTPUT_NAME in_wv
        PREFIX ""
        SUFFIX ".lng"
        LINKER_LANGUAGE "C"
    )
    if(MINGW)
        target_link_libraries(in_wv_lng PRIVATE -mwindows)
    endif()

endif()

set(WAVPACK_DOC_NAMES
    wavpack
    wvgain
    wvunpack
    wvtag
)


# Features

set_package_properties(Threads PROPERTIES
	TYPE OPTIONAL
	DESCRIPTION "Threads library"
    PURPOSE "Required to build tests, optional for library."
)

add_feature_info(BUILD_SHARED_LIBS BUILD_SHARED_LIBS "Build shared library.")
add_feature_info(BUILD_TESTING BUILD_TESTING "Build tests.")
add_feature_info(WAVPACK_ENABLE_LEGACY WAVPACK_ENABLE_LEGACY "Decode legacy (< 4.0) WavPack files.")
add_feature_info(WAVPACK_ENABLE_DSD WAVPACK_ENABLE_DSD "Enable support for WavPack DSD files.")
add_feature_info(WAVPACK_ENABLE_THREADS WAVPACK_ENABLE_THREADS "Enable support for threading in libwavpack.")
add_feature_info(WAVPACK_INSTALL_CMAKE_MODULE WAVPACK_INSTALL_CMAKE_MODULE "Generate and install CMake package configuration module.")

add_feature_info(WAVPACK_ENABLE_ASM WAVPACK_ENABLE_ASM "Enable assembly optimizations.")
add_feature_info(WAVPACK_BUILD_PROGRAMS WAVPACK_BUILD_PROGRAMS "Build programs.")
add_feature_info(WAVPACK_BUILD_COOLEDIT_PLUGIN WAVPACK_BUILD_COOLEDIT_PLUGIN "Build CoolEdit plugin.")
add_feature_info(WAVPACK_BUILD_WINAMP_PLUGIN WAVPACK_BUILD_WINAMP_PLUGIN "Build WinAmp plugin.")
add_feature_info(WAVPACK_INSTALL_DOCS WAVPACK_INSTALL_DOCS "Install documentation.")
add_feature_info(WAVPACK_INSTALL_PKGCONFIG_MODULE WAVPACK_INSTALL_PKGCONFIG_MODULE "Generate and install wavpack.pc.")

feature_summary(WHAT ALL)


# Installation

set(WAVPACK_INSTALL_TARGETS wavpack)
if(WAVPACK_BUILD_PROGRAMS)
	list(APPEND WAVPACK_INSTALL_TARGETS wavpackapp wvunpack wvgain wvtag)
endif()

install(TARGETS ${WAVPACK_INSTALL_TARGETS}
    EXPORT wavpack-targets
    RUNTIME DESTINATION         ${CMAKE_INSTALL_BINDIR}
    ARCHIVE DESTINATION         ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION         ${CMAKE_INSTALL_LIBDIR}
    PUBLIC_HEADER DESTINATION   ${CMAKE_INSTALL_INCLUDEDIR}/wavpack
)

if(WAVPACK_INSTALL_CMAKE_MODULE)

	if(WIN32)
		set(CMAKE_INSTALL_PACKAGEDIR cmake)
	else()
		set(CMAKE_INSTALL_PACKAGEDIR ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME})
	endif()

	if(CMAKE_VERSION VERSION_LESS 3.11)
		write_basic_package_version_file(wavpack-config-version.cmake COMPATIBILITY SameMajorVersion)
	else()
		write_basic_package_version_file(wavpack-config-version.cmake COMPATIBILITY SameMinorVersion)
    endif()

    configure_package_config_file(
        ${CMAKE_CURRENT_LIST_DIR}/wavpack-config.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/wavpack-config.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_PACKAGEDIR}
    )

    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/wavpack-config.cmake"
        DESTINATION ${CMAKE_INSTALL_PACKAGEDIR}
    )

	install(EXPORT wavpack-targets
		FILE wavpack-targets.cmake
		NAMESPACE WavPack::
		DESTINATION ${CMAKE_INSTALL_PACKAGEDIR}
	)
	install(
		FILES ${CMAKE_CURRENT_BINARY_DIR}/wavpack-config-version.cmake
		DESTINATION	${CMAKE_INSTALL_PACKAGEDIR}
	)
endif()

if(WAVPACK_INSTALL_PKGCONFIG_MODULE)
	set (prefix ${CMAKE_INSTALL_PREFIX})
	set (exec_prefix "\$\{prefix\}")
	set (libdir "\$\{prefix\}/${CMAKE_INSTALL_LIBDIR}")
	set (includedir "\$\{prefix\}/${CMAKE_INSTALL_INCLUDEDIR}")
	set (PACKAGE_VERSION ${PROJECT_VERSION})
	if (HAVE_LIBM)
		set (LIBM "-lm")
	endif ()
	configure_file (wavpack.pc.in wavpack.pc @ONLY)
	install(FILES ${CMAKE_CURRENT_BINARY_DIR}/wavpack.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
endif()

if(WAVPACK_INSTALL_DOCS)
    set(MAN_FILES_INSTALL_LIST "")
    foreach(WAVPACK_DOC_NAME ${WAVPACK_DOC_NAMES})
	    list(APPEND MAN_FILES_INSTALL_LIST ${CMAKE_CURRENT_SOURCE_DIR}/man/${WAVPACK_DOC_NAME}.1)
    endforeach()
    install(
        FILES ${MAN_FILES_INSTALL_LIST}
        DESTINATION ${CMAKE_INSTALL_MANDIR}/man1
    )
endif()

# Tests

if(BUILD_TESTING)
    enable_testing()

    add_executable(wvtest
        cli/wvtest.c
        cli/md5.c
        $<$<BOOL:${WIN32}>:cli/win32_unicode_support.h>
        $<$<BOOL:${WIN32}>:cli/win32_unicode_support.c>
    )
    target_compile_definitions(wvtest
        PRIVATE
            "PACKAGE_VERSION=\"${PROJECT_VERSION}\""
            "VERSION_OS=\"${CMAKE_SYSTEM_NAME}\"")
    target_link_libraries(wvtest
        PRIVATE
            wavpack
            $<$<NOT:$<BOOL:${WIN32}>>:Threads::Threads>
            $<$<BOOL:${HAVE_LIBM}>:m>
    )
    add_test(NAME wvtest COMMAND $<TARGET_FILE:wvtest> --exhaustive --short --no-extras)
endif()
