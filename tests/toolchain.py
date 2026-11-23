"""One way to find the host C compiler for every test that builds a C fixture."""
import os
from pathlib import Path
import shutil


def host_compiler(*names):
    """Path of the first available compiler among names (default clang, gcc, cc).

    Visual Studio and the LLVM installer do not put clang on PATH, so their
    usual folders are checked too. A missing compiler is an error, not a skip:
    these fixtures execute the real C sources.
    """
    names = names or ('clang', 'gcc', 'cc')
    for name in names:
        found = shutil.which(name)
        if found:
            return found
    if os.name == 'nt' and 'clang' in names:
        base = Path(os.environ.get('ProgramFiles', 'C:/Program Files'))
        folders = [base / 'Microsoft Visual Studio/2022' / edition / 'VC/Tools/Llvm/x64/bin'
                   for edition in ('Community', 'Professional', 'Enterprise', 'BuildTools')]
        for folder in (*folders, base / 'LLVM/bin'):
            if (folder / 'clang.exe').is_file():
                return str(folder / 'clang.exe')
    raise RuntimeError(' or '.join(names) + ' is required to build the C test fixtures')
