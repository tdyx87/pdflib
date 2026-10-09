from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout
from conan.tools.files import copy
import os


class PdfLibConan(ConanFile):
    name = "pdflib"
    version = "1.0.0"
    package_type = "library"

    settings = "os", "compiler", "build_type", "arch"
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
        "with_libharu": [True, False],
        "with_podofo": [True, False],
        "with_tests": [True, False],
        "with_examples": [True, False],
    }
    default_options = {
        "shared": False,
        "fPIC": True,
        "with_libharu": True,
        "with_podofo": True,
        "with_tests": True,
        "with_examples": True,
        # upstream library options
        "libharu/*:shared": False,
        # crypto-free podofo: the point of the 0.9.x track
        "podofo-local/*:with_openssl": False,
        "podofo-local/*:with_libidn": False,
        "podofo-local/*:shared": False,
        # libjpeg is deliberately off. PoDoFo 0.9.7 declares
        # jpeg_memory_src() itself and clashes with libjpeg's header
        # (C2375: redefinition with different linkage), and pdflib
        # exposes no JPEG path anyway. Pinned here rather than passed on
        # the command line so the dynamic and static builds cannot
        # diverge.
        "podofo-local/*:with_jpeg": False,
        "podofo-local/*:with_tiff": False,
    }

    exports_sources = (
        "CMakeLists.txt",
        "pdflib/*",
        "examples/*",
        "tests/*",
    )

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def configure(self):
        if self.options.shared:
            del self.options.fPIC

    def requirements(self):
        if self.options.with_libharu:
            self.requires("libharu/2.4.4")
        if self.options.with_podofo:
            # ConanCenter only packages podofo 1.0.x, which hard-depends on
            # OpenSSL. Use the bundled local recipe instead: podofo-local
            # (0.9.7, API-compatible with podofo::podofo) builds fine with
            # with_openssl=False. Export it via
            #   conan export recipes/podofo/all podofo-local/0.9.7@
            self.requires("podofo-local/0.9.7")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.variables["PDFLIB_WITH_LIBHARU"] = bool(self.options.with_libharu)
        tc.variables["PDFLIB_WITH_PODOFO"] = bool(self.options.with_podofo)
        tc.variables["PDFLIB_BUILD_TESTS"] = bool(self.options.with_tests)
        tc.variables["PDFLIB_BUILD_EXAMPLES"] = bool(self.options.with_examples)
        tc.generate()
        deps = CMakeDeps(self)
        deps.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
        if self.options.with_tests:
            cmake.test()

    def package(self):
        cmake = CMake(self)
        cmake.install()
        copy(self, "LICENSE*", src=self.export_folder, dst=os.path.join(self.package_folder, "licenses"))

    def package_info(self):
        self.cpp_info.libs = ["pdflib"]
        if self.options.with_libharu:
            self.cpp_info.defines.append("PDFLIB_HAS_LIBHARU")
        if self.options.with_podofo:
            self.cpp_info.defines.append("PDFLIB_HAS_PODOFO")
