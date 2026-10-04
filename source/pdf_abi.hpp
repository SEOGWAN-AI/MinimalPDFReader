#pragma once
#include <windows.h>
#include <inspectable.h>
#include <roapi.h>
#include <winstring.h>
#include <asyncinfo.h>

// Minimal Windows Runtime ABI declarations, checked against Microsoft's
// microsoft/windows-rs Windows/Data/Pdf generated bindings (MIT license).
namespace pdfabi {
constexpr GUID documentIID = {0xac7ebedd,0x80fa,0x4089,{0x84,0x6e,0x81,0xb7,0x7f,0xf5,0xa8,0x6c}};
constexpr GUID staticsIID = {0x433a0b5f,0xc007,0x4788,{0x90,0xf2,0x08,0x14,0x3d,0x92,0x25,0x99}};
constexpr GUID optionsIID = {0x3c98056f,0xb7cf,0x4c29,{0x9a,0x04,0x52,0xd9,0x02,0x67,0xf4,0x25}};
constexpr GUID randomStreamIID = {0x905a0fe1,0xbc53,0x11df,{0x8c,0x49,0x00,0x1e,0x4f,0xc6,0x86,0xda}};
constexpr GUID closableIID = {0x30d5a829,0x7fa4,0x4026,{0x83,0xbb,0xd7,0x5b,0xae,0x4e,0xa9,0x9e}};
struct Size { float width, height; };
struct Rect { float x, y, width, height; };
struct Color { BYTE a, r, g, b; };
struct Document; struct Page; struct RenderOptions;
struct Action : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(IUnknown*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(IUnknown**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults() = 0;
};
struct LoadOperation : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(IUnknown*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(IUnknown**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(Document**) = 0;
};
struct Document : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetPage(UINT32, Page**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PageCount(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPasswordProtected(boolean*) = 0;
};
struct Statics : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE LoadFromFileAsync(IUnknown*,LoadOperation**) = 0;
    virtual HRESULT STDMETHODCALLTYPE LoadFromFileWithPasswordAsync(IUnknown*,HSTRING,LoadOperation**) = 0;
    virtual HRESULT STDMETHODCALLTYPE LoadFromStreamAsync(IUnknown*,LoadOperation**) = 0;
    virtual HRESULT STDMETHODCALLTYPE LoadFromStreamWithPasswordAsync(IUnknown*,HSTRING,LoadOperation**) = 0;
};
struct Page : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE RenderToStreamAsync(IUnknown*,Action**) = 0;
    virtual HRESULT STDMETHODCALLTYPE RenderWithOptionsToStreamAsync(IUnknown*,RenderOptions*,Action**) = 0;
    virtual HRESULT STDMETHODCALLTYPE PreparePageAsync(Action**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Index(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(Size*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Dimensions(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Rotation(INT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PreferredZoom(float*) = 0;
};
struct RenderOptions : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_SourceRect(Rect*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_SourceRect(Rect) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DestinationWidth(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_DestinationWidth(UINT32) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DestinationHeight(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_DestinationHeight(UINT32) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BackgroundColor(Color*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_BackgroundColor(Color) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsIgnoringHighContrast(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsIgnoringHighContrast(boolean) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BitmapEncoderId(GUID*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_BitmapEncoderId(GUID) = 0;
};
struct Closable : IInspectable { virtual HRESULT STDMETHODCALLTYPE Close() = 0; };

template<class T> class Com {
    T* value = nullptr;
public:
    Com() = default;
    ~Com() { reset(); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    T* get() const { return value; }
    T* operator->() const { return value; }
    T** out() { reset(); return &value; }
    void** outVoid() { return reinterpret_cast<void**>(out()); }
    explicit operator bool() const { return value != nullptr; }
    void reset() { if (value) { value->Release(); value = nullptr; } }
};
class String {
    HSTRING value = nullptr;
public:
    explicit String(const wchar_t* s) { WindowsCreateString(s,UINT32(wcslen(s)),&value); }
    ~String() { WindowsDeleteString(value); }
    HSTRING get() const { return value; }
};
inline void closePage(Page* page) {
    Com<Closable> closer;
    if (SUCCEEDED(page->QueryInterface(closableIID,closer.outVoid()))) closer->Close();
}
}
