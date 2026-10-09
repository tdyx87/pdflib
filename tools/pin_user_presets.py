"""Pin CMakeUserPresets.json to a set of preset files that can coexist.

Every ``conan install`` appends its generated ``CMakePresets.json`` to the
project's ``CMakeUserPresets.json``. Two conan layouts (e.g. ``build/mt``
and ``build/all-backends``) both define a preset called ``conan-release``,
and CMake then refuses to read *any* preset:

    CMake Error: Could not read presets from <dir>: Duplicate preset: "conan-release"

This script rewrites the include list to exactly the paths given on the
command line, dropping any that do not exist yet, so ``cmake --preset``
keeps working whichever conan layout was installed last.

    python tools/pin_user_presets.py build/all-backends/build/Release/generators/CMakePresets.json
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TARGET = os.path.join(ROOT, "CMakeUserPresets.json")


def main(argv):
    keep = []
    for rel in argv:
        if os.path.exists(os.path.join(ROOT, rel)) and rel not in keep:
            keep.append(rel)

    data = {
        "version": 4,
        "vendor": {"conan": {}},
        "include": keep,
    }
    with open(TARGET, "w", encoding="utf-8") as handle:
        json.dump(data, handle, indent=4)
        handle.write("\n")
    print("CMakeUserPresets.json include ->", keep)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
