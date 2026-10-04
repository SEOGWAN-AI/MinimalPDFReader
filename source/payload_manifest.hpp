#pragma once
#include <windows.h>
struct PayloadFile { const wchar_t* name; int resource; DWORD bytes; };
static constexpr PayloadFile payloadFiles[] = {
    {L"PDFReader.exe",200,912896},
    {L"PDFReader.ico",201,50517},
    {L"Uninstall.exe",202,1028608},
    {L"使用說明.txt",203,3174},
    {L"LICENSE.txt",204,1287},
    {L"licenses/LLVM-libcxx-LICENSE.txt",205,16703},
    {L"licenses/LLVM-libcxxabi-LICENSE.txt",206,16706},
    {L"licenses/Microsoft-windows-rs-MIT.txt",207,1142},
    {L"licenses/MinGW-COPYING.txt",208,2326},
    {L"licenses/Zig-LICENSE.txt",209,1080},
    {L"install-id.txt",210,70},
};
