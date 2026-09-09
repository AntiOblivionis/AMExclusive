#include <windows.h>

#include <conio.h>
#include <fcntl.h>
#include <io.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "SetupResources.h"

namespace {

namespace fs = std::filesystem;

enum class Operation {
    Install,
    Uninstall,
};

struct ResourceSpec {
    int id;
    const wchar_t* fileName;
};

constexpr ResourceSpec kResources[] = {
    {AMMOD_RESOURCE_BROKER, L"am-exclusive-broker.exe"},
    {AMMOD_RESOURCE_MEDIA_CONTROL, L"am_exclusive_media_control.exe"},
    {AMMOD_RESOURCE_HOOK, L"am-exclusive-hook.dll"},
    {AMMOD_RESOURCE_UI, L"am-exclusive-ui.dll"},
    {AMMOD_RESOURCE_INI, L"am-exclusive.ini"},
    {AMMOD_RESOURCE_INSTALL, L"Install-Mod.ps1"},
    {AMMOD_RESOURCE_UNINSTALL, L"Uninstall-Mod.ps1"},
    {AMMOD_RESOURCE_REGISTER_COMPANION, L"Register-Companion.ps1"},
    {AMMOD_RESOURCE_UNINSTALL_CMD, L"Uninstall.cmd"},
    {AMMOD_RESOURCE_LICENSE, L"LICENSE"},
    {AMMOD_RESOURCE_THIRD_PARTY_NOTICES, L"THIRD_PARTY_NOTICES.md"},
};

struct ProcessResult {
    DWORD exitCode = 1;
    std::string output;
    bool started = false;
};

void ConfigureConsole() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    DWORD mode = 0;
    const bool stdoutIsConsole = GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode) != FALSE;
    const bool stderrIsConsole = GetConsoleMode(GetStdHandle(STD_ERROR_HANDLE), &mode) != FALSE;
    const bool stdinIsConsole = GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) != FALSE;
    _setmode(_fileno(stdout), stdoutIsConsole ? _O_U16TEXT : _O_U8TEXT);
    _setmode(_fileno(stderr), stderrIsConsole ? _O_U16TEXT : _O_U8TEXT);
    if (stdinIsConsole) {
        _setmode(_fileno(stdin), _O_U16TEXT);
    }
}

bool IsChineseLanguageTag(std::wstring_view tag) {
    return tag.size() >= 2 &&
           (tag[0] == L'z' || tag[0] == L'Z') &&
           (tag[1] == L'h' || tag[1] == L'H') &&
           (tag.size() == 2 || tag[2] == L'-');
}

bool UseChineseUi() {
    static const bool chinese = [] {
        // Windows writes a pending preferred UI language before sign-out. Use
        // it first so Setup follows a display-language change immediately,
        // matching packaged apps that can already switch in a fresh process.
        DWORD valueType = 0;
        DWORD valueBytes = 0;
        if (RegGetValueW(HKEY_CURRENT_USER,
                         L"Control Panel\\Desktop",
                         L"PreferredUILanguagesPending",
                         RRF_RT_REG_MULTI_SZ | RRF_RT_REG_SZ,
                         &valueType,
                         nullptr,
                         &valueBytes) == ERROR_SUCCESS &&
            valueBytes >= sizeof(wchar_t) * 2) {
            std::vector<wchar_t> pending(valueBytes / sizeof(wchar_t) + 1, L'\0');
            if (RegGetValueW(HKEY_CURRENT_USER,
                             L"Control Panel\\Desktop",
                             L"PreferredUILanguagesPending",
                             RRF_RT_REG_MULTI_SZ | RRF_RT_REG_SZ,
                             &valueType,
                             pending.data(),
                             &valueBytes) == ERROR_SUCCESS &&
                pending[0] != L'\0') {
                return IsChineseLanguageTag(pending.data());
            }
        }

        ULONG languageCount = 0;
        ULONG bufferLength = 0;
        if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME,
                                        &languageCount,
                                        nullptr,
                                        &bufferLength) &&
            bufferLength > 1) {
            std::vector<wchar_t> languages(bufferLength, L'\0');
            if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME,
                                            &languageCount,
                                            languages.data(),
                                            &bufferLength) &&
                languages[0] != L'\0') {
                return IsChineseLanguageTag(languages.data());
            }
        }
        const LANGID language = GetUserDefaultUILanguage();
        return PRIMARYLANGID(language) == LANG_CHINESE;
    }();
    return chinese;
}

