#ifndef UNICODE
#define UNICODE
#endif
#define _UNICODE
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <dwmapi.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "payload_manifest.hpp"

namespace fs = std::filesystem;
constexpr wchar_t productName[] = L"極簡 PDF 閱讀器";
constexpr wchar_t registryKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\MinimalPDFReader-7C063285";
constexpr char installIdentity[] = "MinimalPDFReader:{7C063285-4FE0-4BA4-AAC8-918DF65D943A}";
constexpr UINT progressMessage = WM_APP+10, completionMessage = WM_APP+11;
constexpr int installButton = 100, closeButton = 101, desktopCheckbox = 102, launchCheckbox = 103;

struct Failure { DWORD code; std::wstring context; };
[[noreturn]] static void fail(const std::wstring& context,DWORD code=GetLastError()) {
    throw Failure{code ? code : ERROR_GEN_FAILURE,context};
}
static std::wstring systemError(DWORD code) {
    LPWSTR text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER|FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr,code,0,reinterpret_cast<LPWSTR>(&text),0,nullptr);
    std::wstring result = text ? text : L"操作未完成。";
    LocalFree(text); return result;
}
static fs::path folder(REFKNOWNFOLDERID id) {
    PWSTR value = nullptr;
    HRESULT hr = SHGetKnownFolderPath(id,0,nullptr,&value);
    if (FAILED(hr)) fail(L"無法取得使用者資料夾",DWORD(hr));
    fs::path result(value); CoTaskMemFree(value); return result;
}
static fs::path installRoot() { return folder(FOLDERID_LocalAppData)/L"Programs"/L"MinimalPDFReader"; }
static fs::path executablePath() {
    std::vector<wchar_t> value(32768);
    DWORD count=GetModuleFileNameW(nullptr,value.data(),DWORD(value.size()));
    if (!count || count==value.size()) fail(L"無法取得程式路徑");
    return fs::path(std::wstring(value.data(),count));
}
static bool samePath(const fs::path& a,const fs::path& b) {
    return _wcsicmp(a.lexically_normal().c_str(),b.lexically_normal().c_str())==0;
}
static bool fileExists(const fs::path& p) {
    const DWORD attributes=GetFileAttributesW(p.c_str());
    if (attributes!=INVALID_FILE_ATTRIBUTES) return true;
    const DWORD error=GetLastError();
    if (error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND) return false;
    fail(L"無法讀取："+p.wstring(),error);
}
static void rejectReparse(const fs::path& p) {
    DWORD attributes=GetFileAttributesW(p.c_str());
    if (attributes!=INVALID_FILE_ATTRIBUTES && (attributes&FILE_ATTRIBUTE_REPARSE_POINT))
        fail(L"安裝位置含有連結或重新導向，已停止操作："+p.wstring(),ERROR_ACCESS_DENIED);
}
static void makeDirectories(const fs::path& p) {
    std::error_code error; fs::create_directories(p,error);
    if (error) fail(L"無法建立資料夾："+p.wstring(),DWORD(error.value()));
}
static void moveFile(const fs::path& from,const fs::path& to) {
    makeDirectories(to.parent_path());
    if (!MoveFileExW(from.c_str(),to.c_str(),MOVEFILE_WRITE_THROUGH))
        fail(L"無法更新檔案，請先關閉 PDF 閱讀器："+to.wstring());
}
static fs::path temporaryDirectory(const fs::path& parent) {
    makeDirectories(parent);
    GUID id{}; if (FAILED(CoCreateGuid(&id))) fail(L"無法建立暫存識別碼");
    wchar_t text[64]; StringFromGUID2(id,text,64);
    fs::path result=parent/(std::wstring(L"MinimalPDFReader-")+text);
    if (!CreateDirectoryW(result.c_str(),nullptr)) fail(L"無法建立安裝暫存資料夾");
    return result;
}
static bool ownedInstallation(const fs::path& root) {
    rejectReparse(root); rejectReparse(root/L"install-id.txt");
    if (!fileExists(root/L"install-id.txt")) return false;
    std::ifstream input(root/L"install-id.txt",std::ios::binary);
    std::string firstLine; std::getline(input,firstLine);
    if (!firstLine.empty() && firstLine.back()=='\r') firstLine.pop_back();
    return firstLine==installIdentity;
}
static void validateRoot(const fs::path& root,bool allowEmpty) {
    rejectReparse(root.parent_path()); rejectReparse(root);
    if (!fileExists(root)) return;
    if (!ownedInstallation(root) && !(allowEmpty && fs::is_empty(root)))
        fail(L"此資料夾包含其他檔案，已停止操作以保留它們："+root.wstring(),ERROR_ALREADY_EXISTS);
    rejectReparse(root/L"licenses");
    for (const auto& file : payloadFiles) rejectReparse(root/file.name);
}
static void checkReaderClosed(const fs::path& root) {
    if (!fileExists(root/L"PDFReader.exe")) return;
    HANDLE handle=CreateFileW((root/L"PDFReader.exe").c_str(),GENERIC_READ|GENERIC_WRITE,0,
                              nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (handle==INVALID_HANDLE_VALUE)
        fail(L"請先關閉 PDF 閱讀器，再重試。程式檔案目前無法更新。");
    CloseHandle(handle);
}
static void writeBytes(const fs::path& destination,const void* bytes,DWORD length) {
    makeDirectories(destination.parent_path());
    HANDLE handle=CreateFileW(destination.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (handle==INVALID_HANDLE_VALUE) fail(L"無法寫入："+destination.wstring());
    DWORD total=0,error=ERROR_SUCCESS;
    while (total<length) {
        DWORD count=0;
        if (!WriteFile(handle,static_cast<const BYTE*>(bytes)+total,length-total,&count,nullptr) || count==0) {
            error=GetLastError(); if (!error) error=ERROR_WRITE_FAULT; break;
        }
        total+=count;
    }
    if (!error && !FlushFileBuffers(handle)) error=GetLastError();
    CloseHandle(handle);
    if (error) fail(L"無法完整寫入："+destination.wstring(),error);
}
#ifndef BUILD_UNINSTALLER
static void extract(const PayloadFile& file,const fs::path& stage) {
    HRSRC resource=FindResourceW(nullptr,MAKEINTRESOURCEW(file.resource),RT_RCDATA);
    if (!resource) fail(L"安裝包缺少程式檔案："+std::wstring(file.name));
    DWORD size=SizeofResource(nullptr,resource);
    HGLOBAL loaded=LoadResource(nullptr,resource);
    const void* bytes=LockResource(loaded);
    if (!bytes || size!=file.bytes) fail(L"安裝包中的檔案不完整："+std::wstring(file.name),ERROR_INVALID_DATA);
    writeBytes(stage/file.name,bytes,size);
}
#endif

template<class T> class Com {
    T* p=nullptr;
public:
    ~Com(){if(p)p->Release();}
    T* operator->()const{return p;}
    T** out(){return &p;}
    void** outVoid(){return reinterpret_cast<void**>(&p);}
};
struct Shortcut { fs::path path,target; std::wstring description; bool existed=false; fs::path backup; };
static bool linkPointsTo(const fs::path& link,const fs::path& target) {
    if (!fileExists(link)) return false;
    Com<IShellLinkW> shell; Com<IPersistFile> file;
    if (FAILED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_IShellLinkW,shell.outVoid())) ||
        FAILED(shell->QueryInterface(IID_IPersistFile,file.outVoid())) || FAILED(file->Load(link.c_str(),STGM_READ))) return false;
    wchar_t path[32768]{}; WIN32_FIND_DATAW data{};
    return SUCCEEDED(shell->GetPath(path,32768,&data,SLGP_RAWPATH)) && samePath(path,target);
}
static void createLink(const Shortcut& shortcut,const fs::path& root) {
    Com<IShellLinkW> shell; Com<IPersistFile> file;
    HRESULT hr=CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_IShellLinkW,shell.outVoid());
    if (FAILED(hr)) fail(L"無法建立捷徑",DWORD(hr));
    if (FAILED(hr=shell->SetPath(shortcut.target.c_str())) ||
        FAILED(hr=shell->SetWorkingDirectory(root.c_str())) ||
        FAILED(hr=shell->SetDescription(shortcut.description.c_str())) ||
        FAILED(hr=shell->SetIconLocation((root/L"PDFReader.ico").c_str(),0)) ||
        FAILED(hr=shell->QueryInterface(IID_IPersistFile,file.outVoid()))) fail(L"無法設定捷徑",DWORD(hr));
    makeDirectories(shortcut.path.parent_path());
    if (FAILED(hr=file->Save(shortcut.path.c_str(),TRUE))) fail(L"無法儲存捷徑："+shortcut.path.wstring(),DWORD(hr));
}
static std::vector<Shortcut> shortcutList(const fs::path& root,bool desktop) {
    const auto menu=folder(FOLDERID_Programs)/productName;
    std::vector<Shortcut> list={
        {menu/(std::wstring(productName)+L".lnk"),root/L"PDFReader.exe",productName},
        {menu/L"解除安裝.lnk",root/L"Uninstall.exe",L"解除安裝極簡 PDF 閱讀器"}
    };
    if (desktop) list.push_back({folder(FOLDERID_Desktop)/(std::wstring(productName)+L".lnk"),root/L"PDFReader.exe",productName});
    return list;
}
static constexpr const wchar_t* registryNames[]={L"DisplayName",L"DisplayVersion",L"Publisher",L"InstallLocation",
    L"DisplayIcon",L"UninstallString",L"NoModify",L"NoRepair",L"EstimatedSize"};
