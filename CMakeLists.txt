cmake_minimum_required(VERSION 3.15)

project(pdflib
    VERSION 1.0.0
    DESCRIPTION "A backend-agnostic C PDF generation wrapper (libharu / PoDoFo)"
    LANGUAGES C CXX)

option(PDFLIB_WITH_LIBHARU "Build the libharu backend" ON)
option(PDFLIB_WITH_PODOFO  "Build the PoDoFo backend" ON)
option(PDFLIB_BUILD_TESTS  "Build the test suite" ON)
option(PDFLIB_BUILD_EXAMPLES "Build the example programs" ON)
option(PDFLIB_BUILD_DRIVER "Build the WDK-facing C ABI facade DLL (pdflibdrv)" OFF)
option(PDFLIB_DRIVER_STATIC_CRT
       "Require pdflibdrv.dll to link the static CRT (/MT)" OFF)
option(PDFLIB_BUILD_HPDF_COMPAT
       "Build the libharu-compatible HPDF facade (hpdf.lib)" ON)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)

# ------------------------------------------------------------------ #
# Core library (backend-agnostic).                                   #
# ------------------------------------------------------------------ #
add_library(pdflib
    pdflib/pdf_api.h
    pdflib/pdf_backend.h
    pdflib/pdf_core.h
    pdflib/pdf_internal.h
    pdflib/pdf_core.c
    pdflib/pdf_init.c
    pdflib/backends/pdf_backend_dummy.c
)

target_include_directories(pdflib
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        # lets backends/ sources do #include "pdf_backend.h"
        ${CMAKE_CURRENT_SOURCE_DIR}/pdflib
)

target_compile_definitions(pdflib PRIVATE PDFLIB_BUILDING)

if(BUILD_SHARED_LIBS OR DEFINED PDFLIB_SHARED)
    target_compile_definitions(pdflib PUBLIC PDFLIB_SHARED)
endif()

# The dummy backend is always compiled in; register the real ones below.
find_package(Threads QUIET)

# ------------------------------------------------------------------ #
# libharu backend                                                    #
# ------------------------------------------------------------------ #
if(PDFLIB_WITH_LIBHARU)
    find_package(libharu CONFIG QUIET)
    if(NOT libharu_FOUND)
        find_package(haru CONFIG QUIET)  # alternative export name
    endif()

    if(libharu_FOUND OR haru_FOUND)
        target_sources(pdflib PRIVATE pdflib/backends/pdf_backend_libharu.c)
        if(libharu_FOUND)
            target_link_libraries(pdflib PRIVATE libharu::libharu)
        else()
            target_link_libraries(pdflib PRIVATE haru::haru)
        endif()
        target_compile_definitions(pdflib PRIVATE PDFLIB_BACKEND_LIBHARU_ENABLED)
        message(STATUS "pdflib: libharu backend enabled")
    else()
        message(FATAL_ERROR "PDFLIB_WITH_LIBHARU is ON but libharu was not found (conan: conan install -o with_libharu=True)")
    endif()
endif()

# ------------------------------------------------------------------ #
# PoDoFo backend (C++11 minimum; the 0.9.x API is used)              #
# ------------------------------------------------------------------ #
if(PDFLIB_WITH_PODOFO)
    find_package(podofo CONFIG REQUIRED)
    target_sources(pdflib PRIVATE pdflib/backends/pdf_backend_podofo.cpp)
    target_link_libraries(pdflib PRIVATE podofo::podofo)
    target_compile_definitions(pdflib PRIVATE PDFLIB_BACKEND_PODOFO_ENABLED)
    if(MSVC)
        target_compile_options(pdflib PRIVATE /EHsc)
    endif()
    # PoDoFo 0.9.x on Windows: imported shared lib needs this define so
    # PODOFO_API resolves to dllimport. Harmless for static builds.
    if(TARGET podofo::podofo)
        get_target_property(_podofo_type podofo::podofo TYPE)
        if(_podofo_type STREQUAL "SHARED_LIBRARY")
            target_compile_definitions(pdflib PRIVATE USING_SHARED_PODOFO)
        endif()
    endif()
    message(STATUS "pdflib: PoDoFo backend enabled (0.9.x API)")
endif()

# ------------------------------------------------------------------ #
# Warnings                                                           #
# ------------------------------------------------------------------ #
if(MSVC)
    target_compile_options(pdflib PRIVATE /W4)