std::wstring Localized(std::wstring_view chinese, std::wstring_view english) {
    return std::wstring(UseChineseUi() ? chinese : english);
}

std::wstring Win32Error(DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                        FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;
    const LANGID messageLanguage = UseChineseUi()
                                       ? 0
                                       : MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
    DWORD length = FormatMessageW(
        flags,
        nullptr,
        error,
        messageLanguage,
        reinterpret_cast<wchar_t*>(&buffer),
        0,
        nullptr);
    if (length == 0 && buffer == nullptr) {
        length = FormatMessageW(
            flags,
            nullptr,
            error,
            0,
            reinterpret_cast<wchar_t*>(&buffer),
            0,
            nullptr);
    }
    std::wstring message = length != 0 ? std::wstring(buffer, length)
                                       : Localized(L"未知 Win32 错误", L"Unknown Win32 error");
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n')) {
        message.pop_back();
    }
    return message;
}

std::wstring DecodeBytes(const std::string& bytes, UINT codePage, DWORD flags) {
    if (bytes.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(
        codePage,
        flags,
        bytes.data(),
        static_cast<int>(bytes.size()),
        nullptr,
        0);
    if (length <= 0) {
        return {};
    }
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(
        codePage,
        flags,
        bytes.data(),
        static_cast<int>(bytes.size()),
        result.data(),
        length);
    return result;
}

std::string EncodeUtf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (length <= 0) {
        return {};
    }
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        result.data(),
        length,
        nullptr,
        nullptr);
    return result;
}

std::wstring DecodeCapturedOutput(const std::string& bytes) {
    if (bytes.size() >= 2) {
        size_t zeroOddBytes = 0;
        for (size_t index = 1; index < bytes.size(); index += 2) {
            if (bytes[index] == '\0') {
                ++zeroOddBytes;
            }
        }
        if (zeroOddBytes * 4 >= bytes.size()) {
            const size_t wcharCount = bytes.size() / sizeof(wchar_t);
            std::wstring decoded(wcharCount, L'\0');
            std::memcpy(decoded.data(), bytes.data(), wcharCount * sizeof(wchar_t));
            return decoded;
        }
    }

    std::wstring decoded = DecodeBytes(bytes, CP_UTF8, MB_ERR_INVALID_CHARS);
    if (!decoded.empty() || bytes.empty()) {
        return decoded;
    }
    decoded = DecodeBytes(bytes, CP_ACP, 0);
    if (!decoded.empty()) {
        return decoded;
    }
    decoded = DecodeBytes(bytes, GetOEMCP(), 0);
    return decoded.empty()
               ? Localized(L"(无法解码 PowerShell 输出)", L"(Unable to decode PowerShell output)")
               : decoded;
}

bool CreateUniqueTempDirectory(fs::path& directory, std::wstring& error) {
    wchar_t tempPath[MAX_PATH] = {};
    const DWORD pathLength = GetTempPathW(static_cast<DWORD>(std::size(tempPath)), tempPath);
    if (pathLength == 0 || pathLength >= std::size(tempPath)) {
        error = Localized(L"无法取得 Windows 临时目录: ", L"Unable to get the Windows temporary directory: ") +
                Win32Error(GetLastError());
        return false;
    }

    wchar_t uniquePath[MAX_PATH] = {};
    if (GetTempFileNameW(tempPath, L"AMM", 0, uniquePath) == 0) {
        error = Localized(L"无法创建唯一临时路径: ", L"Unable to create a unique temporary path: ") +
                Win32Error(GetLastError());
        return false;
    }
    if (!DeleteFileW(uniquePath)) {
        error = Localized(L"无法准备临时目录: ", L"Unable to prepare the temporary directory: ") +
                Win32Error(GetLastError());
        return false;
    }
    if (!CreateDirectoryW(uniquePath, nullptr)) {
        error = Localized(L"无法创建临时目录: ", L"Unable to create the temporary directory: ") +
                Win32Error(GetLastError());
        return false;
    }
    directory = fs::path(uniquePath);
    return true;
}

