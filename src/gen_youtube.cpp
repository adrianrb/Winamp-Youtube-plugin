#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <winsock2.h>
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

#include "GEN.H"
#include "wa_ipc.h"
#include "resource.h"

static constexpr wchar_t kPluginName[] = L"YouTube no Winamp";
static constexpr wchar_t kIniSection[] = L"youtube";
static constexpr UINT kJobDoneMessage = WM_APP + 1;
static constexpr UINT kLiveReadyMessage = WM_APP + 2;
static constexpr UINT kLiveFailedMessage = WM_APP + 3;
static constexpr UINT kLiveEndedMessage = WM_APP + 4;
static constexpr DWORD kLiveFirstAudioTimeoutMs = 90 * 1000;
static constexpr DWORD kLiveAcceptTimeoutMs = 30 * 1000;
static constexpr DWORD kDownloadTimeoutMs = 15 * 60 * 1000;
static constexpr DWORD kPlaylistTimeoutMs = 120 * 60 * 1000;
static constexpr DWORD kConvertTimeoutMs = 30 * 60 * 1000;

static char g_description[] = "YouTube no Winamp";

static int Init();
static void Config();
static void Quit();

static winampGeneralPurposePlugin g_plugin = {
    GPPHDR_VER,
    g_description,
    Init,
    Config,
    Quit,
    nullptr,
    nullptr
};

extern "C" __declspec(dllexport) winampGeneralPurposePlugin* winampGetGeneralPurposePlugin() {
    return &g_plugin;
}

static HWND g_winamp = nullptr;
static HINSTANCE g_instance = nullptr;
static WNDPROC g_oldWndProc = nullptr;
static std::wstring g_pluginDir;
static std::wstring g_iniPath;
struct LiveContext;

static HWND g_mainDialog = nullptr;
static bool g_jobRunning = false;
static bool g_jobEnqueue = false;
static HANDLE g_jobThread = nullptr;
static HANDLE g_jobStopEvent = nullptr;
static bool g_winsockReady = false;
static bool g_liveStarting = false;
static bool g_liveRunning = false;
static uint64_t g_liveId = 0;
static LiveContext* g_live = nullptr;

struct AudioEntry {
    std::wstring file;
    std::wstring title;
};

struct JobContext {
    HWND dialog;
    std::wstring url;
    std::wstring ytdlp;
};

struct JobResult {
    bool ok = false;
    std::vector<AudioEntry> entries;
    std::wstring error;
};

struct LiveEvent {
    uint64_t id = 0;
    std::wstring url;
    std::wstring error;
};

struct LiveContext {
    uint64_t id = 0;
    HWND dialog = nullptr;
    HANDLE stopEvent = nullptr;
    HANDLE thread = nullptr;
    HANDLE ytdlpProcess = nullptr;
    HANDLE ffmpegProcess = nullptr;
    HANDLE audioPipe = nullptr;
    SOCKET listener = INVALID_SOCKET;
    int port = 0;
    std::wstring url;
    std::wstring ytdlp;
    std::wstring ffmpeg;
    std::wstring workDir;
    std::wstring ytdlpLog;
    std::wstring ffmpegLog;
    CRITICAL_SECTION lock{};
};

struct SettingsValues {
    std::wstring ytdlp;
    std::wstring ffmpeg;
};

static std::wstring Trim(const std::wstring& value) {
    size_t begin = 0;
    size_t end = value.size();
    while (begin < end && iswspace(value[begin])) {
        ++begin;
    }
    while (end > begin && iswspace(value[end - 1])) {
        --end;
    }
    return value.substr(begin, end - begin);
}

static std::wstring ToLower(std::wstring value) {
    for (wchar_t& ch : value) {
        ch = static_cast<wchar_t>(towlower(ch));
    }
    return value;
}

static bool StartsWith(const std::wstring& value, const std::wstring& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

static std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return std::string();
    }
    int length = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return std::string();
    }
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
    return result;
}

static std::string WideToAnsi(const std::wstring& value) {
    if (value.empty()) {
        return std::string();
    }
    int length = WideCharToMultiByte(
        CP_ACP, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return std::string();
    }
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(
        CP_ACP, 0, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
    return result;
}

static std::wstring AnsiToWide(const std::string& value) {
    if (value.empty()) {
        return std::wstring();
    }
    int length = MultiByteToWideChar(
        CP_ACP, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) {
        return std::wstring();
    }
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(
        CP_ACP, 0, value.data(), static_cast<int>(value.size()), result.data(), length);
    return result;
}

static std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return std::wstring();
    }
    int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    UINT codePage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (length <= 0) {
        codePage = CP_ACP;
        flags = 0;
        length = MultiByteToWideChar(
            codePage, flags, value.data(), static_cast<int>(value.size()), nullptr, 0);
    }
    if (length <= 0) {
        return std::wstring();
    }
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(
        codePage, flags, value.data(), static_cast<int>(value.size()), result.data(), length);
    return result;
}

static std::wstring GetModulePath(HMODULE module) {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        DWORD length = GetModuleFileNameW(
            module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return std::wstring();
        }
        if (length < buffer.size()) {
            return std::wstring(buffer.data(), length);
        }
        buffer.resize(buffer.size() * 2);
    }
}

static std::wstring DirectoryOf(const std::wstring& path) {
    size_t position = path.find_last_of(L"\\/");
    if (position == std::wstring::npos) {
        return std::wstring();
    }
    return path.substr(0, position);
}

static std::wstring JoinPath(const std::wstring& left, const std::wstring& right) {
    if (left.empty()) {
        return right;
    }
    if (right.empty()) {
        return left;
    }
    wchar_t last = left[left.size() - 1];
    if (last == L'\\' || last == L'/') {
        return left + right;
    }
    return left + L"\\" + right;
}

static bool FileExists(const std::wstring& path) {
    if (path.empty()) {
        return false;
    }
    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring GetEnvironmentValue(const wchar_t* name) {
    DWORD length = GetEnvironmentVariableW(name, nullptr, 0);
    if (length == 0) {
        return std::wstring();
    }
    std::vector<wchar_t> buffer(length, L'\0');
    DWORD written = GetEnvironmentVariableW(name, buffer.data(), length);
    if (written == 0 || written >= length) {
        return std::wstring();
    }
    return std::wstring(buffer.data(), written);
}

static std::wstring GetCacheRoot() {
    DWORD length = GetTempPathW(0, nullptr);
    if (length == 0) {
        return std::wstring();
    }
    std::vector<wchar_t> buffer(length + 1, L'\0');
    length = GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
    if (length == 0) {
        return std::wstring();
    }
    return JoinPath(std::wstring(buffer.data(), length), L"WinampYouTube");
}

static std::wstring QuoteArgument(const std::wstring& value) {
    std::wstring quoted = L"\"";
    size_t backslashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
        } else {
            quoted.append(backslashes, L'\\');
            quoted.push_back(ch);
        }
        backslashes = 0;
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

static std::wstring GetExtension(const std::wstring& name) {
    size_t position = name.find_last_of(L'.');
    if (position == std::wstring::npos) {
        return std::wstring();
    }
    return name.substr(position);
}

static std::wstring FileNameOf(const std::wstring& path) {
    size_t position = path.find_last_of(L"\\/");
    if (position == std::wstring::npos) {
        return path;
    }
    return path.substr(position + 1);
}

static std::wstring StripExtension(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) {
        return path;
    }
    return path.substr(0, dot);
}

static std::wstring ReplaceExtension(const std::wstring& path, const std::wstring& extension) {
    return StripExtension(path) + extension;
}

static std::wstring HashUrl(const std::wstring& url) {
    std::string utf8 = WideToUtf8(url);
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char ch : utf8) {
        hash ^= static_cast<uint64_t>(ch);
        hash *= 1099511628211ULL;
    }
    wchar_t buffer[17];
    for (int index = 0; index < 16; ++index) {
        unsigned int nibble = static_cast<unsigned int>((hash >> ((15 - index) * 4)) & 0xF);
        buffer[index] = nibble < 10
            ? static_cast<wchar_t>(L'0' + nibble)
            : static_cast<wchar_t>(L'a' + (nibble - 10));
    }
    buffer[16] = L'\0';
    return std::wstring(buffer, 16);
}

static std::wstring NormalizeUrl(const std::wstring& raw) {
    std::wstring url = Trim(raw);
    if (url.empty()) {
        return url;
    }
    std::wstring lower = ToLower(url);
    if (!StartsWith(lower, L"http://") && !StartsWith(lower, L"https://")) {
        if (StartsWith(lower, L"youtu.be/") ||
            StartsWith(lower, L"youtube.com/") ||
            StartsWith(lower, L"www.youtube.com/") ||
            StartsWith(lower, L"m.youtube.com/") ||
            StartsWith(lower, L"music.youtube.com/") ||
            StartsWith(lower, L"youtube-nocookie.com/") ||
            StartsWith(lower, L"www.youtube-nocookie.com/")) {
            url = L"https://" + url;
        }
    }
    return url;
}

static bool HasUnsafeUrlCharacters(const std::wstring& value) {
    for (wchar_t ch : value) {
        if (ch <= 0x20 || ch == 0x7F || ch == L'"') {
            return true;
        }
    }
    return false;
}