else()
    target_compile_options(pdflib PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

set_target_properties(pdflib PROPERTIES
    PUBLIC_HEADER "pdflib/pdf_api.h"
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

# ------------------------------------------------------------------ #
# libharu-compatible facade (optional, source-level drop-in)          #
# ------------------------------------------------------------------ #
if(PDFLIB_BUILD_HPDF_COMPAT)
    add_library(hpdf STATIC
        compat/hpdf_compat.c
        compat/include/hpdf.h
        compat/include/hpdf_types.h
        compat/include/hpdf_consts.h
        compat/include/hpdf_errors.h
        compat/include/hpdf_config.h
        compat/include/hpdf_version.h)
    target_include_directories(hpdf
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/compat/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(hpdf PUBLIC pdflib)
    if(MSVC)
        target_compile_definitions(hpdf PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(hpdf PRIVATE /W3 /utf-8)
    else()
        target_compile_options(hpdf PRIVATE -Wall -Wextra)
    endif()
    set_target_properties(hpdf PROPERTIES OUTPUT_NAME hpdf)
endif()

# ------------------------------------------------------------------ #
# Tests                                                              #
# ------------------------------------------------------------------ #
if(PDFLIB_BUILD_TESTS)
    enable_testing()
    add_executable(pdflib_tests tests/test_pdflib.c)
    target_link_libraries(pdflib_tests PRIVATE pdflib)
    add_test(NAME pdflib_tests COMMAND pdflib_tests)
    # The tests exercise the core + dummy backend and must work even
    # when the third-party backends are disabled, so they only use the
    # dummy engine explicitly.
    target_compile_definitions(pdflib_tests PRIVATE PDFLIB_TEST_USE_DUMMY)

    # Concurrency and lifecycle contract tests (dummy backend only).
    add_executable(pdflib_thread_tests tests/test_threads.c)
    target_link_libraries(pdflib_thread_tests PRIVATE pdflib)
    target_compile_definitions(pdflib_thread_tests PRIVATE
                               PDFLIB_TEST_USE_DUMMY)
    add_test(NAME pdflib_thread_tests COMMAND pdflib_thread_tests)

    # Complex-document tests: rich multi-page PDFs, byte-level structure
    # verification, concurrency. Detects available backends at runtime,
    # so it also builds (and reports skips) in the core-only config.
    add_executable(pdflib_complex_tests tests/test_complex.c)
    target_link_libraries(pdflib_complex_tests PRIVATE pdflib)
    add_test(NAME pdflib_complex_tests COMMAND pdflib_complex_tests)
endif()

# ------------------------------------------------------------------ #
# Examples                                                           #
# ------------------------------------------------------------------ #
if(PDFLIB_BUILD_EXAMPLES)
    add_executable(pdf_hello examples/hello_pdf.c)
    target_link_libraries(pdf_hello PRIVATE pdflib)
    add_executable(feature_pdf examples/feature_pdf.c)
    target_link_libraries(feature_pdf PRIVATE pdflib)

    # Streaming-only example: requires the incremental writer and falls
    # back to PoDoFo when the active backend cannot stream.
    add_executable(streamed_pdf examples/streamed_pdf.c)
    target_link_libraries(streamed_pdf PRIVATE pdflib)

    # Streamed-mode stress tool: random content, memory growth check.
    # Windows needs psapi for GetProcessMemoryInfo.
    add_executable(stress_streamed tools/stress_streamed.c)
    target_link_libraries(stress_streamed PRIVATE pdflib)
    if(WIN32)
        target_link_libraries(stress_streamed PRIVATE psapi)
    endif()
endif()

# ------------------------------------------------------------------ #
# Driver facade DLL (pdflibdrv) - the only ABI a print driver sees.   #
#                                                                     #
# Built separately from the core so a driver package can ship exactly  #
# one DLL with exactly the exported names listed in pdflibdrv.def.     #
# The runtime library must match whatever the third-party backends     #
# were built with (the conan toolchain pins MultiThreadedDLL), or the  #
# link fails with LNK2038.                                             #
# ------------------------------------------------------------------ #
if(PDFLIB_BUILD_DRIVER)
    add_library(pdflibdrv SHARED
        driver/pdflibdrv.h
        driver/pdflibdrv.cpp
        driver/pdflibdrv.def)
    target_link_libraries(pdflibdrv PRIVATE pdflib)
    target_include_directories(pdflibdrv PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    # The ABI header must also be reachable as #include "pdflibdrv.h" from
    # the driver's own translation units.
    target_include_directories(pdflibdrv PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/driver>)
    if(MSVC)
        # /EHsc so catch(...) in the facade is legal C++.
        target_compile_options(pdflibdrv PRIVATE /EHsc /W4)
    endif()
    # Export names come from the .def, never from __declspec; the import
    # lib next to the DLL therefore carries the undecorated names.
    set_target_properties(pdflibdrv PROPERTIES OUTPUT_NAME pdflibdrv)

    # Export-surface guard: if dumpbin is available, fail the build when
    # the DLL's exports do not match pdflibdrv.def exactly. This is the
    # only automated protection against a silent `LIBRARY`/output-name
    # mismatch (LNK4070) or an accidental __declspec-driven decoration.
    find_program(PDFLIB_DUMPBIN dumpbin)
    if(WIN32 AND PDFLIB_DUMPBIN)
        add_custom_command(TARGET pdflibdrv POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -DDUMPBIN=${PDFLIB_DUMPBIN}
                -DDLL=$<TARGET_FILE:pdflibdrv>
                -DDEFFILE=${CMAKE_CURRENT_SOURCE_DIR}/driver/pdflibdrv.def
                -DREQUIRE_STATIC_CRT=$<BOOL:${PDFLIB_DRIVER_STATIC_CRT}>
                -P ${CMAKE_CURRENT_SOURCE_DIR}/driver/check_exports.cmake
            COMMENT "pdflibdrv: verifying export table against pdflibdrv.def")
    endif()

    # Consumer-side contract harness: loads the DLL by name through
    # GetProcAddress, exactly like a print filter would.
    if(WIN32)
        add_executable(pdflibdrv_harness driver/drv_harness.c)
        target_include_directories(pdflibdrv_harness PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/driver)
        if(MSVC)
            target_compile_options(pdflibdrv_harness PRIVATE /W4)
        endif()
        if(PDFLIB_BUILD_TESTS)
            enable_testing()
            add_test(NAME pdflibdrv_harness
                     COMMAND pdflibdrv_harness $<TARGET_FILE:pdflibdrv>)
        endif()

        # The other half of the C-ABI proof: a plain C translation unit
        # that includes pdflibdrv.h and links the .def import library
        # directly (no GetProcAddress). If an export were mangled or
        # missing from the import library this fails to link.
        add_executable(pdflibdrv_c_link_check driver/c_link_check.c)
        target_include_directories(pdflibdrv_c_link_check PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/driver)
        target_link_libraries(pdflibdrv_c_link_check PRIVATE pdflibdrv)
        if(MSVC)
            target_compile_options(pdflibdrv_c_link_check PRIVATE /W4)
        endif()
        if(PDFLIB_BUILD_TESTS)
            add_test(NAME pdflibdrv_c_link_check
                     COMMAND pdflibdrv_c_link_check)
        endif()

        # The XP half of the verification: a self-checking consumer meant
        # to be copied, together with pdflibdrv.dll, into a real Windows
        # XP machine. It is built by whatever toolset builds the DLL, so
        # in the x86-xp configuration it is itself a v141_xp /MT /5.01
        # binary - a pass there is about the DLL, not about the test.
        # It takes an optional DLL path or directory (default: next to
        # the executable), hence the ctest argument ordering.
        add_executable(pdflibdrv_xp_smoke tools/xp_smoke_test.c)
        target_include_directories(pdflibdrv_xp_smoke PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/driver)
        if(MSVC)
            target_compile_options(pdflibdrv_xp_smoke PRIVATE /W4)
        endif()
        if(PDFLIB_BUILD_TESTS)
            add_test(NAME pdflibdrv_xp_smoke
                     COMMAND pdflibdrv_xp_smoke $<TARGET_FILE_DIR:pdflibdrv>)
        endif()

        # Same program, linked against the .def import library instead of
        # resolving the exports at runtime, so the loader maps the DLL
        # before main() runs. On Windows XP that is not a stylistic
        # difference: a DLL loaded with LoadLibrary cannot use the static
        # TLS this one contains, and the two executables tell those two
        # failures apart.
        add_executable(pdflibdrv_xp_smoke_linked tools/xp_smoke_test.c)
        target_include_directories(pdflibdrv_xp_smoke_linked PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/driver)
        target_link_libraries(pdflibdrv_xp_smoke_linked PRIVATE pdflibdrv)
        target_compile_definitions(pdflibdrv_xp_smoke_linked PRIVATE
            PDFLIBDRV_SMOKE_LINK_DIRECT)
        if(MSVC)
            target_compile_options(pdflibdrv_xp_smoke_linked PRIVATE /W4)
        endif()
        if(PDFLIB_BUILD_TESTS)
            add_test(NAME pdflibdrv_xp_smoke_linked
                     COMMAND pdflibdrv_xp_smoke_linked)
        endif()
    endif()
endif()

# ------------------------------------------------------------------ #
# Install                                                            #
# ------------------------------------------------------------------ #
install(TARGETS pdflib
    EXPORT pdflibTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/pdflib
)

install(EXPORT pdflibTargets
    NAMESPACE pdflib::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pdflib
)

if(PDFLIB_BUILD_HPDF_COMPAT)
    install(TARGETS hpdf
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(FILES
        compat/include/hpdf.h
        compat/include/hpdf_types.h
        compat/include/hpdf_consts.h
        compat/include/hpdf_errors.h
        compat/include/hpdf_config.h
        compat/include/hpdf_version.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()