void RemoveTempDirectory(const fs::path& directory) noexcept {
    if (directory.empty()) {
        return;
    }
    std::error_code error;
    fs::remove_all(directory, error);
}

bool ExtractResource(const ResourceSpec& resource,
                     const fs::path& directory,
                     std::wstring& error) {
    const HRSRC resourceHandle = FindResourceW(
        nullptr,
        MAKEINTRESOURCEW(resource.id),
        RT_RCDATA);
    if (resourceHandle == nullptr) {
        error = Localized(L"找不到嵌入资源 ", L"Embedded resource not found: ") +
                std::wstring(resource.fileName) + L": " + Win32Error(GetLastError());
        return false;
    }

    const HGLOBAL loaded = LoadResource(nullptr, resourceHandle);
    const DWORD size = SizeofResource(nullptr, resourceHandle);
    const void* data = loaded != nullptr ? LockResource(loaded) : nullptr;
    if (loaded == nullptr || data == nullptr || size == 0) {
        error = Localized(L"无法读取嵌入资源 ", L"Unable to read embedded resource: ") +
                std::wstring(resource.fileName);
        return false;
    }

    const fs::path destination = directory / resource.fileName;
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = Localized(L"无法写出临时文件 ", L"Unable to write temporary file: ") +
                destination.wstring();
        return false;
    }
    output.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    output.close();
    if (!output || fs::file_size(destination) != size) {
        error = Localized(L"嵌入资源写出不完整 ", L"Embedded resource was not written completely: ") +
                destination.wstring();
        return false;
    }
    return true;
}

bool ExtractAllResources(const fs::path& directory, std::wstring& error) {
    for (const ResourceSpec& resource : kResources) {
        if (!ExtractResource(resource, directory, error)) {
            return false;
        }
    }
    return true;
}