static bool IsYouTubeUrl(const std::wstring& url) {
    std::wstring value = Trim(url);
    if (value.empty() || HasUnsafeUrlCharacters(value)) {
        return false;
    }

    std::wstring lower = ToLower(value);
    if (!StartsWith(lower, L"http://") && !StartsWith(lower, L"https://")) {
        return false;
    }
    size_t scheme = lower.find(L"://");
    if (scheme == std::wstring::npos) {
        return false;
    }
    size_t hostStart = scheme + 3;
    size_t hostEnd = lower.find_first_of(L"/?#", hostStart);
    std::wstring host = lower.substr(
        hostStart, hostEnd == std::wstring::npos ? std::wstring::npos : hostEnd - hostStart);
    size_t colon = host.find(L':');
    if (colon != std::wstring::npos) {
        host = host.substr(0, colon);
    }

    bool youtubeHost = host == L"youtube.com" ||
                       host == L"www.youtube.com" ||
                       host == L"m.youtube.com" ||
                       host == L"music.youtube.com" ||
                       host == L"youtube-nocookie.com" ||
                       host == L"www.youtube-nocookie.com";
    if (!youtubeHost && host != L"youtu.be") {
        return false;
    }
    if (hostEnd == std::wstring::npos) {
        return false;
    }

    std::wstring path = lower.substr(hostEnd);
    if (host == L"youtu.be") {
        return path.size() > 1 && path[0] == L'/';
    }
    return StartsWith(path, L"/watch") ||
           StartsWith(path, L"/shorts/") ||
           StartsWith(path, L"/live/") ||
           StartsWith(path, L"/embed/") ||
           StartsWith(path, L"/v/") ||
           StartsWith(path, L"/playlist") ||
           StartsWith(path, L"/clip/");
}

static bool IsPlaylistUrl(const std::wstring& url) {
    return ToLower(url).find(L"/playlist") != std::wstring::npos;
}

static std::wstring FormatWin32Error(DWORD code) {
    wchar_t* buffer = nullptr;
    DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        0,
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);
    std::wstring message;
    if (length > 0 && buffer != nullptr) {
        message.assign(buffer, length);
    }
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    message = Trim(message);
    if (message.empty()) {
        message = L"Erro desconhecido";
    }
    return message + L" (" + std::to_wstring(code) + L")";
}

static std::wstring ReadSetting(const wchar_t* key) {
    if (g_iniPath.empty()) {
        return std::wstring();
    }
    std::vector<wchar_t> buffer(32768, L'\0');
    DWORD length = GetPrivateProfileStringW(
        kIniSection,
        key,
        L"",
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        g_iniPath.c_str());
    return Trim(std::wstring(buffer.data(), length));
}

static void WriteSetting(const wchar_t* key, const std::wstring& value) {
    if (g_iniPath.empty()) {
        return;
    }
    WritePrivateProfileStringW(
        kIniSection,
        key,
        value.empty() ? nullptr : value.c_str(),
        g_iniPath.c_str());
}

static std::wstring FindTool(const wchar_t* executable, const std::wstring& configured) {
    if (!configured.empty() && FileExists(configured)) {
        return configured;
    }

    std::vector<std::wstring> directories;
    if (!g_pluginDir.empty()) {
        directories.push_back(g_pluginDir);
    }
    std::wstring winampPath = GetModulePath(nullptr);
    std::wstring winampDir = DirectoryOf(winampPath);
    if (!winampDir.empty()) {
        directories.push_back(winampDir);
    }
    std::wstring localAppData = GetEnvironmentValue(L"LOCALAPPDATA");
    if (!localAppData.empty()) {
        directories.push_back(JoinPath(localAppData, L"Microsoft\\WinGet\\Links"));
        directories.push_back(JoinPath(localAppData, L"Programs\\yt-dlp"));
    }
    std::wstring userProfile = GetEnvironmentValue(L"USERPROFILE");
    if (!userProfile.empty()) {
        directories.push_back(JoinPath(userProfile, L"scoop\\shims"));
    }

    for (const std::wstring& directory : directories) {
        std::wstring candidate = JoinPath(directory, executable);
        if (FileExists(candidate)) {
            return candidate;
        }
    }

    wchar_t found[32768];
    DWORD length = SearchPathW(nullptr, executable, nullptr, 32768, found, nullptr);
    if (length > 0 && length < 32768) {
        return std::wstring(found, length);
    }
    return std::wstring();
}

static std::wstring FindFfmpeg(const std::wstring& ytdlpPath) {
    std::wstring ffmpeg = FindTool(L"ffmpeg.exe", ReadSetting(L"ffmpeg"));
    if (!ffmpeg.empty()) {
        return ffmpeg;
    }
    if (!ytdlpPath.empty()) {
        std::wstring candidate = JoinPath(DirectoryOf(ytdlpPath), L"ffmpeg.exe");
        if (FileExists(candidate)) {
            return candidate;
        }
    }
    return std::wstring();
}

static bool EnsureDirectory(const std::wstring& path, std::wstring& error) {
    if (path.empty()) {
        error = L"Caminho de cache inválido.";
        return false;
    }
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
            return true;
        }
        error = L"Já existe um arquivo no lugar da pasta: " + path;
        return false;
    }
    if (CreateDirectoryW(path.c_str(), nullptr)) {
        return true;
    }
    DWORD code = GetLastError();
    if (code == ERROR_ALREADY_EXISTS) {
        return true;
    }
    error = L"Não foi possível criar a pasta " + path + L": " + FormatWin32Error(code);
    return false;
}

static std::string ReadLogTail(const std::wstring& path) {
    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::string();
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0) {
        CloseHandle(file);
        return std::string();
    }
    const LONGLONG maximum = 8192;
    LARGE_INTEGER offset{};
    if (size.QuadPart > maximum) {
        offset.QuadPart = size.QuadPart - maximum;
    }
    SetFilePointerEx(file, offset, nullptr, FILE_BEGIN);
    DWORD toRead = static_cast<DWORD>(size.QuadPart - offset.QuadPart);
    std::vector<char> buffer(static_cast<size_t>(toRead) + 1, '\0');
    DWORD read = 0;
    ReadFile(file, buffer.data(), toRead, &read, nullptr);
    CloseHandle(file);
    return std::string(buffer.data(), read);
}

static bool RunProcess(
    const std::wstring& executable,
    const std::wstring& arguments,
    const std::wstring& workingDirectory,
    const std::wstring& logPath,
    DWORD timeoutMs,
    DWORD& exitCode,
    std::wstring& error) {
    if (g_jobStopEvent != nullptr &&
        WaitForSingleObject(g_jobStopEvent, 0) == WAIT_OBJECT_0) {
        error = L"Operação cancelada.";
        return false;
    }

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE log = CreateFileW(
        logPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (log == INVALID_HANDLE_VALUE) {
        error = L"Não foi possível criar o log em " + logPath + L": " +
                FormatWin32Error(GetLastError());
        return false;
    }

    HANDLE nullInput = CreateFileW(
        L"NUL",
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nullInput != INVALID_HANDLE_VALUE ? nullInput : log;
    startup.hStdOutput = log;
    startup.hStdError = log;

    PROCESS_INFORMATION process{};
    std::wstring commandLine = QuoteArgument(executable);
    if (!arguments.empty()) {
        commandLine += L" " + arguments;
    }
    std::vector<wchar_t> command(commandLine.begin(), commandLine.end());
    command.push_back(L'\0');

    BOOL started = CreateProcessW(
        executable.c_str(),
        command.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
        &startup,
        &process);
    DWORD createError = GetLastError();

    if (nullInput != INVALID_HANDLE_VALUE) {
        CloseHandle(nullInput);
    }
    CloseHandle(log);

    if (!started) {
        error = L"Não foi possível executar " + executable + L": " +
                FormatWin32Error(createError);
        return false;
    }

    HANDLE waitHandles[2] = {process.hProcess, g_jobStopEvent};
    DWORD handleCount = g_jobStopEvent != nullptr ? 2 : 1;
    DWORD wait = WaitForMultipleObjects(handleCount, waitHandles, FALSE, timeoutMs);
    if (wait == WAIT_OBJECT_0 + 1) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 5000);
        exitCode = 1;
        error = L"Operação cancelada.";
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return false;
    }
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 5000);
        exitCode = 1;
        error = L"O processo excedeu o tempo limite.";
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return false;
    }
    if (wait == WAIT_FAILED) {
        exitCode = 1;
        error = L"Falha ao aguardar o processo: " +
                FormatWin32Error(GetLastError());
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return false;
    }

    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    if (exitCode != 0) {
        std::wstring tail = Trim(Utf8ToWide(ReadLogTail(logPath)));
        if (tail.size() > 1500) {
            tail = tail.substr(tail.size() - 1500);
        }
        error = L"Falha ao executar " + executable + L" (código " +
                std::to_wstring(exitCode) + L").";
        if (!tail.empty()) {
            error += L"\n\n" + tail;
        }
        return false;
    }
    return true;
}

static int AudioRank(const std::wstring& extension) {
    if (extension == L".m4a") {
        return 0;
    }
    if (extension == L".mp3") {
        return 1;
    }
    if (extension == L".flac") {
        return 2;
    }
    if (extension == L".wav") {
        return 3;
    }
    if (extension == L".aac") {
        return 4;
    }
    if (extension == L".ogg") {
        return 5;
    }
    if (extension == L".opus") {
        return 6;
    }
    if (extension == L".webm") {
        return 7;
    }
    if (extension == L".mka") {
        return 8;
    }
    return 100;
}

static bool PlaylistIndexFromName(
    const std::wstring& name, unsigned long long& index) {
    size_t position = 0;
    while (position < name.size() &&
           name[position] >= L'0' && name[position] <= L'9') {
        ++position;
    }
    if (position == 0 || position >= name.size() || name[position] != L' ') {
        return false;
    }

    index = 0;
    for (size_t i = 0; i < position; ++i) {
        if (index > 100000000ULL) {
            return false;
        }
        index = index * 10 + static_cast<unsigned int>(name[i] - L'0');
    }
    return true;
}