struct RegistryValue { const wchar_t* name; DWORD type=0; std::vector<BYTE> data; bool existed=false; };
struct RegistrySnapshot {
    bool existed=false;
    std::vector<RegistryValue> values;
    RegistrySnapshot() {
        HKEY key=nullptr;
        LONG error=RegOpenKeyExW(HKEY_CURRENT_USER,registryKey,0,KEY_QUERY_VALUE,&key);
        if (error==ERROR_FILE_NOT_FOUND) return;
        if (error!=ERROR_SUCCESS) fail(L"無法讀取安裝紀錄",error);
        existed=true;
        for (auto name : registryNames) {
            RegistryValue value{name}; DWORD count=0;
            if (RegQueryValueExW(key,name,nullptr,&value.type,nullptr,&count)==ERROR_SUCCESS) {
                value.data.resize(count);
                if (RegQueryValueExW(key,name,nullptr,&value.type,value.data.data(),&count)==ERROR_SUCCESS) value.existed=true;
            }
            values.push_back(std::move(value));
        }
        RegCloseKey(key);
    }
    bool restore() const {
        if (!existed) {
            LONG result=RegDeleteTreeW(HKEY_CURRENT_USER,registryKey);
            return result==ERROR_SUCCESS || result==ERROR_FILE_NOT_FOUND;
        }
        HKEY key=nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER,registryKey,0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr)!=ERROR_SUCCESS) return false;
        bool ok=true;
        for (const auto& value : values) {
            LONG result=value.existed ? RegSetValueExW(key,value.name,0,value.type,value.data.data(),DWORD(value.data.size()))
                                     : RegDeleteValueW(key,value.name);
            if (result!=ERROR_SUCCESS && result!=ERROR_FILE_NOT_FOUND) ok=false;
        }
        RegCloseKey(key); return ok;
    }
};
static void registerUninstaller(const fs::path& root) {
    HKEY key=nullptr;
    LONG error=RegCreateKeyExW(HKEY_CURRENT_USER,registryKey,0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr);
    if (error!=ERROR_SUCCESS) fail(L"無法寫入安裝紀錄",error);
    auto text=[&](const wchar_t* name,const std::wstring& value){
        LONG result=RegSetValueExW(key,name,0,REG_SZ,reinterpret_cast<const BYTE*>(value.c_str()),DWORD((value.size()+1)*sizeof(wchar_t)));
        if (result!=ERROR_SUCCESS && error==ERROR_SUCCESS) error=result;
    };
    auto number=[&](const wchar_t* name,DWORD value){
        LONG result=RegSetValueExW(key,name,0,REG_DWORD,reinterpret_cast<const BYTE*>(&value),sizeof(value));
        if (result!=ERROR_SUCCESS && error==ERROR_SUCCESS) error=result;
    };
    text(L"DisplayName",productName); text(L"DisplayVersion",L"1.0.0");
    text(L"Publisher",L"Minimal PDF Reader"); text(L"InstallLocation",root.wstring());
    text(L"DisplayIcon",L"\""+(root/L"PDFReader.exe").wstring()+L"\",0");
    text(L"UninstallString",L"\""+(root/L"Uninstall.exe").wstring()+L"\"");
    number(L"NoModify",1); number(L"NoRepair",1);
    DWORD bytes=0; for (const auto& p : payloadFiles) bytes+=p.bytes;
    number(L"EstimatedSize",(bytes+1023)/1024);
    RegCloseKey(key);
    if (error!=ERROR_SUCCESS) fail(L"無法建立解除安裝紀錄",error);
}

