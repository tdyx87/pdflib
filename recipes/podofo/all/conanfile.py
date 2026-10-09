"""Local conan recipe for PoDoFo 0.9.7.

ConanCenter only ships podofo 1.0.x, whose OpenSSL dependency is *mandatory*.
0.9.x makes libcrypto optional (FIND_PACKAGE(LIBCRYPTO), not REQUIRED), so
building "podofo without openssl" is only possible with 0.9.x. This recipe
wraps the upstream 0.9.7 tarball and exposes `with_openssl` as a first-class
option (default False) so consumers can build a crypto-free podofo.

Optional deps are also switchable so the dependency tree stays minimal:
  with_jpeg / with_png / with_tiff / with_libidn

The export name is deliberately "podofo-local" so it cannot collide with a
future ConanCenter package. Client code links podofo::podofo either way
(CMakeDeps maps the target name to the cpp_info set below).
"""

import os

from conan import ConanFile
from conan.tools.build import check_min_cppstd
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout
from conan.tools.files import copy, get, apply_conandata_patches
from conan.tools.scm import Version


class PodofoLocalConan(ConanFile):
    name = "podofo-local"
    version = "0.9.7"
    description = "PoDoFo 0.9.7 - PDF library with optional OpenSSL support"
    license = ("GPL-2.0-or-later", "LGPL-2.0-or-later")
    url = "https://github.com/podofo/podofo"
    homepage = "https://github.com/podofo/podofo"
    topics = ("pdf", "pdf-generation")
    package_type = "library"
    settings = "os", "compiler", "build_type", "arch"
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
        "with_openssl": [True, False],
        "with_libidn": [True, False],
        "with_jpeg": [True, False],
        "with_png": [True, False],
        "with_tiff": [True, False],
        "with_fontconfig": [True, False],
    }
    default_options = {
        "shared": False,
        "fPIC": True,
        # The whole point of this local recipe: crypto is optional.
        "with_openssl": False,
        "with_libidn": False,
        "with_jpeg": True,
        "with_png": True,
        "with_tiff": False,
        "with_fontconfig": False,
    }
    exports_sources = "CMakeLists.txt", "patches/*"
    generators = "CMakeDeps"

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC
            # fontconfig is a unix thing; upstream only enables it on UNIX
            del self.options.with_fontconfig

    def configure(self):
        if self.options.shared:
            del self.options.fPIC
        if not self.info:  # old conan 1 compat, harmless on conan 2
            pass
        # libidn only makes sense together with openssl (it enables AES-256)
        if not self.options.with_openssl:
            self.options.rm_safe("with_libidn")

    def layout(self):
        cmake_layout(self, src_folder="src")

    def requirements(self):
        self.requires("freetype/2.13.2")
        self.requires("zlib/[>=1.2.11 <2]")
        if self.options.get_safe("with_openssl"):
            self.requires("openssl/[>=1.1 <4]")
            if self.options.get_safe("with_libidn"):
                self.requires("libidn/1.36")
        if self.options.get_safe("with_jpeg"):
            self.requires("libjpeg/[>=9e]")
        if self.options.get_safe("with_png"):
            self.requires("libpng/[>=1.6 <2]")
        if self.options.get_safe("with_tiff"):
            self.requires("libtiff/4.6.0")
        if self.options.get_safe("with_fontconfig"):
            self.requires("fontconfig/2.15.0")

    def validate(self):
        check_min_cppstd(self, 11)

    def source(self):
        get(self, **self.conan_data["sources"][self.version],
            destination=self.source_folder, strip_root=True)
        # The upstream tarball has no top-level CMakeLists wrapper that this
        # recipe could drive, so we simply build in-source via the layout.

    def generate(self):
        tc = CMakeToolchain(self)
        # upstream switches
        tc.cache_variables["PODOFO_BUILD_STATIC"] = not self.options.shared
        tc.cache_variables["PODOFO_BUILD_SHARED"] = self.options.shared
        tc.cache_variables["PODOFO_BUILD_LIB_ONLY"] = True
        tc.cache_variables["PODOFO_NO_FONTMANAGER"] = not bool(
            self.options.get_safe("with_fontconfig", False))
        if self.settings.os == "Windows":
            # upstream adds vcincludes to the include path via EXTRA_INCLUDES
            tc.variables["EXTRA_INCLUDES"] = os.path.join(
                self.source_folder, "vcincludes").replace("\\", "/")
        # Upstream's bundled FindFREETYPE.cmake only searches hardcoded
        # unix paths with NO_CMAKE_SYSTEM_PATH, so on Windows we must
        # pre-seed its cache variables with the conan freetype location.
        ft = self.dependencies["freetype"]
        # ft2build.h may sit directly in an includedir or one level down
        # (upstream installs into <prefix>/include/freetype2/).
        ft2_dir = None
        for d in ft.cpp_info.includedirs:
            if os.path.isfile(os.path.join(d, "ft2build.h")):
                ft2_dir = d
                break
            sub = os.path.join(d, "freetype2")
            if os.path.isfile(os.path.join(sub, "ft2build.h")):
                ft2_dir = sub
                break
        if ft2_dir is None:
            raise ConanException("freetype: ft2build.h not found in %s" %
                                 ft.cpp_info.includedirs)
        ft2_dir = ft2_dir.replace("\\", "/")
        ft_libdir = ft.cpp_info.libdirs[0].replace("\\", "/")
        ft_lib = os.path.join(ft_libdir, "freetype.lib").replace("\\", "/")
        if not os.path.isfile(ft_lib):
            ft_lib = os.path.join(ft_libdir, "libfreetype.a").replace("\\", "/")
        # both variables must be the dir that CONTAINS freetype/config/ftheader.h
        tc.cache_variables["FREETYPE_INCLUDE_DIR_FT2BUILD"] = ft2_dir
        tc.cache_variables["FREETYPE_INCLUDE_DIR_FTHEADER"] = ft2_dir
        tc.cache_variables["FREETYPE_LIBRARY_RELEASE"] = ft_lib
        tc.cache_variables["FREETYPE_LIBRARY_DEBUG"] = ft_lib
        # CMakeDeps also ships a config-based FindFREETYPE.cmake that wins
        # over upstream's broken module and defines target-only variables,
        # leaving the upstream variable names empty. Pre-seed them directly.
        tc.cache_variables["FREETYPE_INCLUDE_DIR"] = ft2_dir
        tc.cache_variables["FREETYPE_LIBRARIES"] = ft_lib
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        copy(self, "COPYING", src=self.source_folder,
             dst=os.path.join(self.package_folder, "licenses"))
        copy(self, "COPYING.LIB", src=self.source_folder,
             dst=os.path.join(self.package_folder, "licenses"))
        copy(self, "COPYING.exception", src=self.source_folder,
             dst=os.path.join(self.package_folder, "licenses"))
        cmake = CMake(self)
        cmake.install()

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "podofo")
        self.cpp_info.set_property("cmake_target_name", "podofo::podofo")
        libname = "podofo"
        self.cpp_info.libs = [libname]
        # upstream defines these when building against shared podofo on win32
        if self.options.shared:
            self.cpp_info.defines.append("USING_SHARED_PODOFO")
        # The generated podofo_config.h ends up in the build dir and is
        # installed to include/; nothing extra needed.

        # freetype headers are referenced by public headers (PdfFontMetrics.h
        # includes ft2build.h)
        self.cpp_info.requires = []
        if self.options.get_safe("with_openssl"):
            self.cpp_info.requires.append("openssl::openssl")
            if self.options.get_safe("with_libidn"):
                self.cpp_info.requires.append("libidn::libidn")
        if self.options.get_safe("with_jpeg"):
            self.cpp_info.requires.append("libjpeg::libjpeg")
        if self.options.get_safe("with_png"):
            self.cpp_info.requires.append("libpng::libpng")
        if self.options.get_safe("with_tiff"):
            self.cpp_info.requires.append("libtiff::libtiff")
        if self.options.get_safe("with_fontconfig"):
            self.cpp_info.requires.append("fontconfig::fontconfig")
        self.cpp_info.requires.append("freetype::freetype")
        self.cpp_info.requires.append("zlib::zlib")

        if self.settings.os in ("Linux", "FreeBSD"):
            self.cpp_info.system_libs = ["pthread"]
        elif self.settings.os == "Windows":
            self.cpp_info.system_libs = ["ws2_32"]