static std::wstring FindAudioFile(const std::wstring& directory) {
    WIN32_FIND_DATAW data{};
    std::wstring pattern = JoinPath(directory, L"audio.*");
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        return std::wstring();
    }

    std::wstring best;
    int bestRank = 101;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            continue;
        }
        std::wstring name = data.cFileName;
        std::wstring extension = ToLower(GetExtension(name));
        int rank = AudioRank(extension);
        if (rank >= 100) {
            continue;
        }
        if (rank < bestRank) {
            bestRank = rank;
            best = JoinPath(directory, name);
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return best;
}

static std::vector<std::wstring> FindAllAudioFiles(const std::wstring& directory) {
    std::vector<std::wstring> files;
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(JoinPath(directory, L"*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        return files;
    }

    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            continue;
        }
        std::wstring name = data.cFileName;
        if (AudioRank(ToLower(GetExtension(name))) >= 100) {
            continue;
        }
        files.push_back(JoinPath(directory, name));
    } while (FindNextFileW(find, &data));
    FindClose(find);

    std::sort(
        files.begin(),
        files.end(),
        [](const std::wstring& left, const std::wstring& right) {
            return ToLower(FileNameOf(left)) < ToLower(FileNameOf(right));
        });
    return files;
}

static std::wstring EnsurePlayable(
    const std::wstring& source, const std::wstring& ytdlpPath, std::wstring& error) {
    std::wstring extension = ToLower(GetExtension(source));
    int rank = AudioRank(extension);
    if (rank <= 5) {
        return source;
    }
    if (rank >= 100) {
        error = L"Formato de áudio não reconhecido: " + source;
        return std::wstring();
    }

    std::wstring ffmpeg = FindFfmpeg(ytdlpPath);
    if (ffmpeg.empty()) {
        error = L"O áudio foi baixado em " + extension +
                L", formato que o Winamp não reproduz.\n\n"
                L"Instale o ffmpeg.exe e informe o caminho em Configurações para converter automaticamente.";
        return std::wstring();
    }

    std::wstring directory = DirectoryOf(source);
    std::wstring mp3 = ReplaceExtension(source, L".mp3");
    std::wstring arguments =
        L"-y -hide_banner -loglevel error -i " + QuoteArgument(source) +
        L" -vn -c:a libmp3lame -q:a 2 " + QuoteArgument(mp3);
    std::wstring logPath = JoinPath(directory, L"ffmpeg.log");

    DWORD exitCode = 1;
    if (!RunProcess(
            ffmpeg,
            arguments,
            directory,
            logPath,
            kConvertTimeoutMs,
            exitCode,
            error)) {
        DeleteFileW(mp3.c_str());
        return std::wstring();
    }
    if (!FileExists(mp3)) {
        error = L"A conversão para MP3 não gerou o arquivo esperado.";
        DeleteFileW(mp3.c_str());
        return std::wstring();
    }

    DeleteFileW(source.c_str());
    return mp3;
}

static bool BuildSingleEntry(
    const std::wstring& directory,
    const std::wstring& ytdlpPath,
    const std::wstring& url,
    std::vector<AudioEntry>& entries,
    std::wstring& error) {
    std::wstring downloaded = FindAudioFile(directory);
    if (downloaded.empty()) {
        error = L"O yt-dlp terminou, mas nenhum arquivo de áudio foi encontrado.";
        return false;
    }

    std::wstring playable = EnsurePlayable(downloaded, ytdlpPath, error);
    if (playable.empty()) {
        return false;
    }

    entries.clear();
    entries.push_back(AudioEntry{playable, url});
    return true;
}

static bool BuildPlaylistEntries(
    const std::wstring& directory,
    const std::wstring& ytdlpPath,
    std::vector<AudioEntry>& entries,
    std::wstring& error) {
    std::vector<std::wstring> files = FindAllAudioFiles(directory);
    if (files.empty()) {
        error = L"A playlist terminou, mas nenhum arquivo de áudio foi encontrado.";
        return false;
    }

    struct Candidate {
        std::wstring path;
        std::wstring key;
        int rank = 100;
    };

    std::vector<Candidate> candidates;
    for (const std::wstring& file : files) {
        std::wstring name = FileNameOf(file);
        std::wstring key = ToLower(StripExtension(name));
        int rank = AudioRank(ToLower(GetExtension(name)));
        auto existing = std::find_if(
            candidates.begin(),
            candidates.end(),
            [&](const Candidate& candidate) { return candidate.key == key; });
        if (existing == candidates.end()) {
            candidates.push_back(Candidate{file, key, rank});
        } else if (rank < existing->rank) {
            existing->path = file;
            existing->rank = rank;
        }
    }

    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const Candidate& left, const Candidate& right) {
            unsigned long long leftIndex = 0;
            unsigned long long rightIndex = 0;
            bool leftNumbered = PlaylistIndexFromName(FileNameOf(left.path), leftIndex);
            bool rightNumbered = PlaylistIndexFromName(FileNameOf(right.path), rightIndex);
            if (leftNumbered && rightNumbered && leftIndex != rightIndex) {
                return leftIndex < rightIndex;
            }
            if (leftNumbered != rightNumbered) {
                return leftNumbered;
            }
            return left.key < right.key;
        });

    entries.clear();
    for (const Candidate& candidate : candidates) {
        std::wstring playable = EnsurePlayable(candidate.path, ytdlpPath, error);
        if (playable.empty()) {
            entries.clear();
            return false;
        }
        entries.push_back(AudioEntry{
            playable,
            StripExtension(FileNameOf(playable))});
    }

    if (entries.empty()) {
        error = L"A playlist não contém arquivos de áudio reproduzíveis.";
        return false;
    }
    return true;
}

static void DeleteMatchingFiles(
    const std::wstring& directory, const std::wstring& pattern) {
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(JoinPath(directory, pattern).c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            DeleteFileW(JoinPath(directory, data.cFileName).c_str());
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);
}

static void MarkPlaylistComplete(const std::wstring& directory) {
    std::wstring marker = JoinPath(directory, L"playlist.complete");
    HANDLE file = CreateFileW(
        marker.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
    }
}

static bool DownloadMedia(
    const JobContext& job, std::vector<AudioEntry>& entries, std::wstring& error) {
    std::wstring cacheRoot = GetCacheRoot();
    if (cacheRoot.empty()) {
        error = L"Não foi possível obter a pasta temporária do Windows.";
        return false;
    }

    bool playlist = IsPlaylistUrl(job.url);
    std::wstring cacheDirectory = JoinPath(cacheRoot, HashUrl(job.url));
    if (!EnsureDirectory(cacheRoot, error) || !EnsureDirectory(cacheDirectory, error)) {
        return false;
    }

    if (playlist) {
        std::wstring marker = JoinPath(cacheDirectory, L"playlist.complete");
        if (FileExists(marker)) {
            if (!FindAllAudioFiles(cacheDirectory).empty()) {
                return BuildPlaylistEntries(cacheDirectory, job.ytdlp, entries, error);
            }
            DeleteFileW(marker.c_str());
        }
    } else if (!FindAudioFile(cacheDirectory).empty()) {
        return BuildSingleEntry(cacheDirectory, job.ytdlp, job.url, entries, error);
    }

    if (!FileExists(job.ytdlp)) {
        error = L"yt-dlp.exe não encontrado. Configure o caminho em Configurações.";
        return false;
    }

    if (playlist) {
        // Não reutilize arquivos de uma playlist antiga ou incompleta.
        DeleteMatchingFiles(cacheDirectory, L"*");
    }

    std::wstring outputName = playlist
        ? L"%(playlist_index)03d - %(title)s.%(ext)s"
        : L"audio.%(ext)s";
    std::wstring outputTemplate = JoinPath(cacheDirectory, outputName);

    std::wstring arguments = playlist ? L"--yes-playlist " : L"--no-playlist ";
    arguments += L"--no-mtime --newline --no-warnings --socket-timeout 20 --retries 2 ";
    arguments += L"-f \"bestaudio[ext=m4a]/bestaudio[ext=mp3]/bestaudio\" -o ";
    arguments += QuoteArgument(outputTemplate);
    arguments += L" " + QuoteArgument(job.url);

    std::wstring logPath = JoinPath(cacheDirectory, L"yt-dlp.log");
    DWORD timeoutMs = playlist ? kPlaylistTimeoutMs : kDownloadTimeoutMs;
    DWORD exitCode = 1;
    if (!RunProcess(
            job.ytdlp,
            arguments,
            cacheDirectory,
            logPath,
            timeoutMs,
            exitCode,
            error)) {
        return false;
    }

    if (playlist) {
        if (!BuildPlaylistEntries(cacheDirectory, job.ytdlp, entries, error)) {
            return false;
        }
        MarkPlaylistComplete(cacheDirectory);
        return true;
    }
    return BuildSingleEntry(cacheDirectory, job.ytdlp, job.url, entries, error);
}

static DWORD WINAPI WorkerThread(LPVOID parameter) {
    JobContext* job = static_cast<JobContext*>(parameter);
    JobResult* result = new JobResult();
    std::vector<AudioEntry> entries;
    result->ok = DownloadMedia(*job, entries, result->error);
    if (result->ok) {
        result->entries = entries;
    }
    if (!PostMessageW(job->dialog, kJobDoneMessage, 0, reinterpret_cast<LPARAM>(result))) {
        delete result;
    }
    delete job;
    return 0;
}

static std::wstring GetDialogText(HWND dialog, int controlId) {
    HWND control = GetDlgItem(dialog, controlId);
    int length = GetWindowTextLengthW(control);
    std::vector<wchar_t> buffer(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, buffer.data(), static_cast<int>(buffer.size()));
    return std::wstring(buffer.data());
}

static void SetStatus(HWND dialog, const std::wstring& text) {
    SetDlgItemTextW(dialog, IDC_STATUS, text.c_str());
}