struct Completion { bool ok=false; std::wstring message; };
static HWND window=nullptr,progress=nullptr,statusText=nullptr,action=nullptr,cancel=nullptr,desktop=nullptr,launch=nullptr;
static std::thread worker;
static bool busy=false,completed=false,successful=false;
static fs::path rootPath;
static HFONT font=nullptr,titleFont=nullptr;

static void report(unsigned position,const std::wstring& text) {
    auto* value=new std::wstring(text);
    if (!PostMessageW(window,progressMessage,position,reinterpret_cast<LPARAM>(value))) delete value;
}
#ifndef BUILD_UNINSTALLER
static Completion install(bool wantDesktop) {
    const auto root=installRoot(); validateRoot(root,true); checkReaderClosed(root);
    const auto stage=temporaryDirectory(root.parent_path());
    const auto fresh=stage/L"new",backup=stage/L"backup";
    std::vector<const PayloadFile*> installed,backedUp;
    std::vector<Shortcut> links;
    std::unique_ptr<RegistrySnapshot> registry;
    bool registryTouched=false,linksTouched=false;
    const bool rootExisted=fileExists(root);
    try {
        unsigned position=0;
        for (const auto& file : payloadFiles) {
            report(++position,L"正在準備程式檔案…"); extract(file,fresh);
        }
        registry=std::make_unique<RegistrySnapshot>();
        links=shortcutList(root,wantDesktop);
        for (size_t i=0;i<links.size();++i) {
            auto& link=links[i]; link.existed=fileExists(link.path);
            if (link.existed) {
                if (!linkPointsTo(link.path,link.target))
                    fail(L"同名捷徑已存在，已保留現有捷徑："+link.path.wstring(),ERROR_ALREADY_EXISTS);
                link.backup=stage/L"old-links"/(std::to_wstring(i)+L".lnk");
                makeDirectories(link.backup.parent_path());
                if (!CopyFileW(link.path.c_str(),link.backup.c_str(),TRUE)) fail(L"無法備份原有捷徑");
            }
        }
        makeDirectories(root);
        for (const auto& file : payloadFiles) {
            const auto destination=root/file.name;
            if (fileExists(destination)) {
                moveFile(destination,backup/file.name); backedUp.push_back(&file);
            }
            moveFile(fresh/file.name,destination); installed.push_back(&file);
        }
        report(++position,L"正在建立開始功能表與桌面捷徑…");
        linksTouched=true;
        for (const auto& link : links) createLink(link,root);
        report(++position,L"正在建立解除安裝紀錄…");
        registryTouched=true; registerUninstaller(root);
        std::error_code ignored; fs::remove_all(stage,ignored);
        SHChangeNotify(SHCNE_ASSOCCHANGED,SHCNF_IDLIST,nullptr,nullptr);
        return {true,L"安裝完成。可以從開始功能表或桌面捷徑開啟。"};
    } catch (...) {
        bool restored=true;
        if (registryTouched && registry && !registry->restore()) restored=false;
        if (linksTouched) {
            for (const auto& link : links) {
                if (link.existed) {
                    if (!CopyFileW(link.backup.c_str(),link.path.c_str(),FALSE)) restored=false;
                } else if (fileExists(link.path) && linkPointsTo(link.path,link.target)) {
                    if (!DeleteFileW(link.path.c_str())) restored=false;
                }
            }
        }
        for (auto i=installed.rbegin();i!=installed.rend();++i)
            if (!DeleteFileW((root/(*i)->name).c_str())) restored=false;
        for (auto i=backedUp.rbegin();i!=backedUp.rend();++i)
            if (!MoveFileExW((backup/(*i)->name).c_str(),(root/(*i)->name).c_str(),MOVEFILE_WRITE_THROUGH)) restored=false;
        if (!rootExisted) { RemoveDirectoryW((root/L"licenses").c_str()); RemoveDirectoryW(root.c_str()); }
        if (restored) { std::error_code ignored; fs::remove_all(stage,ignored); }
        else fail(L"安裝未完成，備份保留在："+stage.wstring(),ERROR_WRITE_FAULT);
        throw;
    }
}
#else
static Completion uninstall() {
    const auto root=installRoot(); validateRoot(root,false);
    if (!ownedInstallation(root)) fail(L"找不到此程式的安裝紀錄，沒有移除任何檔案。",ERROR_FILE_NOT_FOUND);
    checkReaderClosed(root);
    // Delete only names installed by this product; unrelated PDFs stay in place.
    if (fileExists(root/L"PDFReader.exe") && !DeleteFileW((root/L"PDFReader.exe").c_str()))
        fail(L"請先關閉 PDF 閱讀器，再解除安裝。");
    for (const auto& file : payloadFiles) {
        if (std::wstring(file.name)==L"PDFReader.exe" || std::wstring(file.name)==L"Uninstall.exe" ||
            std::wstring(file.name)==L"install-id.txt") continue;
        const auto path=root/file.name;
        if (fileExists(path) && !DeleteFileW(path.c_str())) fail(L"無法移除程式檔案："+path.wstring());
    }
    for (const auto& link : shortcutList(root,true))
        if (linkPointsTo(link.path,link.target) && !DeleteFileW(link.path.c_str()))
            fail(L"無法移除程式捷徑："+link.path.wstring());
    LONG error=RegDeleteTreeW(HKEY_CURRENT_USER,registryKey);
    if (error!=ERROR_SUCCESS && error!=ERROR_FILE_NOT_FOUND) fail(L"無法移除安裝紀錄",error);
    if (fileExists(root/L"Uninstall.exe") && !DeleteFileW((root/L"Uninstall.exe").c_str()))
        fail(L"無法移除解除安裝程式，請在此視窗重試。");
    if (!DeleteFileW((root/L"install-id.txt").c_str())) fail(L"無法移除安裝識別檔案");
    RemoveDirectoryW((root/L"licenses").c_str());
    RemoveDirectoryW((folder(FOLDERID_Programs)/productName).c_str());
    const bool removed=RemoveDirectoryW(root.c_str())!=0;
    SHChangeNotify(SHCNE_ASSOCCHANGED,SHCNF_IDLIST,nullptr,nullptr);
    return {true,removed ? L"解除安裝完成。您的 PDF 文件已保留。"
                        : L"程式已移除。資料夾中的其他檔案已保留。"};
}
static bool relaunchForUninstall() {
    const auto current=executablePath();
    const auto root=installRoot();
    if (!samePath(current.parent_path(),root)) return false;
    validateRoot(root,false);
    wchar_t temp[32768]{};
    if (!GetTempPathW(32768,temp)) fail(L"無法取得臨時資料夾");
    const auto folder=temporaryDirectory(temp),copy=folder/L"Uninstall.exe";
    if (!CopyFileW(current.c_str(),copy.c_str(),TRUE)) fail(L"無法啟動解除安裝程式");
    std::wstring command=L"\""+copy.wstring()+L"\" --remove";
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
    if (!CreateProcessW(copy.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,folder.c_str(),&startup,&process)) {
        const DWORD error=GetLastError(); DeleteFileW(copy.c_str()); RemoveDirectoryW(folder.c_str());
        fail(L"無法啟動解除安裝程式",error);
    }
    CloseHandle(process.hThread); CloseHandle(process.hProcess); return true;
}
#endif

