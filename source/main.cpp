#ifndef UNICODE
#define UNICODE
#endif
#define _UNICODE
#define NOMINMAX
#define WINVER 0x0A00
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shcore.h>
#include <wincodec.h>
#include <dwmapi.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include "layout.hpp"
#include "pdf_abi.hpp"

using namespace pdfabi;
constexpr UINT resultMessage = WM_APP + 1, startupMessage = WM_APP + 2;
constexpr UINT_PTR renderTimer = 1;
constexpr size_t cacheBudget = 160 * 1024 * 1024;

struct Image {
    UINT width = 0, height = 0;
    std::vector<BYTE> pixels;
};
struct OpenRequest { unsigned token; std::wstring path, password; };
struct RenderRequest { unsigned token; size_t page; UINT width; };
enum class ResultKind { Loaded, Rendered, Failed, RenderFailed };
struct Result {
    unsigned token;
    ResultKind kind;
    size_t page = 0;
    UINT requestedWidth = 0;
    HRESULT error = S_OK;
    std::wstring path;
    std::vector<reader::Size> sizes;
    std::shared_ptr<Image> image;
};

class Engine {
    HWND target;
    std::mutex mutex;
    std::condition_variable condition;
    std::optional<OpenRequest> nextOpen;
    std::deque<RenderRequest> requests;
    std::thread thread;
    std::atomic<bool> stopping{false};
    std::atomic<unsigned> currentToken{0};

