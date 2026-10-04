#!/usr/bin/env python3
"""Build on Linux/macOS/Windows with official Zig 0.14.1 (or compatible)."""
import argparse
import pathlib
import subprocess

p=argparse.ArgumentParser()
p.add_argument('--zig',default='zig',help='path to the official Zig compiler')
args=p.parse_args()
root=pathlib.Path(__file__).resolve().parent
destination=root.parent/'package'
destination.mkdir(exist_ok=True)
subprocess.run([args.zig,'rc','/fo',str(root/'reader.res'),str(root/'reader.rc')],cwd=root,check=True)
subprocess.run([args.zig,'c++','-target','x86_64-windows-gnu','-std=c++17','-O2','-static',
    '-municode','-Wl,--subsystem,windows','-fno-ident',str(root/'main.cpp'),str(root/'reader.res'),
    '-o',str(destination/'PDFReader.exe'),'-luser32','-lgdi32','-lcomdlg32','-lshell32',
    '-lole32','-lshcore','-ldwmapi','-lwindowscodecs','-luuid',
    '-lapi-ms-win-core-winrt-l1-1-0','-lapi-ms-win-core-winrt-string-l1-1-0'],cwd=root,check=True)
print(destination/'PDFReader.exe')