static void UpdateDialogControls(HWND dialog) {
    BOOL enabled = (g_jobRunning || g_liveStarting) ? FALSE : TRUE;
    const int controls[] = {
        IDC_URL,
        IDC_PLAY,
        IDC_ENQUEUE,
        IDC_PASTE,
        IDC_SETTINGS_BTN,
        IDCANCEL
    };
    for (int control : controls) {
        EnableWindow(GetDlgItem(dialog, control), enabled);
    }
    EnableWindow(
        GetDlgItem(dialog, IDC_LIVE),
        enabled && !g_liveRunning ? TRUE : FALSE);
    EnableWindow(GetDlgItem(dialog, IDC_STOP_LIVE), g_liveRunning ? TRUE : FALSE);
}

static std::wstring ReadClipboardText() {
    if (!OpenClipboard(nullptr)) {
        return std::wstring();
    }
    std::wstring result;
    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (data != nullptr) {
        const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(data));
        if (text != nullptr) {
            result = text;
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    return Trim(result);
}

static void PasteClipboard(HWND dialog) {
    std::wstring text = ReadClipboardText();
    if (!text.empty()) {
        SetDlgItemTextW(dialog, IDC_URL, text.c_str());
    }
    SetFocus(GetDlgItem(dialog, IDC_URL));
}

static bool PickExecutable(
    HWND owner, const std::wstring& current, std::wstring& selected) {
    wchar_t file[32768] = {};
    if (!current.empty() && current.size() < 32767) {
        wmemcpy(file, current.c_str(), current.size() + 1);
    }

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter =
        L"Executáveis (*.exe)\0*.exe\0Todos os arquivos (*.*)\0*.*\0";
    dialog.lpstrFile = file;
    dialog.nMaxFile = 32768;
    dialog.lpstrTitle = L"Selecionar executável";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetOpenFileNameW(&dialog)) {
        return false;
    }
    selected = file;
    return true;
}

static void DeleteTree(const std::wstring& path) {
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(JoinPath(path, L"*").c_str(), &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(data.cFileName, L".") == 0 ||
                wcscmp(data.cFileName, L"..") == 0) {
                continue;
            }
            std::wstring child = JoinPath(path, data.cFileName);
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                DeleteTree(child);
            } else {
                DeleteFileW(child.c_str());
            }
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    RemoveDirectoryW(path.c_str());
}

static void SendToWinamp(const std::vector<AudioEntry>& entries, bool enqueue) {
    if (!IsWindow(g_winamp) || entries.empty()) {
        return;
    }

    if (!enqueue) {
        SendMessageW(g_winamp, WM_WA_IPC, 0, IPC_DELETE);
    }

    int version = static_cast<int>(SendMessageW(g_winamp, WM_WA_IPC, 0, IPC_GETVERSION));
    for (const AudioEntry& audio : entries) {
        if (version >= 0x5003) {
            enqueueFileWithMetaStructW entry{};
            entry.filename = audio.file.c_str();
            entry.title = audio.title.c_str();
            entry.length = 0;
            SendMessageW(
                g_winamp, WM_WA_IPC, reinterpret_cast<WPARAM>(&entry), IPC_ENQUEUEFILEW);
        } else {
            std::string ansi = WideToAnsi(audio.file);
            COPYDATASTRUCT data{};
            data.dwData = IPC_PLAYFILE;
            data.cbData = static_cast<DWORD>(ansi.size() + 1);
            data.lpData = ansi.data();
            SendMessageW(
                g_winamp,
                WM_COPYDATA,
                reinterpret_cast<WPARAM>(g_winamp),
                reinterpret_cast<LPARAM>(&data));
        }
    }

    if (!enqueue) {
        int length = static_cast<int>(
            SendMessageW(g_winamp, WM_WA_IPC, 0, IPC_GETLISTLENGTH));
        if (length > 0) {
            int firstAdded = length - static_cast<int>(entries.size());
            if (firstAdded < 0) {
                firstAdded = 0;
            }
            SendMessageW(
                g_winamp,
                WM_WA_IPC,
                static_cast<WPARAM>(firstAdded),
                IPC_SETPLAYLISTPOS);
        }
        SendMessageW(g_winamp, WM_WA_IPC, 0, IPC_STARTPLAY);
    }
}

static void SendToWinamp(
    const std::wstring& file, const std::wstring& title, bool enqueue) {
    std::vector<AudioEntry> entries;
    entries.push_back(AudioEntry{file, title});
    SendToWinamp(entries, enqueue);
}

static bool IsLiveStopping(const LiveContext* live) {
    return live != nullptr && live->stopEvent != nullptr &&
           WaitForSingleObject(live->stopEvent, 0) == WAIT_OBJECT_0;
}

static void TerminateLiveProcesses(LiveContext* live) {
    if (live == nullptr) {
        return;
    }
    EnterCriticalSection(&live->lock);
    HANDLE ytdlp = live->ytdlpProcess;
    HANDLE ffmpeg = live->ffmpegProcess;
    LeaveCriticalSection(&live->lock);

    if (ytdlp != nullptr) {
        TerminateProcess(ytdlp, 1);
    }
    if (ffmpeg != nullptr) {
        TerminateProcess(ffmpeg, 1);
    }
}

static void CloseLiveResources(LiveContext* live) {
    if (live == nullptr) {
        return;
    }

    EnterCriticalSection(&live->lock);
    HANDLE ytdlp = live->ytdlpProcess;
    HANDLE ffmpeg = live->ffmpegProcess;
    HANDLE audioPipe = live->audioPipe;
    SOCKET listener = live->listener;
    live->ytdlpProcess = nullptr;
    live->ffmpegProcess = nullptr;
    live->audioPipe = nullptr;
    live->listener = INVALID_SOCKET;
    LeaveCriticalSection(&live->lock);

    if (audioPipe != nullptr) {
        CloseHandle(audioPipe);
    }
    if (ytdlp != nullptr) {
        CloseHandle(ytdlp);
    }
    if (ffmpeg != nullptr) {
        CloseHandle(ffmpeg);
    }
    if (listener != INVALID_SOCKET) {
        closesocket(listener);
    }
}

static bool PostLiveEvent(
    HWND target,
    UINT message,
    uint64_t id,
    const std::wstring& url,
    const std::wstring& error) {
    if (target == nullptr || !IsWindow(target)) {
        return false;
    }
    LiveEvent* event = new LiveEvent();
    event->id = id;
    event->url = url;
    event->error = error;
    if (!PostMessageW(target, message, 0, reinterpret_cast<LPARAM>(event))) {
        delete event;
        return false;
    }
    return true;
}

static std::wstring LiveLogTail(const LiveContext* live) {
    if (live == nullptr) {
        return std::wstring();
    }
    std::wstring ytdlp = Trim(Utf8ToWide(ReadLogTail(live->ytdlpLog)));
    std::wstring ffmpeg = Trim(Utf8ToWide(ReadLogTail(live->ffmpegLog)));
    std::wstring tail;
    if (!ytdlp.empty()) {
        tail += L"\n\nyt-dlp:\n" + ytdlp;
    }
    if (!ffmpeg.empty()) {
        tail += L"\n\nffmpeg:\n" + ffmpeg;
    }
    if (tail.size() > 2000) {
        tail = tail.substr(tail.size() - 2000);
    }
    return tail;
}

static HANDLE CreateInheritableLog(const std::wstring& path) {
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    return CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
}

static HANDLE CreateInheritableNull() {
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    return CreateFileW(
        L"NUL",
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
}

static void CloseHandleIfValid(HANDLE& handle) {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
    }
    handle = nullptr;
}

