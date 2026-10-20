#include "common.h"
#include "diagnostic_paths.h"
#include "status_transport.h"
#include "ui_status_json.h"
#include "temporal_interval_trace.h"
#include <aclapi.h>
#include <fstream>
#include <string>
#pragma comment(lib, "advapi32.lib")
using namespace harness;
// Only the standalone trace-file test uses this stub. It submits no NGX work;
// the candidate DLL retains its unchanged authoritative GPU selection policy.
namespace gpu_dispatch { bool UsesTranslatedProgram() noexcept {return false;} }
static std::string ReadText(const std::filesystem::path& path) {
    std::ifstream file(path);return {std::istreambuf_iterator<char>(file),{}};
}
static bool Has(const std::string& text,const char* key,uint64_t value) {
    const auto token=std::string("\"")+key+"\":"+std::to_string(value);
    const auto at=text.find(token);
    return at!=std::string::npos && at+token.size()<text.size()
        && (text[at+token.size()]==','||text[at+token.size()]=='}');
}
static bool ReadFileText(const std::wstring& path,std::string& result){result=ReadText(path);return !result.empty();}
static std::filesystem::path StatusPath(){return std::filesystem::current_path()/L"RTXMFG-Universal.status.json";}
// Denies (or gives back) the current user's right to create files and folders
// in one folder, like a protected game install. Restore puts the old DACL back.
struct CreateDenied {
    std::wstring path;PSECURITY_DESCRIPTOR original=nullptr;PACL originalDacl=nullptr;
    bool Deny(const std::filesystem::path& directory) {
        path=directory.wstring();
        if(GetNamedSecurityInfoW(path.c_str(),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,&originalDacl,nullptr,&original)!=ERROR_SUCCESS)return false;
        HANDLE token=nullptr;
        if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return false;
        alignas(TOKEN_USER) BYTE buffer[512]{};DWORD bytes=0;
        const bool user=GetTokenInformation(token,TokenUser,buffer,sizeof(buffer),&bytes)!=FALSE;
        CloseHandle(token);
        if(!user)return false;
        EXPLICIT_ACCESSW deny{};
        deny.grfAccessPermissions=FILE_ADD_FILE|FILE_ADD_SUBDIRECTORY;deny.grfAccessMode=DENY_ACCESS;deny.grfInheritance=NO_INHERITANCE;
        deny.Trustee.TrusteeForm=TRUSTEE_IS_SID;deny.Trustee.TrusteeType=TRUSTEE_IS_USER;
        deny.Trustee.ptstrName=static_cast<LPWSTR>(reinterpret_cast<TOKEN_USER*>(buffer)->User.Sid);
        PACL dacl=nullptr;
        if(SetEntriesInAclW(1,&deny,originalDacl,&dacl)!=ERROR_SUCCESS)return false;
        const DWORD set=SetNamedSecurityInfoW(path.data(),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,dacl,nullptr);
        LocalFree(dacl);return set==ERROR_SUCCESS;
    }
    bool Restore() {
        if(!original)return true;
        const DWORD set=SetNamedSecurityInfoW(path.data(),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,originalDacl,nullptr);
        LocalFree(original);original=nullptr;return set==ERROR_SUCCESS;
    }
    ~CreateDenied(){Restore();}
};
static int Worker(const wchar_t* mode,const wchar_t* dll,const wchar_t* events) {
    if(!wcscmp(mode,L"worker-log")) {
        // Runs from a folder it cannot write: the candidate's log must land in %TEMP%.
        if(!Check(LoadCandidate(dll)!=nullptr,"log worker loads exact candidate"))return 1;
        const auto log=std::filesystem::path(diagnostic_paths::TempDirectory())/diagnostic_paths::FileName(diagnostic_paths::Executable());
        for(unsigned i=0;i<480&&ReadText(log).find("Patch worker started")==std::string::npos;++i)Sleep(25);
        Check(ReadText(log).find("Patch worker started")!=std::string::npos,"candidate writes its runtime log to %TEMP% when the game folder cannot be written");
        return gFailures?1:0;
    }
    if(!wcscmp(mode,L"worker-trace")) {
        const auto directory=std::filesystem::path(diagnostic_paths::RuntimeLog()).parent_path();
        temporal_interval_trace::Initialize(directory.c_str(),diagnostic_paths::Executable().c_str());
        temporal_interval_trace::SetEnabled(true);temporal_interval_trace::Flush();
        Check(temporal_interval_trace::ReadSnapshot().logReady,"production trace writer opens stable file");
        return gFailures?1:0;
    }
    if(!Check(LoadCandidate(dll)!=nullptr,"retention worker loads exact candidate"))return 1;
    std::string current;
    auto valid=[](const std::string& s){return ui_status_json::CompleteObject(s)
        &&Has(s,"pid",GetCurrentProcessId())&&Has(s,"processBirth",diagnostic_paths::ProcessBirth());};
    status_transport::Source source=status_transport::Source::None;
    for(unsigned i=0;i<320;++i) {
        source=status_transport::Read(StatusPath().wstring(),current,&ReadFileText,valid);
        if(source!=status_transport::Source::None)break;
        Sleep(25);
    }
    if(!Check(source!=status_transport::Source::None&&valid(current),"worker receives only its own current-lifetime snapshot"))return 1;
    if(!wcscmp(mode,L"worker-rival"))Check(source==status_transport::Source::Fallback,"overlapping launch uses process memory without stealing primary file");
    if(!wcscmp(mode,L"worker-hold")) {
        HANDLE ready=OpenEventW(EVENT_MODIFY_STATE,FALSE,(std::wstring(events)+L"-Ready").c_str());
        HANDLE release=OpenEventW(SYNCHRONIZE,FALSE,(std::wstring(events)+L"-Release").c_str());
        if(!ready||!release)return 1;
        SetEvent(ready);
        Check(WaitForSingleObject(release,30000)==WAIT_OBJECT_0,"owner released by test coordinator");
        CloseHandle(ready);CloseHandle(release);
    }
    return gFailures?1:0;
}
struct Child {
    PROCESS_INFORMATION process{};
    bool Start(const wchar_t* mode,const wchar_t* dll,const std::wstring& events,const std::wstring& executable=diagnostic_paths::Executable()) {
        auto command=L"\""+executable+L"\" "+mode+L" \""+dll+L"\" \""+events+L"\"";
        STARTUPINFOW startup{};startup.cb=sizeof(startup);
        startup.dwFlags=STARTF_USESTDHANDLES;
        startup.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE);startup.hStdError=GetStdHandle(STD_ERROR_HANDLE);startup.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
        return CreateProcessW(nullptr,command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)!=FALSE;
    }
    bool Finish() {
        if(!process.hProcess||WaitForSingleObject(process.hProcess,12000)!=WAIT_OBJECT_0)return false;
        DWORD code=1;GetExitCodeProcess(process.hProcess,&code);return code==0;
    }
    ~Child(){if(process.hProcess){if(WaitForSingleObject(process.hProcess,0)==WAIT_TIMEOUT)TerminateProcess(process.hProcess,2);CloseHandle(process.hProcess);}if(process.hThread)CloseHandle(process.hThread);}
};
int wmain(int argc,wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    if(argc==4)return Worker(argv[1],argv[2],argv[3]);
    if(argc!=3)return 2;
    const auto log=diagnostic_paths::RuntimeLog();
    const auto csv=std::filesystem::path(log).parent_path()/diagnostic_paths::FileName(diagnostic_paths::Executable(),L"-intervals.csv");
    if(!wcscmp(argv[1],L"interval-retention")) {
        std::ofstream(csv)<<"PREVIOUS-TRACE-MARKER";
        Child first;Check(first.Start(L"worker-trace",argv[2],L"unused")&&first.Finish(),"first trace process exits cleanly");
        Check(ReadText(csv).find("PREVIOUS-TRACE-MARKER")==std::string::npos,"first trace launch truncates previous file");
        std::ofstream(csv,std::ios::app)<<"RESTART-TRACE-MARKER";
        Child next;Check(next.Start(L"worker-trace",argv[2],L"unused")&&next.Finish(),"second trace process exits cleanly");
        Check(ReadText(csv).find("RESTART-TRACE-MARKER")==std::string::npos,"next trace launch overwrites same file");
        Check(ReadText(csv).starts_with("# NGX temporal-request trace."),"trace file retains production header");
        return gFailures?1:0;
    }
    if(!wcscmp(argv[1],L"log-location")) {
        const auto executable=std::filesystem::path(diagnostic_paths::Executable());
        Check(std::filesystem::path(log).parent_path()==executable.parent_path(),"runtime log is in the game folder beside the executable");
        const auto game=std::filesystem::current_path()/L"protected-game";
        const auto temp=std::filesystem::current_path()/L"fallback-temp";
        std::filesystem::create_directories(game);std::filesystem::create_directories(temp);
        const auto copy=game/executable.filename();
        std::filesystem::copy_file(executable,copy,std::filesystem::copy_options::overwrite_existing);
        auto entries=[&]{size_t n=0;for(const auto& e:std::filesystem::directory_iterator(game)){(void)e;++n;}return n;};
        Check(diagnostic_paths::CanCreateFiles(game.wstring())&&entries()==1,"write probe passes in a writable folder and leaves nothing behind");
        CreateDenied denied;
        if(!Check(denied.Deny(game),"game folder denies file creation"))return 1;
        Check(!diagnostic_paths::CanCreateFiles(game.wstring()),"write probe fails in a folder that denies file creation");
        // The child inherits TEMP/TMP; put them back for this process afterwards.
        wchar_t savedTemp[MAX_PATH]{},savedTmp[MAX_PATH]{};
        GetEnvironmentVariableW(L"TEMP",savedTemp,MAX_PATH);GetEnvironmentVariableW(L"TMP",savedTmp,MAX_PATH);
        SetEnvironmentVariableW(L"TEMP",temp.c_str());SetEnvironmentVariableW(L"TMP",temp.c_str());
        Child child;
        const bool started=child.Start(L"worker-log",argv[2],L"unused",copy.wstring());
        SetEnvironmentVariableW(L"TEMP",savedTemp[0]?savedTemp:nullptr);SetEnvironmentVariableW(L"TMP",savedTmp[0]?savedTmp:nullptr);
        Check(started&&child.Finish(),"launch from a protected game folder exits cleanly");
        const auto name=diagnostic_paths::FileName(copy.wstring());
        Check(ReadText(temp/name).find("Patch worker started")!=std::string::npos,"protected game folder: runtime log falls back to %TEMP%");
        Check(!std::filesystem::exists(game/name)&&entries()==1,"nothing is written in the protected game folder");
        Check(denied.Restore(),"game folder permissions restored");
        return gFailures?1:0;
    }
    Check(std::filesystem::path(log).parent_path()==std::filesystem::path(diagnostic_paths::Executable()).parent_path(),"runtime log is in the game folder beside the executable");
    Check(diagnostic_paths::FileName(L"C:\\one\\game.exe")!=diagnostic_paths::FileName(L"D:\\two\\game.exe"),"same executable names in different games have different logs");
    Check(diagnostic_paths::PathHash(L"C:\\GAME\\game.exe")==diagnostic_paths::PathHash(L"c:/game/game.exe"),"path identity is insensitive to ASCII case and slash spelling");
    std::ofstream(std::filesystem::path(log))<<"PREVIOUS-LAUNCH-MARKER";
    std::ofstream(StatusPath())<<"{\"previous-launch\":true}";
    const auto events=L"Local\\RTXMFG-RetentionTest-"+std::to_wstring(GetCurrentProcessId());
    HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,(events+L"-Ready").c_str());
    HANDLE release=CreateEventW(nullptr,TRUE,FALSE,(events+L"-Release").c_str());
    Child owner;
    if(!Check(owner.Start(L"worker-hold",argv[2],events),"first launch starts"))return 1;
    if(!Check(WaitForSingleObject(ready,12000)==WAIT_OBJECT_0,"first launch publishes stable status"))return 1;
    Check(Has(ReadText(StatusPath()),"pid",owner.process.dwProcessId),"stable status belongs to first live launch");
    Child rival;
    Check(rival.Start(L"worker-rival",argv[2],events)&&rival.Finish(),"overlapping launch remains isolated and exits cleanly");
    Check(Has(ReadText(StatusPath()),"pid",owner.process.dwProcessId),"overlapping launch preserves owner's primary snapshot");
    SetEvent(release);Check(owner.Finish(),"first launch exits cleanly");
    CloseHandle(ready);CloseHandle(release);
    Check(ReadText(log).find("PREVIOUS-LAUNCH-MARKER")==std::string::npos,"first launch truncates previous runtime log");
    std::ofstream(std::filesystem::path(log),std::ios::app)<<"RESTART-MARKER";
    Child next;
    Check(next.Start(L"worker",argv[2],events)&&next.Finish(),"next launch reacquires abandoned status ownership");
    Check(Has(ReadText(StatusPath()),"pid",next.process.dwProcessId),"same snapshot path now belongs to next launch");
    Check(ReadText(log).find("RESTART-MARKER")==std::string::npos,"next launch overwrites same runtime log");
    size_t snapshots=0;
    for(const auto& entry:std::filesystem::directory_iterator(std::filesystem::current_path()))
        if(entry.path().filename().wstring().ends_with(L".status.json"))++snapshots;
    Check(snapshots==1,"three launches leave exactly one disk status snapshot");
    Check(std::filesystem::exists(log),"stable runtime log is present");
    return gFailures?1:0;
}
