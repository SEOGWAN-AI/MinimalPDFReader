#!/usr/bin/env python3
"""Build a per-user Windows setup with native, offline embedded resources."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys

parser=argparse.ArgumentParser()
parser.add_argument('--zig',default='zig')
args=parser.parse_args()
zig=str(Path(args.zig).resolve()) if Path(args.zig).exists() else args.zig
source=Path(__file__).resolve().parent
project=source.parent
package=project/'package'
build=project/'installer-build'
build.mkdir(exist_ok=True)

def command(arguments):
    subprocess.run([zig,*arguments],cwd=source,check=True)

subprocess.run([sys.executable,str(source/'build.py'),'--zig',zig],check=True)
shutil.copyfile(package/'PDFReader.exe',project/'PDFReader.exe')
(build/'install-id.txt').write_text('MinimalPDFReader:{7C063285-4FE0-4BA4-AAC8-918DF65D943A}\nversion=1.0.0\n',encoding='ascii')
payload=[
    ('PDFReader.exe',project/'PDFReader.exe'),
    ('PDFReader.ico',project/'assets/PDFReader.ico'),
    ('Uninstall.exe',build/'Uninstall.exe'),
    ('使用說明.txt',project/'使用說明.txt'),
    ('LICENSE.txt',project/'LICENSE.txt'),
]
for path in sorted((project/'licenses').iterdir()):
    if path.is_file(): payload.append(('licenses/'+path.name,path))
payload.append(('install-id.txt',build/'install-id.txt'))

def manifest(complete):
    header=['#pragma once','#include <windows.h>',
            'struct PayloadFile { const wchar_t* name; int resource; DWORD bytes; };',
            'static constexpr PayloadFile payloadFiles[] = {']
    data=[]
    for index,(name,path) in enumerate(payload,200):
        relative=PurePosixPath(name)
        assert not relative.is_absolute() and '..' not in relative.parts
        size=path.stat().st_size if path.exists() else 0
        if complete: assert size>0
        header.append('    {L'+json.dumps(name,ensure_ascii=False)+','+str(index)+','+str(size)+'},')
        data.append({'name':name,'id':index,'bytes':size,
                     'sha256':hashlib.sha256(path.read_bytes()).hexdigest() if size else None})
    header.append('};')
    (source/'payload_manifest.hpp').write_text('\n'.join(header)+'\n',encoding='utf-8')
    return data

resource_base='#include <windows.h>\n101 ICON "../assets/PDFReader.ico"\n1 RT_MANIFEST "reader.manifest"\n'
version='''1 VERSIONINFO
 FILEVERSION 1,0,0,0
 PRODUCTVERSION 1,0,0,0
 FILEFLAGSMASK 0x3fL
 FILEFLAGS 0
 FILEOS VOS_NT_WINDOWS32
 FILETYPE VFT_APP
BEGIN
 BLOCK "StringFileInfo"
 BEGIN
  BLOCK "040904b0"
  BEGIN
   VALUE "FileDescription", "%s\\0"
   VALUE "FileVersion", "1.0.0\\0"
   VALUE "ProductName", "Minimal PDF Reader\\0"
   VALUE "ProductVersion", "1.0.0\\0"
  END
 END
 BLOCK "VarFileInfo"
 BEGIN
  VALUE "Translation", 0x409, 1200
 END
END
'''
flags=['c++','-target','x86_64-windows-gnu','-std=c++17','-O2','-static',
       '-municode','-Wl,--subsystem,windows','-fno-ident']
libraries=['-luser32','-lgdi32','-lshell32','-lole32','-luuid','-ladvapi32','-lcomctl32','-ldwmapi']

manifest(False)
(source/'uninstall.rc').write_text(resource_base+version%'Minimal PDF Reader Uninstall',encoding='utf-8')
command(['rc','/fo',str(build/'uninstall.res'),'uninstall.rc'])
command(flags+['-DBUILD_UNINSTALLER=1','setup.cpp',str(build/'uninstall.res'),'-o',str(build/'Uninstall.exe')]+libraries)

records=manifest(True)
resources=[resource_base,version%'Minimal PDF Reader Setup']
for record,(_,path) in zip(records,payload):
    staging=build/('payload-'+str(record['id'])+'.bin')
    shutil.copyfile(path,staging)
    resources.append(str(record['id'])+' RCDATA '+json.dumps(staging.as_posix())+'\n')
(source/'setup.rc').write_text(''.join(resources),encoding='utf-8')
command(['rc','/fo',str(build/'setup.res'),'setup.rc'])
output=project.parent/'MinimalPDFReader-Setup-Win11-v1.0.exe'
command(flags+['setup.cpp',str(build/'setup.res'),'-o',str(output)]+libraries)
(build/'payload.json').write_text(json.dumps(records,indent=2,ensure_ascii=False),encoding='utf-8')
print(output)