std::wstring QuoteArgument(std::wstring_view value) {
    std::wstring quoted;
    quoted.push_back(L'"');
    size_t backslashes = 0;
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(character);
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::wstring GetPowerShellPath() {
    wchar_t windowsDirectory[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(
        L"WINDIR",
        windowsDirectory,
        static_cast<DWORD>(std::size(windowsDirectory)));
    if (length != 0 && length < std::size(windowsDirectory)) {
        return (fs::path(windowsDirectory) /
                L"System32\\WindowsPowerShell\\v1.0\\powershell.exe").wstring();
    }
    return L"powershell.exe";
}

std::wstring StatusText(Operation operation, std::string_view token) {
    if (token == "extract_resources") {
        return Localized(L"正在提取自包含安装资源", L"Extracting self-contained installer resources");
    }
    if (token == "launch_script") {
        return operation == Operation::Install
                   ? Localized(L"正在启动安装事务", L"Starting the installation transaction")
                   : Localized(L"正在启动卸载事务", L"Starting the uninstall transaction");
    }
    if (token == "validate") {
        return Localized(L"正在校验 Apple Music 与发布内容", L"Validating Apple Music and the release payload");
    }
    if (token == "validated") {
        return Localized(L"校验通过，准备更新运行环境", L"Validation passed; preparing the runtime update");
    }
    if (token == "stop_runtime") {
        return Localized(L"正在停止 Apple Music 运行时以释放模块", L"Stopping the Apple Music runtime to release loaded modules");
    }
    if (token == "secure_install") {
        return Localized(L"正在设置安装目录权限", L"Securing the installation directory");
    }
    if (token == "copy_payload") {
        return Localized(L"正在复制自包含运行时文件", L"Copying self-contained runtime files");
    }
    if (token == "write_manifest") {
        return Localized(L"正在写入安装清单", L"Writing the installation manifest");
    }
    if (token == "register_companion") {
        return Localized(L"正在注册 Apple Music 伴生任务", L"Registering the Apple Music companion task");
    }
    if (token == "remove_registration") {
        return Localized(L"正在清理计划任务与卸载项", L"Removing scheduled-task and uninstall registrations");
    }
    if (token == "remove_directory") {
        return Localized(L"正在删除安装目录", L"Removing the installation directory");
    }
    if (token == "restart_apple_music") {
        return Localized(L"正在按原状态重新启动 Apple Music", L"Restoring the previous Apple Music running state");
    }
    if (token == "complete") {
        return operation == Operation::Install
                   ? Localized(L"安装/升级完成", L"Installation/upgrade complete")
                   : Localized(L"卸载完成", L"Uninstall complete");
    }
    return DecodeBytes(std::string(token), CP_UTF8, MB_ERR_INVALID_CHARS);
}

void RenderProgress(Operation operation, int percent, std::string_view token) {
    percent = std::clamp(percent, 0, 100);
    constexpr int width = 36;
    const int filled = (percent * width) / 100;
    std::wcout << L'\r' << L"[";
    for (int index = 0; index < width; ++index) {
        std::wcout << (index < filled ? L'#' : L'-');
    }
    std::wcout << L"] " << std::setw(3) << percent << L"% "
               << StatusText(operation, token) << L"   " << std::flush;
}

bool TryParseProgress(const std::string& line,
                      Operation operation,
                      bool& parsed) {
    constexpr std::string_view prefix = "AMMOD_PROGRESS|";
    if (!line.starts_with(prefix)) {
        parsed = false;
        return true;
    }

    const size_t percentStart = prefix.size();
    const size_t separator = line.find('|', percentStart);
    if (separator == std::string::npos || separator == percentStart ||
        separator + 1 >= line.size()) {
        parsed = true;
        return false;
    }
    int percent = 0;
    try {
        percent = std::stoi(line.substr(percentStart, separator - percentStart));
    } catch (...) {
        parsed = true;
        return false;
    }
    RenderProgress(operation, percent, line.substr(separator + 1));
    parsed = true;
    return true;
}

void ConsumeOutput(const std::string& chunk,
                   Operation operation,
                   std::string& pending,
                   std::string& nonProtocolOutput) {
    pending += chunk;
    while (true) {
        const size_t newline = pending.find('\n');
        if (newline == std::string::npos) {
            break;
        }
        std::string line = pending.substr(0, newline);
        pending.erase(0, newline + 1);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        bool parsed = false;
        if (!TryParseProgress(line, operation, parsed) || !parsed) {
            nonProtocolOutput += line;
            nonProtocolOutput += '\n';
        }
    }
}

ProcessResult RunPowerShell(Operation operation, const fs::path& directory) {
    ProcessResult result;
    SECURITY_ATTRIBUTES securityAttributes{};
    securityAttributes.nLength = sizeof(securityAttributes);
    securityAttributes.bInheritHandle = TRUE;

    HANDLE readHandle = nullptr;
    HANDLE writeHandle = nullptr;
    if (!CreatePipe(&readHandle, &writeHandle, &securityAttributes, 0)) {
        const std::wstring error = Win32Error(GetLastError());
        result.output = EncodeUtf8(
            Localized(L"无法创建 PowerShell 输出管道: ", L"Unable to create the PowerShell output pipe: ") +
            error);
        return result;
    }
    SetHandleInformation(readHandle, HANDLE_FLAG_INHERIT, 0);

    const fs::path script = directory /
        (operation == Operation::Install ? L"Install-Mod.ps1" : L"Uninstall-Mod.ps1");
    std::wstring command = QuoteArgument(GetPowerShellPath()) +
        L" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " +
        QuoteArgument(script.wstring());
    if (operation == Operation::Install) {
        command += L" -SourceDirectory " + QuoteArgument(directory.wstring());
    }
    command += L" -EmitProgress";
    std::vector<wchar_t> commandLine(command.begin(), command.end());
    commandLine.push_back(L'\0');

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startupInfo.hStdOutput = writeHandle;
    startupInfo.hStdError = writeHandle;
    PROCESS_INFORMATION processInfo{};
    const BOOL created = CreateProcessW(
        GetPowerShellPath().c_str(),
        commandLine.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_UNICODE_ENVIRONMENT,
        nullptr,
        nullptr,
        &startupInfo,
        &processInfo);
    CloseHandle(writeHandle);
    if (!created) {
        result.output = EncodeUtf8(Localized(L"启动 PowerShell 失败。", L"Failed to start PowerShell."));
        CloseHandle(readHandle);
        return result;
    }
    result.started = true;

    std::string pending;
    char buffer[4096];
    while (true) {
        DWORD bytesRead = 0;
        const BOOL read = ReadFile(readHandle, buffer, sizeof(buffer), &bytesRead, nullptr);
        if (!read || bytesRead == 0) {
            break;
        }
        ConsumeOutput(std::string(buffer, bytesRead), operation, pending, result.output);
    }
    CloseHandle(readHandle);
    if (!pending.empty()) {
        bool parsed = false;
        if (!TryParseProgress(pending, operation, parsed) || !parsed) {
            result.output += pending;
        }
    }

    WaitForSingleObject(processInfo.hProcess, INFINITE);
    GetExitCodeProcess(processInfo.hProcess, &result.exitCode);
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return result;
}

void PrintCapturedFailure(const std::string& output) {
    std::wcout << L"\n\n"
               << Localized(L"---- PowerShell 详细错误输出 ----\n",
                            L"---- Detailed PowerShell error output ----\n");
    const std::wstring decoded = DecodeCapturedOutput(output);
    std::wcout << decoded;
    if (decoded.empty() || decoded.back() != L'\n') {
        std::wcout << L'\n';
    }
    std::wcout << Localized(L"---- 错误输出结束 ----\n", L"---- End of error output ----\n");
}

void WaitForAnyKey() {
    std::wcout << L"\n\n" << Localized(L"按任意键退出...", L"Press any key to exit...") << std::flush;
    _getwch();
    std::wcout << L"\n";
}

int RunExtractTest() {
    fs::path directory;
    std::wstring error;
    if (!CreateUniqueTempDirectory(directory, error)) {
        std::wcerr << Localized(L"extract-test 失败: ", L"extract-test failed: ") << error << L'\n';
        return 1;
    }
    const bool extracted = ExtractAllResources(directory, error);
    RemoveTempDirectory(directory);
    if (!extracted) {
        std::wcerr << Localized(L"extract-test 失败: ", L"extract-test failed: ") << error << L'\n';
        return 1;
    }
    if (UseChineseUi()) {
        std::wcout << L"extract-test 通过：已校验并清理 "
                   << std::size(kResources) << L" 个嵌入资源。\n";
    } else {
        std::wcout << L"extract-test passed: verified and cleaned "
                   << std::size(kResources) << L" embedded resources.\n";
    }
    return 0;
}

int RunOperation(Operation operation) {
    fs::path directory;
    std::wstring error;
    if (!CreateUniqueTempDirectory(directory, error)) {
        std::wcerr << Localized(L"准备临时目录失败: ", L"Failed to prepare the temporary directory: ")
                   << error << L'\n';
        return 1;
    }

    std::wcout << (operation == Operation::Install
                       ? Localized(L"正在准备安装/升级...\n", L"Preparing installation/upgrade...\n")
                       : Localized(L"正在准备卸载...\n", L"Preparing uninstall...\n"));
    RenderProgress(operation, 2, "extract_resources");
    if (!ExtractAllResources(directory, error)) {
        RemoveTempDirectory(directory);
        std::wcerr << Localized(L"提取自包含资源失败: ", L"Failed to extract self-contained resources: ")
                   << error << L'\n';
        return 1;
    }

    RenderProgress(operation, 10, "launch_script");
    const ProcessResult result = RunPowerShell(operation, directory);
    RemoveTempDirectory(directory);
    if (result.exitCode == 0) {
        RenderProgress(operation, 100, "complete");
        std::wcout << L"\n"
                   << (operation == Operation::Install
                           ? Localized(L"安装/升级成功。", L"Installation/upgrade succeeded.")
                           : Localized(L"卸载成功。", L"Uninstall succeeded."))
                   << L'\n';
        return 0;
    }

    std::wcout << L"\n"
               << (result.started
                       ? Localized(L"操作失败。", L"Operation failed.")
                       : Localized(L"无法启动安装脚本。", L"Unable to start the installer script."))
               << Localized(L" 退出码: ", L" Exit code: ") << result.exitCode << L'\n';
    PrintCapturedFailure(result.output);
    return result.exitCode == 0 ? 1 : static_cast<int>(result.exitCode);
}

void PrintMenu() {
    if (UseChineseUi()) {
        std::wcout << L"\nAMExclusive 安装器\n"
                   << L"------------------\n"
                   << L"1. 安装/升级\n"
                   << L"2. 卸载\n"
                   << L"0. 退出\n"
                   << L"请选择: " << std::flush;
    } else {
        std::wcout << L"\nAMExclusive Setup\n"
                   << L"-----------------\n"
                   << L"1. Install/Upgrade\n"
                   << L"2. Uninstall\n"
                   << L"0. Exit\n"
                   << L"Select: " << std::flush;
    }
}

int RunMenu() {
    while (true) {
        PrintMenu();
        std::wstring choice;
        if (!std::getline(std::wcin, choice)) {
            return 1;
        }
        if (choice == L"0") {
            return 0;
        }
        if (choice == L"1" || choice == L"2") {
            const Operation operation = choice == L"1" ? Operation::Install
                                                        : Operation::Uninstall;
            const int result = RunOperation(operation);
            WaitForAnyKey();
            return result;
        }
        std::wcout << Localized(L"请输入 1、2 或 0。\n", L"Enter 1, 2, or 0.\n");
    }
}

void PrintHelp() {
    std::wcout << L"AMExclusive-Setup.exe\n\n";
    if (UseChineseUi()) {
        std::wcout << L"用法:\n"
                   << L"  AMExclusive-Setup.exe       显示菜单\n"
                   << L"  AMExclusive-Setup.exe --install\n"
                   << L"  AMExclusive-Setup.exe --uninstall\n"
                   << L"  AMExclusive-Setup.exe --extract-test\n"
                   << L"  AMExclusive-Setup.exe --help\n\n"
                   << L"--extract-test 只验证嵌入资源并清理临时目录，不安装、不卸载，也不启动 Apple Music。\n";
    } else {
        std::wcout << L"Usage:\n"
                   << L"  AMExclusive-Setup.exe       Show the menu\n"
                   << L"  AMExclusive-Setup.exe --install\n"
                   << L"  AMExclusive-Setup.exe --uninstall\n"
                   << L"  AMExclusive-Setup.exe --extract-test\n"
                   << L"  AMExclusive-Setup.exe --help\n\n"
                   << L"--extract-test only verifies the embedded resources and removes its temporary files; it does not install, uninstall, or start Apple Music.\n";
    }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    ConfigureConsole();
    if (argc == 1) {
        return RunMenu();
    }
    if (argc != 2) {
        PrintHelp();
        return 2;
    }
    const std::wstring argument = argv[1];
    if (argument == L"--help" || argument == L"-h" || argument == L"/?") {
        PrintHelp();
        return 0;
    }
    if (argument == L"--extract-test") {
        return RunExtractTest();
    }
    if (argument == L"--install") {
        const int result = RunOperation(Operation::Install);
        WaitForAnyKey();
        return result;
    }
    if (argument == L"--uninstall") {
        const int result = RunOperation(Operation::Uninstall);
        WaitForAnyKey();
        return result;
    }
    PrintHelp();
    return 2;
}