static bool CreateLiveProcesses(LiveContext* live, std::wstring& error) {
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE ytdlpRead = nullptr;
    HANDLE ytdlpWrite = nullptr;
    HANDLE mp3Read = nullptr;
    HANDLE mp3Write = nullptr;
    HANDLE nullInput = nullptr;
    HANDLE log = nullptr;
    HANDLE ytdlpProcess = nullptr;
    HANDLE ffmpegProcess = nullptr;

    if (!CreatePipe(&ytdlpRead, &ytdlpWrite, &security, 0)) {
        error = L"Não foi possível criar o pipe entre yt-dlp e ffmpeg: " +
                FormatWin32Error(GetLastError());
        return false;
    }
    if (!CreatePipe(&mp3Read, &mp3Write, &security, 0)) {
        error = L"Não foi possível criar o pipe de áudio da transmissão: " +
                FormatWin32Error(GetLastError());
        CloseHandleIfValid(ytdlpRead);
        CloseHandleIfValid(ytdlpWrite);
        return false;
    }

    SetHandleInformation(mp3Read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(mp3Write, HANDLE_FLAG_INHERIT, 0);

    nullInput = CreateInheritableNull();
    if (nullInput == INVALID_HANDLE_VALUE) {
        error = L"Não foi possível preparar a entrada do yt-dlp: " +
                FormatWin32Error(GetLastError());
        CloseHandleIfValid(ytdlpRead);
        CloseHandleIfValid(ytdlpWrite);
        CloseHandleIfValid(mp3Read);
        CloseHandleIfValid(mp3Write);
        return false;
    }

    log = CreateInheritableLog(live->ytdlpLog);
    if (log == INVALID_HANDLE_VALUE) {
        error = L"Não foi possível criar o log do yt-dlp: " +
                FormatWin32Error(GetLastError());
        CloseHandleIfValid(nullInput);
        CloseHandleIfValid(ytdlpRead);
        CloseHandleIfValid(ytdlpWrite);
        CloseHandleIfValid(mp3Read);
        CloseHandleIfValid(mp3Write);
        return false;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nullInput;
    startup.hStdOutput = ytdlpWrite;
    startup.hStdError = log;

    std::wstring ytdlpArguments =
        L"--no-playlist --no-warnings --newline --socket-timeout 20 --retries 3 "
        L"-f \"bestaudio/best\" -o - " + QuoteArgument(live->url);
    std::wstring ytdlpCommand = QuoteArgument(live->ytdlp) + L" " + ytdlpArguments;
    std::vector<wchar_t> ytdlpCommandLine(ytdlpCommand.begin(), ytdlpCommand.end());
    ytdlpCommandLine.push_back(L'\0');

    PROCESS_INFORMATION ytdlpInfo{};
    BOOL ytdlpStarted = CreateProcessW(
        live->ytdlp.c_str(),
        ytdlpCommandLine.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        live->workDir.c_str(),
        &startup,
        &ytdlpInfo);
    DWORD ytdlpCreateError = GetLastError();
    CloseHandleIfValid(nullInput);
    CloseHandleIfValid(log);

    if (!ytdlpStarted) {
        error = L"Não foi possível iniciar o yt-dlp para a transmissão: " +
                FormatWin32Error(ytdlpCreateError);
        CloseHandleIfValid(ytdlpRead);
        CloseHandleIfValid(ytdlpWrite);
        CloseHandleIfValid(mp3Read);
        CloseHandleIfValid(mp3Write);
        return false;
    }
    CloseHandle(ytdlpInfo.hThread);
    ytdlpProcess = ytdlpInfo.hProcess;

    SetHandleInformation(ytdlpWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(mp3Write, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

    log = CreateInheritableLog(live->ffmpegLog);
    if (log == INVALID_HANDLE_VALUE) {
        error = L"Não foi possível criar o log do ffmpeg: " +
                FormatWin32Error(GetLastError());
        TerminateProcess(ytdlpProcess, 1);
        CloseHandleIfValid(ytdlpProcess);
        CloseHandleIfValid(ytdlpRead);
        CloseHandleIfValid(ytdlpWrite);
        CloseHandleIfValid(mp3Read);
        CloseHandleIfValid(mp3Write);
        return false;
    }

    startup = STARTUPINFOW{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = ytdlpRead;
    startup.hStdOutput = mp3Write;
    startup.hStdError = log;

    std::wstring ffmpegArguments =
        L"-hide_banner -loglevel error -i pipe:0 -vn -c:a libmp3lame "
        L"-b:a 128k -flush_packets 1 -f mp3 pipe:1";
    std::wstring ffmpegCommand = QuoteArgument(live->ffmpeg) + L" " + ffmpegArguments;
    std::vector<wchar_t> ffmpegCommandLine(ffmpegCommand.begin(), ffmpegCommand.end());
    ffmpegCommandLine.push_back(L'\0');

    PROCESS_INFORMATION ffmpegInfo{};
    BOOL ffmpegStarted = CreateProcessW(
        live->ffmpeg.c_str(),
        ffmpegCommandLine.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        live->workDir.c_str(),
        &startup,
        &ffmpegInfo);
    DWORD ffmpegCreateError = GetLastError();
    CloseHandleIfValid(log);
    CloseHandleIfValid(ytdlpRead);
    CloseHandleIfValid(ytdlpWrite);
    CloseHandleIfValid(mp3Write);

    if (!ffmpegStarted) {
        error = L"Não foi possível iniciar o ffmpeg para a transmissão: " +
                FormatWin32Error(ffmpegCreateError);
        TerminateProcess(ytdlpProcess, 1);
        CloseHandleIfValid(ytdlpProcess);
        CloseHandleIfValid(mp3Read);
        return false;
    }
    CloseHandle(ffmpegInfo.hThread);
    ffmpegProcess = ffmpegInfo.hProcess;

    EnterCriticalSection(&live->lock);
    live->ytdlpProcess = ytdlpProcess;
    live->ffmpegProcess = ffmpegProcess;
    live->audioPipe = mp3Read;
    LeaveCriticalSection(&live->lock);

    return true;
}

static bool StartLiveServer(LiveContext* live, std::wstring& error) {
    if (!g_winsockReady) {
        error = L"Winsock não foi inicializado; a transmissão ao vivo não está disponível.";
        return false;
    }

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        error = L"Não foi possível criar o servidor local da transmissão: " +
                FormatWin32Error(static_cast<DWORD>(WSAGetLastError()));
        return false;
    }

    BOOL reuse = TRUE;
    setsockopt(
        listener,
        SOL_SOCKET,
        SO_REUSEADDR,
        reinterpret_cast<const char*>(&reuse),
        sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) ==
        SOCKET_ERROR) {
        error = L"Não foi possível abrir a porta local da transmissão: " +
                FormatWin32Error(static_cast<DWORD>(WSAGetLastError()));
        closesocket(listener);
        return false;
    }
    if (listen(listener, 1) == SOCKET_ERROR) {
        error = L"Não foi possível escutar a conexão do Winamp: " +
                FormatWin32Error(static_cast<DWORD>(WSAGetLastError()));
        closesocket(listener);
        return false;
    }

    sockaddr_in bound{};
    int length = sizeof(bound);
    if (getsockname(
            listener,
            reinterpret_cast<sockaddr*>(&bound),
            &length) == SOCKET_ERROR) {
        error = L"Não foi possível obter a porta da transmissão: " +
                FormatWin32Error(static_cast<DWORD>(WSAGetLastError()));
        closesocket(listener);
        return false;
    }

    EnterCriticalSection(&live->lock);
    live->listener = listener;
    live->port = ntohs(bound.sin_port);
    LeaveCriticalSection(&live->lock);
    if (live->port <= 0) {
        error = L"A porta local da transmissão é inválida.";
        return false;
    }
    return true;
}

static bool WaitForLiveAudio(LiveContext* live, std::wstring& error) {
    DWORD started = GetTickCount();
    for (;;) {
        if (IsLiveStopping(live)) {
            return false;
        }

        DWORD available = 0;
        if (!PeekNamedPipe(
                live->audioPipe,
                nullptr,
                0,
                nullptr,
                &available,
                nullptr)) {
            if (IsLiveStopping(live)) {
                return false;
            }
            error = L"Falha ao ler o áudio da transmissão: " +
                    FormatWin32Error(GetLastError()) + LiveLogTail(live);
            return false;
        }
        if (available > 0) {
            return true;
        }

        if (live->ffmpegProcess != nullptr &&
            WaitForSingleObject(live->ffmpegProcess, 0) == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(live->ffmpegProcess, &code);
            error = L"O ffmpeg terminou antes de iniciar a transmissão (código " +
                    std::to_wstring(code) + L")." + LiveLogTail(live);
            return false;
        }

        if (GetTickCount() - started >= kLiveFirstAudioTimeoutMs) {
            error = L"Tempo esgotado aguardando o áudio da transmissão." +
                    LiveLogTail(live);
            return false;
        }
        WaitForSingleObject(live->stopEvent, 50);
    }
}

static bool RunLivePipeline(LiveContext* live, std::wstring& error) {
    if (IsLiveStopping(live)) {
        return false;
    }

    std::wstring cacheRoot = GetCacheRoot();
    if (cacheRoot.empty()) {
        error = L"Não foi possível obter a pasta temporária do Windows.";
        return false;
    }
    if (!EnsureDirectory(cacheRoot, error) ||
        !EnsureDirectory(live->workDir, error)) {
        return false;
    }
    if (!CreateLiveProcesses(live, error)) {
        return false;
    }
    if (IsLiveStopping(live)) {
        return false;
    }
    if (!StartLiveServer(live, error)) {
        return false;
    }
    return WaitForLiveAudio(live, error);
}

static bool SendAllSocket(SOCKET client, const char* data, int length) {
    while (length > 0) {
        int sent = send(client, data, length, 0);
        if (sent == SOCKET_ERROR || sent == 0) {
            return false;
        }
        data += sent;
        length -= sent;
    }
    return true;
}

static bool AcceptLiveClient(
    LiveContext* live, SOCKET& client, std::wstring& error) {
    DWORD started = GetTickCount();
    for (;;) {
        if (IsLiveStopping(live)) {
            return false;
        }

        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(live->listener, &readSet);
        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 200000;

        int result = select(0, &readSet, nullptr, nullptr, &timeout);
        if (result == SOCKET_ERROR) {
            error = L"Falha ao aguardar a conexão do Winamp: " +
                    FormatWin32Error(static_cast<DWORD>(WSAGetLastError()));
            return false;
        }
        if (result > 0 && FD_ISSET(live->listener, &readSet)) {
            SOCKET accepted = accept(live->listener, nullptr, nullptr);
            if (accepted == INVALID_SOCKET) {
                error = L"Falha ao aceitar a conexão do Winamp: " +
                        FormatWin32Error(static_cast<DWORD>(WSAGetLastError()));
                return false;
            }
            DWORD socketTimeout = 2000;
            setsockopt(
                accepted,
                SOL_SOCKET,
                SO_RCVTIMEO,
                reinterpret_cast<const char*>(&socketTimeout),
                sizeof(socketTimeout));
            setsockopt(
                accepted,
                SOL_SOCKET,
                SO_SNDTIMEO,
                reinterpret_cast<const char*>(&socketTimeout),
                sizeof(socketTimeout));
            client = accepted;
            return true;
        }

        if (GetTickCount() - started >= kLiveAcceptTimeoutMs) {
            error = L"O Winamp não se conectou à transmissão ao vivo a tempo.";
            return false;
        }
    }
}

static bool ReadHttpRequest(
    SOCKET client, LiveContext* live, std::string& request, std::wstring& error) {
    char buffer[4096];
    for (;;) {
        if (IsLiveStopping(live)) {
            return false;
        }
        int received = recv(client, buffer, static_cast<int>(sizeof(buffer)), 0);
        if (received > 0) {
            request.append(buffer, static_cast<size_t>(received));
            if (request.find("\r\n\r\n") != std::string::npos ||
                request.size() > 32768) {
                return true;
            }
        } else if (received == 0) {
            break;
        } else {
            int code = WSAGetLastError();
            if (code == WSAETIMEDOUT) {
                break;
            }
            if (code == WSAEWOULDBLOCK) {
                WaitForSingleObject(live->stopEvent, 20);
                continue;
            }
            error = L"Falha ao ler a requisição HTTP do Winamp: " +
                    FormatWin32Error(static_cast<DWORD>(code));
            return false;
        }
    }

    if (IsLiveStopping(live)) {
        return false;
    }
    error = request.empty()
        ? L"O Winamp não enviou a requisição HTTP da transmissão a tempo."
        : L"A requisição HTTP da transmissão foi interrompida.";
    return false;
}

static bool StreamLive(LiveContext* live, std::wstring& error) {
    SOCKET client = INVALID_SOCKET;
    if (!AcceptLiveClient(live, client, error)) {
        return error.empty() ? false : true;
    }

    std::string request;
    if (!ReadHttpRequest(client, live, request, error)) {
        closesocket(client);
        return error.empty() ? false : true;
    }

    static const char headers[] =
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: audio/mpeg\r\n"
        "Cache-Control: no-cache, no-store\r\n"
        "Pragma: no-cache\r\n"
        "Connection: close\r\n"
        "icy-name: YouTube no Winamp\r\n"
        "\r\n";
    if (!SendAllSocket(client, headers, static_cast<int>(sizeof(headers) - 1))) {
        closesocket(client);
        return true;
    }

    std::vector<char> buffer(32768);
    for (;;) {
        if (IsLiveStopping(live)) {
            closesocket(client);
            return false;
        }

        DWORD available = 0;
        if (!PeekNamedPipe(
                live->audioPipe,
                nullptr,
                0,
                nullptr,
                &available,
                nullptr)) {
            if (IsLiveStopping(live)) {
                closesocket(client);
                return false;
            }
            DWORD code = GetLastError();
            if (code != ERROR_BROKEN_PIPE && code != ERROR_INVALID_HANDLE) {
                error = L"Falha ao ler o áudio da transmissão: " +
                        FormatWin32Error(code);
            }
            closesocket(client);
            return true;
        }

        if (available > 0) {
            DWORD toRead = available;
            if (toRead > buffer.size()) {
                toRead = static_cast<DWORD>(buffer.size());
            }
            DWORD read = 0;
            if (!ReadFile(
                    live->audioPipe,
                    buffer.data(),
                    toRead,
                    &read,
                    nullptr)) {
                error = L"Falha ao ler o áudio da transmissão: " +
                        FormatWin32Error(GetLastError());
                closesocket(client);
                return true;
            }
            if (read > 0 &&
                !SendAllSocket(client, buffer.data(), static_cast<int>(read))) {
                closesocket(client);
                return true;
            }
        } else {
            if (live->ffmpegProcess != nullptr &&
                WaitForSingleObject(live->ffmpegProcess, 0) == WAIT_OBJECT_0) {
                closesocket(client);
                return true;
            }
            WaitForSingleObject(live->stopEvent, 20);
        }
    }
}

static DWORD WINAPI LiveWorkerThread(LPVOID parameter) {
    LiveContext* live = static_cast<LiveContext*>(parameter);
    std::wstring error;

    if (!RunLivePipeline(live, error)) {
        TerminateLiveProcesses(live);
        CloseLiveResources(live);
        if (!IsLiveStopping(live)) {
            PostLiveEvent(live->dialog, kLiveFailedMessage, live->id, L"", error);
        }
        return 0;
    }

    if (IsLiveStopping(live)) {
        TerminateLiveProcesses(live);
        CloseLiveResources(live);
        return 0;
    }

    std::wstring streamUrl =
        L"http://127.0.0.1:" + std::to_wstring(live->port) + L"/live.mp3";
    if (!PostLiveEvent(live->dialog, kLiveReadyMessage, live->id, streamUrl, L"")) {
        TerminateLiveProcesses(live);
        CloseLiveResources(live);
        return 0;
    }

    bool ended = StreamLive(live, error);
    bool stopping = IsLiveStopping(live);
    TerminateLiveProcesses(live);
    CloseLiveResources(live);
    if (ended && !stopping) {
        PostLiveEvent(g_winamp, kLiveEndedMessage, live->id, L"", error);
    }
    return 0;
}

static bool StopLiveNow(bool waitIndefinitely = false) {
    if (g_live == nullptr) {
        g_liveRunning = false;
        g_liveStarting = false;
        return true;
    }

    LiveContext* live = g_live;
    if (live->stopEvent != nullptr) {
        SetEvent(live->stopEvent);
    }
    TerminateLiveProcesses(live);

    EnterCriticalSection(&live->lock);
    SOCKET listener = live->listener;
    live->listener = INVALID_SOCKET;
    LeaveCriticalSection(&live->lock);
    if (listener != INVALID_SOCKET) {
        closesocket(listener);
    }

    DWORD wait = WaitForSingleObject(
        live->thread, waitIndefinitely ? INFINITE : 15000);
    if (wait != WAIT_OBJECT_0) {
        return false;
    }

    CloseLiveResources(live);
    CloseHandleIfValid(live->stopEvent);
    CloseHandleIfValid(live->thread);
    DeleteCriticalSection(&live->lock);
    delete live;
    g_live = nullptr;
    g_liveRunning = false;
    g_liveStarting = false;
    return true;
}

static void StartJob(
    HWND dialog,
    const std::wstring& url,
    const std::wstring& ytdlp,
    bool enqueue) {
    if (g_jobRunning) {
        return;
    }

    g_jobRunning = true;
    g_jobEnqueue = enqueue;
    if (g_jobStopEvent != nullptr) {
        ResetEvent(g_jobStopEvent);
    }
    UpdateDialogControls(dialog);
    SetStatus(
        dialog,
        IsPlaylistUrl(url)
            ? L"Baixando a playlist do YouTube... Isso pode demorar."
            : L"Baixando o áudio do YouTube...");

    JobContext* job = new JobContext();
    job->dialog = dialog;
    job->url = url;
    job->ytdlp = ytdlp;

    HANDLE thread = CreateThread(nullptr, 0, WorkerThread, job, 0, nullptr);
    if (thread == nullptr) {
        DWORD code = GetLastError();
        delete job;
        g_jobRunning = false;
        UpdateDialogControls(dialog);
        SetStatus(dialog, L"Falha ao iniciar o download.");
        MessageBoxW(
            dialog,
            (L"Não foi possível iniciar o download.\n\n" + FormatWin32Error(code)).c_str(),
            kPluginName,
            MB_OK | MB_ICONERROR);
        return;
    }
    g_jobThread = thread;
}

static INT_PTR CALLBACK SettingsDialogProc(
    HWND settingsDialog, UINT message, WPARAM wParam, LPARAM lParam) {
    SettingsValues* settings = reinterpret_cast<SettingsValues*>(
        GetWindowLongPtrW(settingsDialog, DWLP_USER));

    switch (message) {
    case WM_INITDIALOG:
        settings = reinterpret_cast<SettingsValues*>(lParam);
        SetWindowLongPtrW(
            settingsDialog, DWLP_USER, reinterpret_cast<LONG_PTR>(settings));
        SetDlgItemTextW(settingsDialog, IDC_YTDLP, settings->ytdlp.c_str());
        SetDlgItemTextW(settingsDialog, IDC_FFMPEG, settings->ffmpeg.c_str());
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_YTDLP_BROWSE: {
            std::wstring selected;
            if (PickExecutable(
                    settingsDialog,
                    GetDialogText(settingsDialog, IDC_YTDLP),
                    selected)) {
                SetDlgItemTextW(settingsDialog, IDC_YTDLP, selected.c_str());
            }
            return TRUE;
        }
        case IDC_FFMPEG_BROWSE: {
            std::wstring selected;
            if (PickExecutable(
                    settingsDialog,
                    GetDialogText(settingsDialog, IDC_FFMPEG),
                    selected)) {
                SetDlgItemTextW(settingsDialog, IDC_FFMPEG, selected.c_str());
            }
            return TRUE;
        }
        case IDC_AUTODETECT: {
            std::wstring ytdlp = FindTool(
                L"yt-dlp.exe",
                settings != nullptr ? settings->ytdlp : std::wstring());
            std::wstring ffmpeg = FindTool(
                L"ffmpeg.exe",
                settings != nullptr ? settings->ffmpeg : std::wstring());
            if (ytdlp.empty()) {
                ytdlp = FindTool(L"yt-dlp.exe", L"");
            }
            if (ffmpeg.empty()) {
                ffmpeg = FindFfmpeg(ytdlp);
            }
            SetDlgItemTextW(settingsDialog, IDC_YTDLP, ytdlp.c_str());
            SetDlgItemTextW(settingsDialog, IDC_FFMPEG, ffmpeg.c_str());
            if (ytdlp.empty() && ffmpeg.empty()) {
                MessageBoxW(
                    settingsDialog,
                    L"Nenhum executável foi encontrado automaticamente.",
                    kPluginName,
                    MB_OK | MB_ICONINFORMATION);
            }
            return TRUE;
        }
        case IDC_CACHE_CLEAR: {
            if (MessageBoxW(
                    settingsDialog,
                    L"Apagar todos os áudios baixados em cache?",
                    kPluginName,
                    MB_YESNO | MB_ICONQUESTION) == IDYES) {
                DeleteTree(GetCacheRoot());
                MessageBoxW(
                    settingsDialog,
                    L"Cache limpo.",
                    kPluginName,
                    MB_OK | MB_ICONINFORMATION);
            }
            return TRUE;
        }
        case IDOK: {
            std::wstring ytdlp = Trim(GetDialogText(settingsDialog, IDC_YTDLP));
            std::wstring ffmpeg = Trim(GetDialogText(settingsDialog, IDC_FFMPEG));
            if (!ytdlp.empty() && !FileExists(ytdlp)) {
                MessageBoxW(
                    settingsDialog,
                    L"O caminho informado para o yt-dlp.exe não existe.",
                    kPluginName,
                    MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            if (!ffmpeg.empty() && !FileExists(ffmpeg)) {
                MessageBoxW(
                    settingsDialog,
                    L"O caminho informado para o ffmpeg.exe não existe.",
                    kPluginName,
                    MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            if (settings != nullptr) {
                settings->ytdlp = ytdlp;
                settings->ffmpeg = ffmpeg;
            }
            WriteSetting(L"ytdlp", ytdlp);
            WriteSetting(L"ffmpeg", ffmpeg);
            EndDialog(settingsDialog, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(settingsDialog, IDCANCEL);
            return TRUE;
        }
        break;

    case WM_CLOSE:
        EndDialog(settingsDialog, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

static void ShowSettings(HWND dialog) {
    SettingsValues values;
    values.ytdlp = ReadSetting(L"ytdlp");
    values.ffmpeg = ReadSetting(L"ffmpeg");
    if (values.ytdlp.empty()) {
        values.ytdlp = FindTool(L"yt-dlp.exe", L"");
    }
    if (values.ffmpeg.empty()) {
        values.ffmpeg = FindFfmpeg(values.ytdlp);
    }

    DialogBoxParamW(
        g_instance,
        MAKEINTRESOURCEW(IDD_SETTINGS),
        dialog,
        SettingsDialogProc,
        reinterpret_cast<LPARAM>(&values));
}

static bool GetDialogUrl(HWND dialog, std::wstring& url) {
    url = NormalizeUrl(GetDialogText(dialog, IDC_URL));
    if (!IsYouTubeUrl(url)) {
        MessageBoxW(
            dialog,
            L"Informe um link válido do YouTube.\n\n"
            L"Exemplos: https://www.youtube.com/watch?v=... ou https://youtu.be/...",
            kPluginName,
            MB_OK | MB_ICONWARNING);
        SetFocus(GetDlgItem(dialog, IDC_URL));
        return false;
    }
    return true;
}

static bool EnsureYtdlp(HWND dialog, std::wstring& ytdlp) {
    std::wstring configured = ReadSetting(L"ytdlp");
    ytdlp = FindTool(L"yt-dlp.exe", configured);
    if (!ytdlp.empty()) {
        return true;
    }

    if (MessageBoxW(
            dialog,
            L"yt-dlp.exe não foi encontrado.\n\n"
            L"Baixe em github.com/yt-dlp/yt-dlp/releases e coloque na pasta do Winamp, "
            L"no PATH ou informe o caminho agora.\n\nDeseja localizar o yt-dlp.exe?",
            kPluginName,
            MB_YESNO | MB_ICONQUESTION) != IDYES) {
        ShellExecuteW(
            dialog,
            L"open",
            L"https://github.com/yt-dlp/yt-dlp/releases/latest",
            nullptr,
            nullptr,
            SW_SHOWNORMAL);
        return false;
    }

    std::wstring selected;
    if (!PickExecutable(dialog, configured, selected)) {
        return false;
    }
    ytdlp = selected;
    WriteSetting(L"ytdlp", ytdlp);
    return true;
}

static void StartJobFromDialog(HWND dialog, bool enqueue) {
    if (g_jobRunning || g_liveStarting) {
        return;
    }

    std::wstring url;
    if (!GetDialogUrl(dialog, url)) {
        return;
    }

    std::wstring ytdlp;
    if (!EnsureYtdlp(dialog, ytdlp)) {
        return;
    }

    SetDlgItemTextW(dialog, IDC_URL, url.c_str());
    StartJob(dialog, url, ytdlp, enqueue);
}

static void StartLive(
    HWND dialog,
    const std::wstring& url,
    const std::wstring& ytdlp,
    const std::wstring& ffmpeg) {
    if (g_live != nullptr || g_liveRunning) {
        return;
    }

    std::wstring cacheRoot = GetCacheRoot();
    if (cacheRoot.empty()) {
        MessageBoxW(
            dialog,
            L"Não foi possível obter a pasta temporária do Windows.",
            kPluginName,
            MB_OK | MB_ICONERROR);
        return;
    }

    LiveContext* live = new LiveContext();
    InitializeCriticalSection(&live->lock);
    live->id = ++g_liveId;
    live->dialog = dialog;
    live->url = url;
    live->ytdlp = ytdlp;
    live->ffmpeg = ffmpeg;
    live->workDir = JoinPath(cacheRoot, L"live-" + HashUrl(url));
    live->ytdlpLog = JoinPath(live->workDir, L"yt-dlp.log");
    live->ffmpegLog = JoinPath(live->workDir, L"ffmpeg.log");
    live->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (live->stopEvent == nullptr) {
        DWORD code = GetLastError();
        DeleteCriticalSection(&live->lock);
        delete live;
        MessageBoxW(
            dialog,
            (L"Não foi possível preparar a transmissão ao vivo.\n\n" +
             FormatWin32Error(code)).c_str(),
            kPluginName,
            MB_OK | MB_ICONERROR);
        return;
    }

    live->thread = CreateThread(nullptr, 0, LiveWorkerThread, live, 0, nullptr);
    if (live->thread == nullptr) {
        DWORD code = GetLastError();
        CloseHandle(live->stopEvent);
        DeleteCriticalSection(&live->lock);
        delete live;
        MessageBoxW(
            dialog,
            (L"Não foi possível iniciar a transmissão ao vivo.\n\n" +
             FormatWin32Error(code)).c_str(),
            kPluginName,
            MB_OK | MB_ICONERROR);
        return;
    }

    g_live = live;
    g_liveStarting = true;
    g_liveRunning = true;
    UpdateDialogControls(dialog);
    SetStatus(dialog, L"Conectando à transmissão ao vivo...");
}

static void StartLiveFromDialog(HWND dialog) {
    if (g_jobRunning || g_liveStarting) {
        return;
    }
    if (g_liveRunning) {
        MessageBoxW(
            dialog,
            L"Já existe uma transmissão ao vivo em andamento.\n\n"
            L"Pare a transmissão atual antes de iniciar outra.",
            kPluginName,
            MB_OK | MB_ICONINFORMATION);
        return;
    }

    std::wstring url;
    if (!GetDialogUrl(dialog, url)) {
        return;
    }

    std::wstring ytdlp;
    if (!EnsureYtdlp(dialog, ytdlp)) {
        return;
    }

    std::wstring ffmpeg = FindFfmpeg(ytdlp);
    if (ffmpeg.empty()) {
        if (MessageBoxW(
                dialog,
                L"A transmissão ao vivo requer o ffmpeg.exe para converter o áudio em tempo real.\n\n"
                L"Deseja abrir as Configurações para localizá-lo?",
                kPluginName,
                MB_YESNO | MB_ICONQUESTION) == IDYES) {
            ShowSettings(dialog);
        }
        return;
    }

    SetDlgItemTextW(dialog, IDC_URL, url.c_str());
    StartLive(dialog, url, ytdlp, ffmpeg);
}

static INT_PTR CALLBACK MainDialogProc(
    HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_INITDIALOG: {
        g_mainDialog = dialog;
        UpdateDialogControls(dialog);

        std::wstring ytdlp = FindTool(L"yt-dlp.exe", ReadSetting(L"ytdlp"));
        if (g_liveStarting) {
            SetStatus(dialog, L"Conectando à transmissão ao vivo...");
        } else if (g_liveRunning) {
            SetStatus(dialog, L"Transmissão ao vivo ativa.");
        } else if (ytdlp.empty()) {
            SetStatus(
                dialog,
                L"yt-dlp.exe não encontrado. Use Configurações para localizá-lo.");
        } else {
            SetStatus(
                dialog,
                L"Cole um link do YouTube e escolha Tocar agora ou Adicionar à playlist.");
        }

        if (lParam != 0) {
            std::wstring clipboard = NormalizeUrl(ReadClipboardText());
            if (IsYouTubeUrl(clipboard)) {
                SetDlgItemTextW(dialog, IDC_URL, clipboard.c_str());
            }
        }
        SetFocus(GetDlgItem(dialog, IDC_URL));
        return FALSE;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_PASTE:
            PasteClipboard(dialog);
            return TRUE;
        case IDC_PLAY:
            StartJobFromDialog(dialog, false);
            return TRUE;
        case IDC_ENQUEUE:
            StartJobFromDialog(dialog, true);
            return TRUE;
        case IDC_LIVE:
            StartLiveFromDialog(dialog);
            return TRUE;
        case IDC_STOP_LIVE: {
            if (!g_liveRunning) {
                SetStatus(dialog, L"Nenhuma transmissão ao vivo em andamento.");
                return TRUE;
            }
            SetStatus(dialog, L"Parando a transmissão ao vivo...");
            bool stopped = StopLiveNow();
            UpdateDialogControls(dialog);
            SetStatus(
                dialog,
                stopped
                    ? L"Transmissão ao vivo parada."
                    : L"Não foi possível encerrar a transmissão. Tente novamente.");
            return TRUE;
        }
        case IDC_SETTINGS_BTN:
            ShowSettings(dialog);
            return TRUE;
        case IDCANCEL:
            if (g_jobRunning || g_liveStarting) {
                MessageBoxW(
                    dialog,
                    g_jobRunning
                        ? L"Aguarde o download terminar."
                        : L"Aguarde a transmissão ao vivo conectar.",
                    kPluginName,
                    MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        break;

    case WM_CLOSE:
        if (g_jobRunning || g_liveStarting) {
            MessageBoxW(
                dialog,
                g_jobRunning
                    ? L"Aguarde o download terminar."
                    : L"Aguarde a transmissão ao vivo conectar.",
                kPluginName,
                MB_OK | MB_ICONINFORMATION);
            return TRUE;
        }
        EndDialog(dialog, 0);
        return TRUE;

    case kJobDoneMessage: {
        JobResult* result = reinterpret_cast<JobResult*>(lParam);
        CloseHandleIfValid(g_jobThread);
        if (!g_jobRunning) {
            delete result;
            return TRUE;
        }

        g_jobRunning = false;
        if (result->ok) {
            if (!g_jobEnqueue && g_liveRunning) {
                StopLiveNow();
            }
            SendToWinamp(result->entries, g_jobEnqueue);
            EndDialog(dialog, 0);
        } else {
            UpdateDialogControls(dialog);
            SetStatus(dialog, L"Falha ao baixar o áudio. Veja a mensagem.");
            MessageBoxW(
                dialog,
                result->error.c_str(),
                kPluginName,
                MB_OK | MB_ICONERROR);
        }
        delete result;
        return TRUE;
    }

    case kLiveReadyMessage: {
        LiveEvent* event = reinterpret_cast<LiveEvent*>(lParam);
        if (event->id != g_liveId || g_live == nullptr || !g_liveStarting) {
            delete event;
            return TRUE;
        }
        g_liveStarting = false;
        SendToWinamp(event->url, L"YouTube ao vivo", false);
        delete event;
        EndDialog(dialog, 0);
        return TRUE;
    }

    case kLiveFailedMessage: {
        LiveEvent* event = reinterpret_cast<LiveEvent*>(lParam);
        if (event->id != g_liveId || g_live == nullptr) {
            delete event;
            return TRUE;
        }
        std::wstring error = event->error;
        delete event;
        StopLiveNow();
        UpdateDialogControls(dialog);
        SetStatus(dialog, L"Falha ao iniciar a transmissão. Veja a mensagem.");
        MessageBoxW(
            dialog,
            error.c_str(),
            kPluginName,
            MB_OK | MB_ICONERROR);
        return TRUE;
    }

    case WM_DESTROY:
        if (g_mainDialog == dialog) {
            g_mainDialog = nullptr;
        }
        break;
    }
    return FALSE;
}

static void ShowMainDialog(HWND parent, bool useClipboard) {
    if (g_mainDialog != nullptr && IsWindow(g_mainDialog)) {
        SetForegroundWindow(g_mainDialog);
        return;
    }
    DialogBoxParamW(
        g_instance,
        MAKEINTRESOURCEW(IDD_MAIN),
        parent,
        MainDialogProc,
        useClipboard ? 1 : 0);
}

static LRESULT CALLBACK WinampWindowProc(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_COMMAND &&
        HIWORD(wParam) == 0 &&
        LOWORD(wParam) == ID_MENU_OPEN) {
        ShowMainDialog(window, true);
        return 0;
    }
    if (message == WM_COMMAND &&
        HIWORD(wParam) == 0 &&
        LOWORD(wParam) == ID_MENU_STOP_LIVE) {
        if (!g_liveRunning) {
            MessageBoxW(
                window,
                L"Nenhuma transmissão ao vivo em andamento.",
                kPluginName,
                MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        bool stopped = StopLiveNow();
        if (g_mainDialog != nullptr) {
            UpdateDialogControls(g_mainDialog);
            SetStatus(
                g_mainDialog,
                stopped
                    ? L"Transmissão ao vivo parada."
                    : L"Não foi possível encerrar a transmissão. Tente novamente.");
        }
        return 0;
    }
    if (message == WM_HOTKEY && wParam == ID_HOTKEY) {
        ShowMainDialog(window, true);
        return 0;
    }
    if (message == kLiveEndedMessage) {
        LiveEvent* event = reinterpret_cast<LiveEvent*>(lParam);
        if (event->id == g_liveId && g_live != nullptr) {
            std::wstring error = event->error;
            StopLiveNow();
            if (g_mainDialog != nullptr) {
                UpdateDialogControls(g_mainDialog);
                SetStatus(
                    g_mainDialog,
                    error.empty()
                        ? L"Transmissão ao vivo encerrada."
                        : L"A transmissão ao vivo foi encerrada.");
            }
            if (!error.empty()) {
                MessageBoxW(
                    g_mainDialog != nullptr ? g_mainDialog : window,
                    error.c_str(),
                    kPluginName,
                    MB_OK | MB_ICONERROR);
            }
        }
        delete event;
        return 0;
    }
    return CallWindowProcW(g_oldWndProc, window, message, wParam, lParam);
}

static void AddMenuItems() {
    if (g_winamp == nullptr) {
        return;
    }
    HMENU fileMenu = reinterpret_cast<HMENU>(
        SendMessageW(g_winamp, WM_WA_IPC, 1, IPC_GET_HMENU));
    if (fileMenu != nullptr) {
        AppendMenuW(
            fileMenu,
            MF_STRING,
            ID_MENU_OPEN,
            L"Abrir link do YouTube...\tCtrl+Alt+Y");
        AppendMenuW(
            fileMenu,
            MF_STRING,
            ID_MENU_STOP_LIVE,
            L"Parar transmissão ao vivo");
    }
    HMENU popupMenu = reinterpret_cast<HMENU>(
        SendMessageW(g_winamp, WM_WA_IPC, 0, IPC_GET_HMENU));
    if (popupMenu != nullptr && popupMenu != fileMenu) {
        AppendMenuW(
            popupMenu,
            MF_STRING,
            ID_MENU_OPEN,
            L"Abrir link do YouTube...\tCtrl+Alt+Y");
        AppendMenuW(
            popupMenu,
            MF_STRING,
            ID_MENU_STOP_LIVE,
            L"Parar transmissão ao vivo");
    }
}

static void RemoveMenuItems() {
    if (g_winamp == nullptr) {
        return;
    }
    HMENU fileMenu = reinterpret_cast<HMENU>(
        SendMessageW(g_winamp, WM_WA_IPC, 1, IPC_GET_HMENU));
    if (fileMenu != nullptr) {
        RemoveMenu(fileMenu, ID_MENU_OPEN, MF_BYCOMMAND);
        RemoveMenu(fileMenu, ID_MENU_STOP_LIVE, MF_BYCOMMAND);
    }
    HMENU popupMenu = reinterpret_cast<HMENU>(
        SendMessageW(g_winamp, WM_WA_IPC, 0, IPC_GET_HMENU));
    if (popupMenu != nullptr && popupMenu != fileMenu) {
        RemoveMenu(popupMenu, ID_MENU_OPEN, MF_BYCOMMAND);
        RemoveMenu(popupMenu, ID_MENU_STOP_LIVE, MF_BYCOMMAND);
    }
}

static int Init() {
    g_instance = g_plugin.hDllInstance;
    g_winamp = g_plugin.hwndParent;

    std::wstring modulePath = GetModulePath(g_instance);
    g_pluginDir = DirectoryOf(modulePath);
    if (g_pluginDir.empty()) {
        g_pluginDir = GetCacheRoot();
    }

    char* iniDirectory = reinterpret_cast<char*>(
        SendMessageW(g_winamp, WM_WA_IPC, 0, IPC_GETINIDIRECTORY));
    std::wstring iniFolder = iniDirectory != nullptr
        ? AnsiToWide(iniDirectory)
        : std::wstring();
    if (iniFolder.empty()) {
        std::wstring appData = GetEnvironmentValue(L"APPDATA");
        if (!appData.empty()) {
            iniFolder = JoinPath(appData, L"Winamp");
        }
    }
    if (iniFolder.empty()) {
        iniFolder = g_pluginDir;
    }
    g_iniPath = JoinPath(iniFolder, L"gen_youtube.ini");

    WSADATA winsockData{};
    if (WSAStartup(MAKEWORD(2, 2), &winsockData) == 0) {
        g_winsockReady = true;
    }
    g_jobStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    if (g_winamp != nullptr) {
        g_oldWndProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(
                g_winamp,
                GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(WinampWindowProc)));
        if (g_oldWndProc != nullptr) {
            AddMenuItems();
        }
        RegisterHotKey(g_winamp, ID_HOTKEY, MOD_CONTROL | MOD_ALT, 'Y');
    }

    return GEN_INIT_SUCCESS;
}

static void Config() {
    ShowMainDialog(g_winamp, true);
}

static void Quit() {
    if (g_jobStopEvent != nullptr) {
        SetEvent(g_jobStopEvent);
    }
    if (g_jobThread != nullptr) {
        if (WaitForSingleObject(g_jobThread, 30000) == WAIT_OBJECT_0) {
            CloseHandleIfValid(g_jobStopEvent);
        }
        CloseHandleIfValid(g_jobThread);
        g_jobRunning = false;
    } else {
        CloseHandleIfValid(g_jobStopEvent);
    }

    StopLiveNow(true);
    if (g_winsockReady) {
        WSACleanup();
        g_winsockReady = false;
    }
    if (g_winamp != nullptr) {
        UnregisterHotKey(g_winamp, ID_HOTKEY);
        RemoveMenuItems();
        if (g_oldWndProc != nullptr) {
            SetWindowLongPtrW(
                g_winamp,
                GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(g_oldWndProc));
            g_oldWndProc = nullptr;
        }
    }
}