    HRESULT wait(IUnknown* operation, unsigned token) {
        Com<IAsyncInfo> info;
        HRESULT hr = operation->QueryInterface(__uuidof(IAsyncInfo), info.outVoid());
        if (FAILED(hr)) return hr;
        for (;;) {
            if (stopping || token != currentToken) {
                info->Cancel(); return HRESULT_FROM_WIN32(ERROR_CANCELLED);
            }
            AsyncStatus status;
            hr = info->get_Status(&status);
            if (FAILED(hr)) return hr;
            if (status == Completed) return S_OK;
            if (status == Canceled) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
            if (status == Error) {
                hr = E_FAIL; info->get_ErrorCode(&hr); return hr;
            }
            std::unique_lock<std::mutex> lock(mutex);
            condition.wait_for(lock, std::chrono::milliseconds(12), [&]{
                return stopping || token != currentToken;
            });
        }
    }
    void deliver(std::unique_ptr<Result> result) {
        if (stopping || result->token != currentToken) return;
        if (PostMessageW(target,resultMessage,0,reinterpret_cast<LPARAM>(result.get())))
            result.release();
    }
    HRESULT load(const OpenRequest& request, Com<Document>& document,
                 std::vector<reader::Size>& sizes) {
        Com<Statics> factory;
        String className(L"Windows.Data.Pdf.PdfDocument");
        HRESULT hr = RoGetActivationFactory(className.get(),staticsIID,factory.outVoid());
        if (FAILED(hr)) return hr;
        Com<IUnknown> stream;
        hr = CreateRandomAccessStreamOnFile(request.path.c_str(),STGM_READ,
                                             randomStreamIID,stream.outVoid());
        if (FAILED(hr)) return hr;
        Com<LoadOperation> operation;
        if (request.password.empty()) {
            hr = factory->LoadFromStreamAsync(stream.get(),operation.out());
        } else {
            String password(request.password.c_str());
            hr = factory->LoadFromStreamWithPasswordAsync(stream.get(),password.get(),operation.out());
        }
        if (FAILED(hr)) return hr;
        hr = wait(operation.get(),request.token);
        if (FAILED(hr)) return hr;
        hr = operation->GetResults(document.out());
        if (FAILED(hr)) return hr;
        UINT32 count = 0;
        hr = document->get_PageCount(&count);
        if (FAILED(hr) || count == 0) return FAILED(hr) ? hr : E_INVALIDARG;
        sizes.reserve(count);
        for (UINT32 i = 0; i < count; ++i) {
            if (stopping || request.token != currentToken)
                return HRESULT_FROM_WIN32(ERROR_CANCELLED);
            Com<Page> page;
            hr = document->GetPage(i,page.out());
            if (FAILED(hr)) return hr;
            Size size{};
            hr = page->get_Size(&size);
            closePage(page.get());
            if (FAILED(hr) || !std::isfinite(size.width) || !std::isfinite(size.height) ||
                size.width <= 0 || size.height <= 0) return E_INVALIDARG;
            sizes.push_back({size.width,size.height});
        }
        return S_OK;
    }
    HRESULT render(Document* document, const RenderRequest& request,
                   std::shared_ptr<Image>& result) {
        Com<Page> page;
        HRESULT hr = document->GetPage(UINT32(request.page),page.out());
        if (FAILED(hr)) return hr;
        struct PageCloser { Page* page; ~PageCloser(){ closePage(page); } } closer{page.get()};
        Size size{};
        hr = page->get_Size(&size);
        if (FAILED(hr)) return hr;
        auto raster = reader::rasterSize({size.width,size.height},request.width);
        Com<IInspectable> instance;
        String className(L"Windows.Data.Pdf.PdfPageRenderOptions");
        hr = RoActivateInstance(className.get(),instance.out());
        if (FAILED(hr)) return hr;
        Com<RenderOptions> options;
        hr = instance->QueryInterface(optionsIID,options.outVoid());
        if (FAILED(hr)) return hr;
        hr = options->put_DestinationWidth(UINT32(raster.width));
        if (FAILED(hr)) return hr;
        hr = options->put_DestinationHeight(UINT32(raster.height));
        if (FAILED(hr)) return hr;
        hr = options->put_BackgroundColor({255,255,255,255});
        if (FAILED(hr)) return hr;
        Com<IStream> stream;
        hr = CreateStreamOnHGlobal(nullptr,TRUE,stream.out());
        if (FAILED(hr)) return hr;
        Com<IUnknown> output;
        hr = CreateRandomAccessStreamOverStream(stream.get(),BSOS_DEFAULT,
                                                randomStreamIID,output.outVoid());
        if (FAILED(hr)) return hr;
        Com<Action> operation;
        hr = page->RenderWithOptionsToStreamAsync(output.get(),options.get(),operation.out());
        if (FAILED(hr)) return hr;
        hr = wait(operation.get(),request.token);
        if (FAILED(hr)) return hr;
        hr = operation->GetResults();
        if (FAILED(hr)) return hr;
        LARGE_INTEGER zero{};
        hr = stream->Seek(zero,STREAM_SEEK_SET,nullptr);
        if (FAILED(hr)) return hr;
        Com<IWICImagingFactory> wic;
        hr = CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,
                              IID_IWICImagingFactory,wic.outVoid());
        if (FAILED(hr)) return hr;
        Com<IWICBitmapDecoder> decoder;
        hr = wic->CreateDecoderFromStream(stream.get(),nullptr,WICDecodeMetadataCacheOnLoad,decoder.out());
        if (FAILED(hr)) return hr;
        Com<IWICBitmapFrameDecode> frame;
        hr = decoder->GetFrame(0,frame.out());
        if (FAILED(hr)) return hr;
        Com<IWICFormatConverter> converter;
        hr = wic->CreateFormatConverter(converter.out());
        if (FAILED(hr)) return hr;
        hr = converter->Initialize(frame.get(),GUID_WICPixelFormat32bppBGR,
                                  WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom);
        if (FAILED(hr)) return hr;
        auto image = std::make_shared<Image>();
        hr = converter->GetSize(&image->width,&image->height);
        if (FAILED(hr)) return hr;
        if (!image->width || !image->height ||
            uint64_t(image->width) * image->height > 17000000) return E_OUTOFMEMORY;
        const UINT stride = image->width * 4;
        image->pixels.resize(size_t(stride) * image->height);
        hr = converter->CopyPixels(nullptr,stride,UINT(image->pixels.size()),image->pixels.data());
        if (SUCCEEDED(hr)) result = std::move(image);
        return hr;
    }
    void work(HRESULT initialization) {
        Com<Document> document;
        unsigned loadedToken = 0;
        for (;;) {
            std::optional<OpenRequest> open;
            std::optional<RenderRequest> draw;
            {
                std::unique_lock<std::mutex> lock(mutex);
                condition.wait(lock,[&]{return stopping || nextOpen || !requests.empty();});
                if (stopping) return;
                if (nextOpen) { open = std::move(nextOpen); nextOpen.reset(); requests.clear(); }
                else { draw = requests.front(); requests.pop_front(); }
            }
            if (open) {
                document.reset(); loadedToken = 0;
                auto result = std::make_unique<Result>();
                result->token = open->token; result->path = open->path;
                try {
                    result->error = FAILED(initialization) ? initialization : load(*open,document,result->sizes);
                } catch (...) { result->error = E_OUTOFMEMORY; document.reset(); }
                if (!open->password.empty())
                    SecureZeroMemory(open->password.data(),open->password.size() * sizeof(wchar_t));
                result->kind = SUCCEEDED(result->error) ? ResultKind::Loaded : ResultKind::Failed;
                if (SUCCEEDED(result->error)) loadedToken = open->token;
                deliver(std::move(result));
            } else if (draw && document && draw->token == loadedToken && draw->token == currentToken) {
                auto result = std::make_unique<Result>();
                result->token = draw->token; result->page = draw->page;
                result->requestedWidth = draw->width;
                try { result->error = render(document.get(),*draw,result->image); }
                catch (...) { result->error = E_OUTOFMEMORY; }
                result->kind = SUCCEEDED(result->error) ? ResultKind::Rendered : ResultKind::RenderFailed;
                deliver(std::move(result));
            }
        }
    }
