"""Run the Conan CLI exactly as the `conan` entry point would.

Some Windows Python installations ship a `conan.exe` launcher whose
embedded interpreter path no longer exists (e.g. it points at
``C:\\Python313`` while Python actually lives in ``D:\\Python313``). In
that case the launcher exits without doing anything, and the CLI has to
be reached through the package instead:

    python tools/conan_main.py install . -of build/mt ...

Any argument is forwarded verbatim.
"""
import sys

from conan.cli.cli import main

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