static void startOperation() {
    if (busy) return;
    busy=true; EnableWindow(action,FALSE); EnableWindow(cancel,FALSE);
    if (desktop) EnableWindow(desktop,FALSE);
    if (launch) EnableWindow(launch,FALSE);
    ShowWindow(progress,SW_SHOW);
    const bool wantDesktop=desktop && SendMessageW(desktop,BM_GETCHECK,0,0)==BST_CHECKED;
    worker=std::thread([wantDesktop]{
        auto result=std::make_unique<Completion>();
        const HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        try {
            if (FAILED(com)) fail(L"無法初始化 Windows 安裝功能",DWORD(com));
#ifndef BUILD_UNINSTALLER
            *result=install(wantDesktop);
#else
            report(1,L"正在移除程式與捷徑…"); *result=uninstall();
#endif
        } catch (const Failure& error) { result->message=error.context+L"\n"+systemError(error.code); }
        catch (const std::exception&) { result->message=L"操作未完成。請確認資料夾可寫入並關閉閱讀器後重試。"; }
        if (SUCCEEDED(com)) CoUninitialize();
        if (PostMessageW(window,completionMessage,0,reinterpret_cast<LPARAM>(result.get()))) result.release();
    });
}
static void launchReader() {
    const auto reader=rootPath/L"PDFReader.exe";
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(window,L"open",reader.c_str(),nullptr,rootPath.c_str(),SW_SHOWNORMAL))<=32)
        MessageBoxW(window,L"程式已安裝，請從桌面或開始功能表開啟。",productName,MB_OK);
}
static HWND control(const wchar_t* type,const wchar_t* text,DWORD style,int x,int y,int w,int h,int id=0) {
    const UINT dpi=GetDpiForWindow(window);
    HWND result=CreateWindowExW(0,type,text,WS_CHILD|WS_VISIBLE|style,
        MulDiv(x,dpi,96),MulDiv(y,dpi,96),MulDiv(w,dpi,96),MulDiv(h,dpi,96),
        window,reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr);
    SendMessageW(result,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE); return result;
}
static LRESULT CALLBACK procedure(HWND hwnd,UINT message,WPARAM w,LPARAM l) {
    if (message==WM_CREATE) {
        window=hwnd;
        const UINT dpi=GetDpiForWindow(window);
        font=CreateFontW(-MulDiv(15,dpi,96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
                         0,0,CLEARTYPE_QUALITY,0,L"Microsoft JhengHei UI");
        titleFont=CreateFontW(-MulDiv(24,dpi,96),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
                              0,0,CLEARTYPE_QUALITY,0,L"Microsoft JhengHei UI");
        HWND icon=control(L"STATIC",L"",SS_ICON,28,25,64,64);
        HICON image=static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(101),IMAGE_ICON,
                                               MulDiv(64,dpi,96),MulDiv(64,dpi,96),LR_SHARED));
        SendMessageW(icon,STM_SETICON,reinterpret_cast<WPARAM>(image),0);
        HWND title=control(L"STATIC",productName,0,114,27,450,37);
        SendMessageW(title,WM_SETFONT,reinterpret_cast<WPARAM>(titleFont),TRUE);
#ifndef BUILD_UNINSTALLER
        control(L"STATIC",L"Windows 11 · 版本 1.0 · 安裝版",0,115,68,430,24);
        control(L"STATIC",L"等比例填滿閱讀區寬度，支援觸控／觸控板縮放。\n閱讀介面沒有工具列或選單。",0,28,117,540,53);
        control(L"STATIC",L"安裝位置（目前使用者，無需管理員權限）：",0,28,181,550,23);
        control(L"STATIC",rootPath.c_str(),SS_PATHELLIPSIS,28,207,550,25);
        desktop=control(L"BUTTON",L"建立桌面捷徑",BS_AUTOCHECKBOX|WS_TABSTOP,28,249,250,26,desktopCheckbox);
        SendMessageW(desktop,BM_SETCHECK,BST_CHECKED,0);
        launch=control(L"BUTTON",L"安裝後開啟程式",BS_AUTOCHECKBOX|WS_TABSTOP,28,282,250,26,launchCheckbox);
        SendMessageW(launch,BM_SETCHECK,BST_CHECKED,0);
        action=control(L"BUTTON",L"安裝",BS_DEFPUSHBUTTON|WS_TABSTOP,357,375,105,34,installButton);
#else
        control(L"STATIC",L"解除安裝",0,115,68,430,24);
        control(L"STATIC",L"將移除程式、桌面捷徑與開始功能表項目。\n您閱讀的 PDF 文件不會被刪除。",0,28,117,540,53);
        control(L"STATIC",L"程式位置：",0,28,181,550,23);
        control(L"STATIC",rootPath.c_str(),SS_PATHELLIPSIS,28,207,550,25);
        action=control(L"BUTTON",L"解除安裝",BS_DEFPUSHBUTTON|WS_TABSTOP,357,375,105,34,installButton);
#endif
        cancel=control(L"BUTTON",L"取消",WS_TABSTOP,477,375,105,34,closeButton);
        statusText=control(L"STATIC",L"",0,28,315,550,45);
        progress=control(PROGRESS_CLASSW,L"",PBS_SMOOTH,28,361,550,7);
        SendMessageW(progress,PBM_SETRANGE32,0,int(std::size(payloadFiles)+2));
        ShowWindow(progress,SW_HIDE);
        return 0;
    }
    if (message==WM_CTLCOLORSTATIC || message==WM_CTLCOLORBTN) {
        HDC dc=reinterpret_cast<HDC>(w); SetBkColor(dc,RGB(255,255,255));
        SetTextColor(dc,RGB(30,30,30)); return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
    }
    if (message==WM_COMMAND) {
        if (LOWORD(w)==installButton) startOperation();
        else if (LOWORD(w)==closeButton && !busy) {
#ifndef BUILD_UNINSTALLER
            if (completed && successful && SendMessageW(launch,BM_GETCHECK,0,0)==BST_CHECKED) launchReader();
#endif
            DestroyWindow(hwnd);
        }
        return 0;
    }
    if (message==progressMessage) {
        std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(l));
        SetWindowTextW(statusText,text->c_str()); SendMessageW(progress,PBM_SETPOS,w,0); return 0;
    }
    if (message==completionMessage) {
        std::unique_ptr<Completion> result(reinterpret_cast<Completion*>(l));
        if (worker.joinable()) worker.join(); busy=false; completed=true; successful=result->ok;
        EnableWindow(cancel,TRUE); SetWindowTextW(cancel,result->ok ? L"完成" : L"取消");
        ShowWindow(action,result->ok ? SW_HIDE : SW_SHOW); ShowWindow(progress,SW_HIDE);
        if (!result->ok) { EnableWindow(action,TRUE); SetWindowTextW(action,L"重試"); }
        if (desktop) EnableWindow(desktop,!result->ok);
        if (launch) EnableWindow(launch,TRUE);
        SetWindowTextW(statusText,result->ok ? result->message.c_str() : L"操作未完成。請依提示修正後重試。");
        if (!result->ok) MessageBoxW(hwnd,result->message.c_str(),productName,MB_OK|MB_ICONERROR);
        return 0;
    }
    if (message==WM_CLOSE) { if (!busy) DestroyWindow(hwnd); return 0; }
    if (message==WM_DESTROY) {
        if (worker.joinable()) worker.join(); DeleteObject(font); DeleteObject(titleFont);
        PostQuitMessage(successful ? 0 : 1); return 0;
    }
    return DefWindowProcW(hwnd,message,w,l);
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    try {
        rootPath=installRoot();
#ifdef BUILD_UNINSTALLER
        if (relaunchForUninstall()) { if (SUCCEEDED(com)) CoUninitialize(); return 0; }
#endif
        INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_PROGRESS_CLASS}; InitCommonControlsEx(&controls);
        WNDCLASSEXW cls{sizeof(cls)}; cls.lpfnWndProc=procedure; cls.hInstance=instance;
        cls.hCursor=LoadCursorW(nullptr,IDC_ARROW); cls.hbrBackground=static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
        cls.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(101)); cls.hIconSm=cls.hIcon; cls.lpszClassName=L"MinimalPDFReaderSetup";
        if (!RegisterClassExW(&cls)) fail(L"無法啟動安裝介面");
        UINT dpi=GetDpiForSystem(); RECT r{0,0,MulDiv(610,dpi,96),MulDiv(435,dpi,96)};
        const DWORD style=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX;
        AdjustWindowRectExForDpi(&r,style,FALSE,0,dpi);
        const int width=r.right-r.left,height=r.bottom-r.top;
        HWND hwnd=CreateWindowExW(0,cls.lpszClassName,
#ifndef BUILD_UNINSTALLER
            L"極簡 PDF 閱讀器 — 安裝",
#else
            L"極簡 PDF 閱讀器 — 解除安裝",
#endif
            style,(GetSystemMetrics(SM_CXSCREEN)-width)/2,(GetSystemMetrics(SM_CYSCREEN)-height)/2,
            width,height,nullptr,nullptr,instance,nullptr);
        if (!hwnd) fail(L"無法建立安裝視窗");
        BOOL dark=FALSE; DwmSetWindowAttribute(hwnd,20,&dark,sizeof(dark));
        ShowWindow(hwnd,SW_SHOW); UpdateWindow(hwnd);
        MSG message{};
        while (GetMessageW(&message,nullptr,0,0)>0) {
            if (!IsDialogMessageW(hwnd,&message)) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        if (SUCCEEDED(com)) CoUninitialize(); return int(message.wParam);
    } catch (const Failure& error) { MessageBoxW(nullptr,(error.context+L"\n"+systemError(error.code)).c_str(),productName,MB_OK|MB_ICONERROR); }
    catch (...) { MessageBoxW(nullptr,L"無法啟動安裝程式。",productName,MB_OK|MB_ICONERROR); }
    if (SUCCEEDED(com)) CoUninitialize(); return 1;
}