public:
    explicit Engine(HWND hwnd) : target(hwnd) {
        thread = std::thread([this]{
            const HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
            {
                try { work(hr); } catch (...) {
                    auto result = std::make_unique<Result>();
                    result->token = currentToken; result->kind = ResultKind::Failed;
                    result->error = E_OUTOFMEMORY; deliver(std::move(result));
                }
                if (SUCCEEDED(hr)) RoUninitialize();
            }
        });
    }
    ~Engine() { stop(); }
    void open(OpenRequest request) {
        currentToken = request.token;
        { std::lock_guard<std::mutex> lock(mutex); nextOpen = std::move(request); requests.clear(); }
        condition.notify_all();
    }
    void draw(std::vector<RenderRequest> values) {
        { std::lock_guard<std::mutex> lock(mutex); requests.assign(values.begin(),values.end()); }
        condition.notify_all();
    }
    void stop() {
        stopping = true; condition.notify_all();
        if (thread.joinable()) thread.join();
    }
};

struct CacheEntry { std::shared_ptr<Image> image; UINT requestedWidth; ULONGLONG touched; };
struct Application {
    HWND window = nullptr;
    reader::Layout layout;
    std::unique_ptr<Engine> engine;
    unsigned token = 0;
    std::map<size_t,CacheEntry> cache;
    std::map<size_t,UINT> failedRenders;
    std::wstring path, message = L"將 PDF 拖入視窗，或按 Ctrl+O 開啟。";
    bool loading = false, updatingBars = false, horizontal = false;
    bool dragging = false, fullScreen = false;
    POINT lastMouse{}, lastPan{}, lastPinch{};
    ULONGLONG gestureDistance = 0;
    WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
    DWORD windowStyle = WS_OVERLAPPEDWINDOW;
    std::wstring initialPath;
    bool diagnostic = false;
    int exitCode = 0;
    std::ofstream diagnosticFile;
};
static Application app;

static std::wstring baseName(const std::wstring& path) {
    const auto slash = path.find_last_of(L"/\\");
    return path.substr(slash == std::wstring::npos ? 0 : slash + 1);
}
static std::wstring errorText(HRESULT hr) {
    wchar_t hex[32]; swprintf(hex,32,L"0x%08lX",static_cast<unsigned long>(hr));
    return std::wstring(L"無法讀取這個 PDF。\n請確認檔案完整且可讀取。\n錯誤碼：") + hex;
}

