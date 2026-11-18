#!/usr/bin/env python3
"""Run one of the translation repository's tools on this disc's files.

Their builders read the original files from <repo>/data/psx and a few constants from the PS1
decompilation at <repo>/references/persona-decomp. This runner points those at our files
instead (P1_DECOMP, default ./_decomp: a clone of github.com/daanhenke/persona-psx) without
touching the submodule.

usage: run_tl_tool.py <tool.py> [args...]      e.g. run_tl_tool.py talk.py build
"""
import os
import runpy
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = os.environ.get('P1_TL', os.path.join(ROOT, 'third_party', 'persona-translation'))
DECOMP = os.environ.get('P1_DECOMP', os.path.join(ROOT, '_decomp'))
TOOLS = os.path.join(REPO, 'src', 'injection_tools')


def main():
    tool = sys.argv[1]
    sys.path.insert(0, TOOLS)
    os.chdir(REPO)
    import vwf_wrap
    vwf_wrap.DECOMP_BTLP = os.path.join(DECOMP, 'src', 'btlp')
    sys.argv = [os.path.join(TOOLS, tool)] + sys.argv[2:]
    runpy.run_path(sys.argv[0], run_name='__main__')


if __name__ == '__main__':
    main()