static void updateBars() {
    if (app.updatingBars) return;
    app.updatingBars = true;
    const bool horizontal = app.layout.maxX() > .5;
    if (horizontal != app.horizontal) {
        app.horizontal = horizontal;
        ShowScrollBar(app.window,SB_HORZ,horizontal);
        RECT client{}; GetClientRect(app.window,&client);
        app.layout.width = std::max(1L,client.right);
        app.layout.height = std::max(1L,client.bottom);
        app.layout.reflow();
    }
    auto bar = [&](int which, double extent, double view, double offset) {
        const double factor = extent > 1000000000.0 ? 1000000000.0 / extent : 1;
        SCROLLINFO info{sizeof(info),SIF_RANGE|SIF_PAGE|SIF_POS|SIF_DISABLENOSCROLL};
        info.nMin = 0; info.nMax = std::max(0,int(std::ceil(extent * factor)) - 1);
        info.nPage = UINT(std::min(1000000000.0,std::ceil(view * factor)));
        info.nPos = int(std::round(offset * factor));
        SetScrollInfo(app.window,which,&info,TRUE);
    };
    bar(SB_VERT,app.layout.totalHeight,app.layout.height,app.layout.scrollY);
    if (app.horizontal) bar(SB_HORZ,app.layout.pageWidth(),app.layout.width,app.layout.scrollX);
    app.updatingBars = false;
}
static void queueRendering() {
    if (!app.engine || app.layout.pages.empty()) return;
    std::vector<RenderRequest> requests;
    for (size_t i : app.layout.visible()) {
        const UINT width = UINT(std::ceil(app.layout.pageWidth()));
        const auto existing = app.cache.find(i);
        const auto failed = app.failedRenders.find(i);
        if ((existing == app.cache.end() || existing->second.requestedWidth != width) &&
            (failed == app.failedRenders.end() || failed->second != width))
            requests.push_back({app.token,i,width});
    }
    app.engine->draw(std::move(requests));
}
static void changed(bool delayRender = false) {
    updateBars(); InvalidateRect(app.window,nullptr,FALSE);
    SetTimer(app.window,renderTimer,delayRender ? 140 : 25,nullptr);
}
static void openPath(std::wstring path, std::wstring password = L"") {
    if (path.empty()) return;
    wchar_t resolved[32768];
    const DWORD length = GetFullPathNameW(path.c_str(),32768,resolved,nullptr);
    if (!length || length >= 32768) { MessageBoxW(app.window,L"檔案路徑過長或無效。",L"開啟 PDF",MB_OK); return; }
    app.path = resolved;
    ++app.token; app.cache.clear(); app.failedRenders.clear();
    app.layout.pages.clear(); app.layout.zoom = 1; app.layout.scrollX = app.layout.scrollY = 0;
    app.layout.reflow(); app.loading = true; app.message = L"正在開啟 PDF…";
    SetWindowTextW(app.window,(baseName(app.path) + L" — 極簡 PDF 閱讀器").c_str());
    app.engine->open({app.token,app.path,std::move(password)});
    changed();
}
static void chooseFile() {
    wchar_t file[32768]{};
    OPENFILENAMEW picker{sizeof(picker)};
    picker.hwndOwner = app.window;
    picker.lpstrFilter = L"PDF 文件 (*.pdf)\0*.pdf\0\0";
    picker.lpstrFile = file; picker.nMaxFile = 32768;
    picker.lpstrTitle = L"開啟 PDF";
    picker.Flags = OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_EXPLORER|OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&picker)) openPath(file);
}

struct PasswordDialog { std::wstring value; bool accepted = false; };
static INT_PTR CALLBACK passwordProcedure(HWND window, UINT message, WPARAM w, LPARAM l) {
    auto* state = reinterpret_cast<PasswordDialog*>(GetWindowLongPtrW(window,DWLP_USER));
    if (message == WM_INITDIALOG) {
        state = reinterpret_cast<PasswordDialog*>(l); SetWindowLongPtrW(window,DWLP_USER,l);
        RECT r{}; GetClientRect(window,&r);
        const int unit = std::max(1,int(GetDpiForWindow(window)/96));
        const int margin = 16*unit, width = r.right - margin*2;
        auto control = [&](const wchar_t* cls,const wchar_t* text,DWORD style,
                           int x,int y,int cw,int ch,int id){
            HWND child = CreateWindowExW(0,cls,text,WS_CHILD|WS_VISIBLE|style,
                x,y,cw,ch,window,reinterpret_cast<HMENU>(INT_PTR(id)),nullptr,nullptr);
            SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);
            return child;
        };
        control(L"STATIC",L"PDF 設有密碼，請輸入開啟密碼：",0,margin,margin,width,22*unit,0);
        HWND edit = control(L"EDIT",L"",WS_BORDER|WS_TABSTOP|ES_PASSWORD|ES_AUTOHSCROLL,
                            margin,margin+28*unit,width,25*unit,100);
        SendMessageW(edit,EM_SETLIMITTEXT,4096,0);
        control(L"BUTTON",L"開啟",WS_TABSTOP|BS_DEFPUSHBUTTON,
                r.right-margin-176*unit,r.bottom-margin-28*unit,80*unit,28*unit,IDOK);
        control(L"BUTTON",L"取消",WS_TABSTOP,
                r.right-margin-80*unit,r.bottom-margin-28*unit,80*unit,28*unit,IDCANCEL);
        SetFocus(edit); return FALSE;
    }
    if (message == WM_COMMAND && LOWORD(w) == IDOK && state) {
        const int length = GetWindowTextLengthW(GetDlgItem(window,100));
        std::vector<wchar_t> value(size_t(length)+1);
        GetDlgItemTextW(window,100,value.data(),length+1);
        state->value.assign(value.data(),length);
        SecureZeroMemory(value.data(),value.size()*sizeof(wchar_t));
        state->accepted = true; EndDialog(window,IDOK); return TRUE;
    }
    if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(w) == IDCANCEL)) {
        EndDialog(window,IDCANCEL); return TRUE;
    }
    return FALSE;
}
static std::optional<std::wstring> askPassword() {
    std::vector<WORD> data;
    auto dword = [&](DWORD x){data.push_back(WORD(x));data.push_back(WORD(x>>16));};
    dword(WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME|DS_CENTER);
    dword(0); data.push_back(0);
    data.insert(data.end(),{0,0,270,90,0,0});
    const wchar_t* title = L"PDF 密碼";
    do { data.push_back(*title); } while (*title++);
    PasswordDialog state;
    DialogBoxIndirectParamW(GetModuleHandleW(nullptr),reinterpret_cast<DLGTEMPLATE*>(data.data()),
                           app.window,passwordProcedure,reinterpret_cast<LPARAM>(&state));
    if (state.accepted) return std::move(state.value);
    return std::nullopt;
}
static void zoomAt(double zoom, POINT point) {
    app.layout.zoomAt(zoom,{double(point.x),double(point.y)}); changed(true);
}
static void fitWidth() {
    app.layout.zoomAt(1,{app.layout.width/2,app.layout.height/2});
    app.layout.scrollX = 0; changed();
}
static void fullScreen() {
    app.fullScreen = !app.fullScreen;
    if (app.fullScreen) {
        app.placement.length = sizeof(app.placement); GetWindowPlacement(app.window,&app.placement);
        app.windowStyle = DWORD(GetWindowLongPtrW(app.window,GWL_STYLE));
        SetWindowLongPtrW(app.window,GWL_STYLE,app.windowStyle & ~WS_OVERLAPPEDWINDOW);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(app.window,MONITOR_DEFAULTTONEAREST),&monitor);
        SetWindowPos(app.window,HWND_TOP,monitor.rcMonitor.left,monitor.rcMonitor.top,
            monitor.rcMonitor.right-monitor.rcMonitor.left,monitor.rcMonitor.bottom-monitor.rcMonitor.top,
            SWP_NOOWNERZORDER|SWP_FRAMECHANGED);
    } else {
        SetWindowLongPtrW(app.window,GWL_STYLE,(app.windowStyle & ~WS_HSCROLL) |
                                               (app.horizontal ? WS_HSCROLL : 0));
        SetWindowPlacement(app.window,&app.placement);
        SetWindowPos(app.window,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED);
    }
}
static void trimCache() {
    size_t bytes = 0;
    for (const auto& item : app.cache) bytes += item.second.image->pixels.size();
    const auto visible = app.layout.visible();
    while (bytes > cacheBudget && app.cache.size() > 1) {
        auto victim = app.cache.end();
        for (auto i = app.cache.begin(); i != app.cache.end(); ++i) {
            if (std::find(visible.begin(),visible.end(),i->first) != visible.end()) continue;
            if (victim == app.cache.end() || i->second.touched < victim->second.touched) victim = i;
        }
        if (victim == app.cache.end()) break;
        bytes -= victim->second.image->pixels.size(); app.cache.erase(victim);
    }
}
static void paint(HWND window) {
    PAINTSTRUCT ps{}; HDC dc = BeginPaint(window,&ps);
    RECT client{}; GetClientRect(window,&client);
    HDC buffer = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc,std::max(1L,client.right),std::max(1L,client.bottom));
    HGDIOBJ old = SelectObject(buffer,bitmap);
    HBRUSH background = CreateSolidBrush(RGB(222,224,228));
    FillRect(buffer,&client,background); DeleteObject(background);
    SetStretchBltMode(buffer,HALFTONE); SetBrushOrgEx(buffer,0,0,nullptr);
    for (size_t i : app.layout.visible()) {
        const auto rect = app.layout.rect(i);
        RECT bounds{LONG(std::floor(rect.x)),LONG(std::floor(rect.y)),
                    LONG(std::ceil(rect.x+rect.width)),LONG(std::ceil(rect.y+rect.height))};
        FillRect(buffer,&bounds,static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        auto cached = app.cache.find(i);
        if (cached != app.cache.end()) {
            cached->second.touched = GetTickCount64(); const auto& image = *cached->second.image;
            BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = LONG(image.width); info.bmiHeader.biHeight = -LONG(image.height);
            info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
            StretchDIBits(buffer,bounds.left,bounds.top,bounds.right-bounds.left,bounds.bottom-bounds.top,
                         0,0,image.width,image.height,image.pixels.data(),&info,DIB_RGB_COLORS,SRCCOPY);
        } else if (app.failedRenders.count(i)) {
            SetTextColor(buffer,RGB(100,100,100)); SetBkMode(buffer,TRANSPARENT);
            DrawTextW(buffer,L"這一頁暫時無法顯示。\n按 Ctrl+0 重試。",-1,&bounds,DT_CENTER|DT_VCENTER|DT_WORDBREAK);
        }
    }
    if (app.layout.pages.empty()) {
        SetBkMode(buffer,TRANSPARENT); SetTextColor(buffer,RGB(70,73,79));
        HFONT font = CreateFontW(-MulDiv(17,int(GetDpiForWindow(window)),96),0,0,0,FW_NORMAL,
            FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Microsoft JhengHei UI");
        HGDIOBJ previous = SelectObject(buffer,font);
        RECT text = client; text.top = std::max(0L,(client.bottom-100)/2);
        DrawTextW(buffer,app.message.c_str(),-1,&text,DT_CENTER|DT_WORDBREAK);
        SelectObject(buffer,previous); DeleteObject(font);
    }
    BitBlt(dc,0,0,client.right,client.bottom,buffer,0,0,SRCCOPY);
    SelectObject(buffer,old); DeleteObject(bitmap); DeleteDC(buffer); EndPaint(window,&ps);
}

static LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM w, LPARAM l) {
    switch (message) {
    case WM_CREATE: {
        app.window = window; app.engine = std::make_unique<Engine>(window);
        DragAcceptFiles(window,TRUE);
        GESTURECONFIG gestures[] = {
            {GID_ZOOM,GC_ZOOM,0},
            {GID_PAN,GC_PAN|GC_PAN_WITH_SINGLE_FINGER_VERTICALLY|
                     GC_PAN_WITH_SINGLE_FINGER_HORIZONTALLY|GC_PAN_WITH_INERTIA,GC_PAN_WITH_GUTTER}
        };
        SetGestureConfig(window,0,2,gestures,sizeof(GESTURECONFIG));
        BOOL dark = FALSE; DwmSetWindowAttribute(window,20,&dark,sizeof(dark));
        ShowScrollBar(window,SB_HORZ,FALSE);
        PostMessageW(window,startupMessage,0,0); return 0;
    }
    case startupMessage:
        if (app.initialPath.empty()) chooseFile(); else openPath(app.initialPath);
        return 0;
    case WM_SIZE:
        if (w != SIZE_MINIMIZED && !app.updatingBars) {
            RECT client{}; GetClientRect(window,&client);
            app.layout.gap = std::max(4.0,6.0*GetDpiForWindow(window)/96);
            app.layout.resize(client.right,client.bottom); changed(true);
        }
        return 0;
    case WM_DPICHANGED: {
        RECT* r = reinterpret_cast<RECT*>(l);
        SetWindowPos(window,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);
        return 0;
    }
    case WM_PAINT: paint(window); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_CONTEXTMENU: return 0;
    case WM_TIMER:
        if (w == renderTimer) { KillTimer(window,renderTimer); queueRendering(); }
        return 0;
    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(w);
        const UINT length = DragQueryFileW(drop,0,nullptr,0);
        std::vector<wchar_t> path(size_t(length)+1);
        if (length) { DragQueryFileW(drop,0,path.data(),length+1); openPath(path.data()); }
        DragFinish(drop); return 0;
    }
    case WM_KEYDOWN: {
        const bool ctrl = (GetKeyState(VK_CONTROL)&0x8000) != 0;
        if (ctrl && w == 'O') chooseFile();
        else if (ctrl && (w == '0' || w == VK_NUMPAD0)) { app.failedRenders.clear(); fitWidth(); }
        else if (ctrl && (w == VK_ADD || w == VK_OEM_PLUS || w == VK_SUBTRACT || w == VK_OEM_MINUS)) {
            POINT point{LONG(app.layout.width/2),LONG(app.layout.height/2)};
            zoomAt(app.layout.zoom * ((w == VK_ADD || w == VK_OEM_PLUS) ? 1.15 : 1/1.15),point);
        } else if (w == VK_F11) fullScreen();
        else if (w == VK_ESCAPE && app.fullScreen) fullScreen();
        else if (w == VK_UP || w == VK_DOWN) {
            app.layout.pan(0,(w == VK_UP ? -1 : 1)*50*GetDpiForWindow(window)/96.0); changed();
        } else if (w == VK_LEFT || w == VK_RIGHT) {
            app.layout.pan(w == VK_LEFT ? -60 : 60,0); changed();
        } else if (w == VK_PRIOR || w == VK_NEXT || w == VK_SPACE) {
            const double direction = w == VK_PRIOR || (w == VK_SPACE && (GetKeyState(VK_SHIFT)&0x8000)) ? -1 : 1;
            app.layout.pan(0,direction*app.layout.height*.9); changed();
        } else if (w == VK_HOME) { app.layout.scrollY = 0; changed(); }
        else if (w == VK_END) { app.layout.scrollY = app.layout.maxY(); changed(); }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        const double delta = GET_WHEEL_DELTA_WPARAM(w);
        POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)}; ScreenToClient(window,&point);
        if (GET_KEYSTATE_WPARAM(w)&MK_CONTROL) zoomAt(app.layout.zoom*std::exp(delta*.0015),point);
        else {
            UINT lines = 3; SystemParametersInfoW(SPI_GETWHEELSCROLLLINES,0,&lines,0);
            double amount = lines == WHEEL_PAGESCROLL ? app.layout.height*.9 : lines*24.0*GetDpiForWindow(window)/96;
            if (GET_KEYSTATE_WPARAM(w)&MK_SHIFT) app.layout.pan(-delta/120*amount,0);
            else app.layout.pan(0,-delta/120*amount);
            changed();
        }
        return 0;
    }
    case WM_MOUSEHWHEEL:
        app.layout.pan(GET_WHEEL_DELTA_WPARAM(w)/120.0*72*GetDpiForWindow(window)/96,0); changed(); return 0;
    case WM_GESTURE: {
        GESTUREINFO gesture{sizeof(gesture)};
        if (!GetGestureInfo(reinterpret_cast<HGESTUREINFO>(l),&gesture)) break;
        if (gesture.dwID == GID_BEGIN) {
            app.dragging = false;
            if (GetCapture() == window) ReleaseCapture();
            return DefWindowProcW(window,message,w,l);
        }
        if (gesture.dwID == GID_END)
            return DefWindowProcW(window,message,w,l);
        POINT point{gesture.ptsLocation.x,gesture.ptsLocation.y}; ScreenToClient(window,&point);
        if (gesture.dwID == GID_ZOOM) {
            if ((gesture.dwFlags&GF_BEGIN) || app.gestureDistance == 0) app.gestureDistance = gesture.ullArguments;
            else if (gesture.ullArguments > 0) {
                const auto anchor = app.layout.capture({double(app.lastPinch.x),double(app.lastPinch.y)});
                app.layout.zoom = std::clamp(app.layout.zoom*double(gesture.ullArguments)/double(app.gestureDistance),
                                             reader::Layout::minZoom,reader::Layout::maxZoom);
                app.layout.reflow(); app.layout.restore(anchor,{double(point.x),double(point.y)});
                app.gestureDistance = gesture.ullArguments; changed(true);
            }
            app.lastPinch = point;
            if (gesture.dwFlags&GF_END) { app.gestureDistance = 0; changed(); }
        } else if (gesture.dwID == GID_PAN) {
            if (!(gesture.dwFlags&GF_BEGIN)) {
                app.layout.pan(app.lastPan.x-point.x,app.lastPan.y-point.y); changed();
            }
            app.lastPan = point;
        }
        CloseGestureInfoHandle(reinterpret_cast<HGESTUREINFO>(l)); return 0;
    }
    case WM_LBUTTONDOWN:
        app.dragging = true; app.lastMouse = {GET_X_LPARAM(l),GET_Y_LPARAM(l)};
        SetCapture(window); return 0;
    case WM_MOUSEMOVE:
        if (app.dragging) {
            POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};
            app.layout.pan(app.lastMouse.x-point.x,app.lastMouse.y-point.y);
            app.lastMouse = point; changed();
        }
        return 0;
    case WM_LBUTTONUP: app.dragging = false; ReleaseCapture(); return 0;
    case WM_CAPTURECHANGED: app.dragging = false; return 0;
    case WM_CANCELMODE:
        app.dragging = false;
        if (GetCapture() == window) ReleaseCapture();
        return 0;
    case WM_VSCROLL: case WM_HSCROLL: {
        const bool vertical = message == WM_VSCROLL;
        double& offset = vertical ? app.layout.scrollY : app.layout.scrollX;
        const double extent = vertical ? app.layout.totalHeight : app.layout.pageWidth();
        const double view = vertical ? app.layout.height : app.layout.width;
        const double factor = extent > 1000000000.0 ? 1000000000.0/extent : 1;
        SCROLLINFO info{sizeof(info),SIF_TRACKPOS}; GetScrollInfo(window,vertical ? SB_VERT : SB_HORZ,&info);
        switch (LOWORD(w)) {
        case SB_LINEUP: offset -= 50; break;
        case SB_LINEDOWN: offset += 50; break;
        case SB_PAGEUP: offset -= view*.9; break;
        case SB_PAGEDOWN: offset += view*.9; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: offset = info.nTrackPos/factor; break;
        case SB_TOP: offset = 0; break;
        case SB_BOTTOM: offset = std::max(0.0,extent-view); break;
        }
        app.layout.clamp(); changed(); return 0;
    }
    case resultMessage: {
        std::unique_ptr<Result> result(reinterpret_cast<Result*>(l));
        if (result->token != app.token) return 0;
        if (result->kind == ResultKind::Loaded) {
            app.loading = false; app.layout.pages = std::move(result->sizes); app.layout.reflow();
            app.layout.scrollX = app.layout.scrollY = 0; changed();
            if (app.diagnosticFile) app.diagnosticFile << "load: PASS; pages=" << app.layout.pages.size() << "\n" << std::flush;
        } else if (result->kind == ResultKind::Rendered) {
            app.cache[result->page] = {std::move(result->image),result->requestedWidth,GetTickCount64()};
            trimCache(); InvalidateRect(window,nullptr,FALSE);
            if (app.diagnosticFile) app.diagnosticFile << "render: PASS; page=" << result->page+1 << "; width=" << result->requestedWidth << "\n" << std::flush;
            if (app.diagnostic) PostMessageW(window,WM_CLOSE,0,0);
        } else if (result->kind == ResultKind::RenderFailed) {
            if (app.diagnostic) app.exitCode = 1;
            app.failedRenders[result->page] = result->requestedWidth;
            InvalidateRect(window,nullptr,FALSE);
            if (app.diagnosticFile) app.diagnosticFile << "render: FAIL; HRESULT=" << std::hex << result->error << "\n" << std::flush;
            if (app.diagnostic) PostMessageW(window,WM_CLOSE,0,0);
        } else {
            if (app.diagnostic) app.exitCode = 1;
            app.loading = false; app.message = errorText(result->error); changed();
            if (app.diagnosticFile) app.diagnosticFile << "load: FAIL; HRESULT=" << std::hex << result->error << "\n" << std::flush;
            if (app.diagnostic) { PostMessageW(window,WM_CLOSE,0,0); return 0; }
            if (result->error == HRESULT_FROM_WIN32(ERROR_WRONG_PASSWORD) ||
                result->error == HRESULT_FROM_WIN32(ERROR_INVALID_PASSWORD)) {
                auto password = askPassword();
                if (password) openPath(app.path,std::move(*password));
            }
        }
        return 0;
    }
    case WM_DESTROY:
        KillTimer(window,renderTimer); app.engine->stop();
        { MSG pending{}; while (PeekMessageW(&pending,window,resultMessage,resultMessage,PM_REMOVE))
            delete reinterpret_cast<Result*>(pending.lParam); }
        PostQuitMessage(app.exitCode); return 0;
    }
    return DefWindowProcW(window,message,w,l);
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT apartment = CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    int count = 0;
    LPWSTR* args = CommandLineToArgvW(GetCommandLineW(),&count);
    if (args) {
        for (int i=1;i<count;++i) {
            if (std::wstring(args[i]) == L"--diagnostic") app.diagnostic = true;
            else app.initialPath = args[i];
        }
        LocalFree(args);
    }
    if (app.diagnostic) {
        app.diagnosticFile.open("PDFReader-diagnostic.txt",std::ios::trunc);
        app.diagnosticFile << "Minimal PDF Reader 1.0 / Windows native PDF engine\n" << std::flush;
    }
    WNDCLASSEXW cls{sizeof(cls)};
    cls.style = CS_DBLCLKS; cls.lpfnWndProc = windowProcedure; cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr,IDC_ARROW); cls.lpszClassName = L"MinimalPDFReaderWindow";
    cls.hIcon = LoadIconW(instance,MAKEINTRESOURCEW(101));
    cls.hIconSm = static_cast<HICON>(LoadImageW(instance,MAKEINTRESOURCEW(101),IMAGE_ICON,
                                             GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON),LR_SHARED));
    if (!RegisterClassExW(&cls)) return 1;
    HWND window = CreateWindowExW(0,cls.lpszClassName,L"極簡 PDF 閱讀器",
        WS_OVERLAPPEDWINDOW|WS_VSCROLL,CW_USEDEFAULT,CW_USEDEFAULT,1000,750,
        nullptr,nullptr,instance,nullptr);
    if (!window) return 1;
    ShowWindow(window,SW_SHOWMAXIMIZED); UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message,nullptr,0,0)>0) {
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    app.engine.reset();
    if (SUCCEEDED(apartment)) CoUninitialize();
    return int(message.wParam);
}
