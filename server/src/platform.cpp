// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

constexpr wchar_t kServiceName[] = L"IdRegistryService";
constexpr wchar_t kServiceDisplay[] = L"ID Registry";
constexpr wchar_t kEventSource[] = L"IdRegistryService";

std::wstring utf8_to_wide(const std::string& text) {
#ifdef _WIN32
    if (text.empty()) {
        return {};
    }
    int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring wide(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
#else
    std::wstring wide;
    wide.reserve(text.size());
    for (unsigned char c : text) {
        wide.push_back(static_cast<wchar_t>(c));
    }
    return wide;
#endif
}

std::string wide_to_utf8(const std::wstring& text) {
#ifdef _WIN32
    if (text.empty()) {
        return {};
    }
    int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string utf8(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), size, nullptr, nullptr);
    return utf8;
#else
    std::string utf8;
    utf8.reserve(text.size());
    for (wchar_t c : text) {
        utf8.push_back(static_cast<char>(c));
    }
    return utf8;
#endif
}

std::wstring executable_path() {
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(length);
    return buffer;
#else
    return {};
#endif
}

class TeeBuf : public std::streambuf {
public:
    TeeBuf(std::mutex& mutex, std::ofstream& file, std::streambuf* console)
        : mutex_(mutex), file_(file), console_(console) {}

protected:
    int_type overflow(int_type ch) override {
        if (traits_type::eq_int_type(ch, traits_type::eof())) {
            return traits_type::not_eof(ch);
        }
        char c = traits_type::to_char_type(ch);
        write_bytes(&c, 1);
        return ch;
    }

    std::streamsize xsputn(const char* s, std::streamsize count) override {
        if (count > 0) {
            write_bytes(s, count);
        }
        return count;
    }

    int sync() override {
        std::lock_guard<std::mutex> lock(mutex_);
        file_.flush();
        if (console_ != nullptr) {
            console_->pubsync();
        }
        return 0;
    }

private:
    void write_bytes(const char* s, std::streamsize count) {
        std::lock_guard<std::mutex> lock(mutex_);
        file_.write(s, count);
        if (console_ != nullptr) {
            console_->sputn(s, count);
        }
        for (std::streamsize i = 0; i < count; ++i) {
            if (s[i] == '\n') {
                file_.flush();
                if (console_ != nullptr) {
                    console_->pubsync();
                }
                break;
            }
        }
    }

    std::mutex& mutex_;
    std::ofstream& file_;
    std::streambuf* console_;
};

struct LogState {
    std::mutex mutex;
    std::ofstream file;
    std::unique_ptr<std::ofstream> console;
    std::streambuf* old_cout = nullptr;
    std::streambuf* old_cerr = nullptr;
    std::unique_ptr<TeeBuf> cout_buf;
    std::unique_ptr<TeeBuf> cerr_buf;

    ~LogState() {
        if (old_cout != nullptr) {
            std::cout.rdbuf(old_cout);
        }
        if (old_cerr != nullptr) {
            std::cerr.rdbuf(old_cerr);
        }
    }
};

LogState* g_log = nullptr;

void write_parent_console(const std::string& text) {
    std::string line = text;
    if (line.empty() || line.back() != '\n') {
        line.push_back('\n');
    }
    // A console launch already owns stdout. Attaching again fails and drops the message.
    std::cout << line;
    std::cout.flush();
#ifdef _WIN32
    HANDLE existing = GetStdHandle(STD_OUTPUT_HANDLE);
    if (existing != nullptr && existing != INVALID_HANDLE_VALUE) {
        return;
    }
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        return;
    }
    HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        if (!WriteConsoleA(handle, line.data(), static_cast<DWORD>(line.size()), &written, nullptr)) {
            WriteFile(handle, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
        }
    }
    FreeConsole();
#endif
}

#ifdef _WIN32

SERVICE_STATUS_HANDLE g_status_handle = nullptr;
SERVICE_STATUS g_status{};
void (*g_stop_handler)() = nullptr;
int (*g_run_server)(const Options&, bool) = nullptr;

void report_status(DWORD state, DWORD exit_code, DWORD wait_hint) {
    static DWORD checkpoint = 1;
    if (g_status_handle == nullptr) {
        return;
    }

    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwWaitHint = wait_hint;
    g_status.dwWin32ExitCode = (state == SERVICE_STOPPED && exit_code != 0) ? ERROR_SERVICE_SPECIFIC_ERROR : 0;
    g_status.dwServiceSpecificExitCode = (state == SERVICE_STOPPED) ? static_cast<DWORD>(exit_code) : 0;

    if (state == SERVICE_RUNNING) {
        g_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
        g_status.dwCheckPoint = 0;
    } else if (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING) {
        g_status.dwControlsAccepted = 0;
        g_status.dwCheckPoint = checkpoint++;
    } else {
        g_status.dwControlsAccepted = 0;
        g_status.dwCheckPoint = 0;
    }

    SetServiceStatus(g_status_handle, &g_status);
}

DWORD WINAPI service_control_handler(DWORD control, DWORD, LPVOID, LPVOID) {
    switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        report_status(SERVICE_STOP_PENDING, 0, 30000);
        if (g_stop_handler != nullptr) {
            g_stop_handler();
        }
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        report_status(g_status.dwCurrentState, 0, 0);
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

void report_event(WORD type, const std::string& message) {
    HANDLE source = RegisterEventSourceW(nullptr, kEventSource);
    if (source == nullptr) {
        return;
    }
    std::wstring wide = utf8_to_wide(message);
    LPCWSTR strings[1] = {wide.c_str()};
    // Event ID 0 has no message template, so Event Viewer shows a blank description.
    ReportEventW(source, type, 0, 1, nullptr, 1, 0, strings, nullptr);
    DeregisterEventSource(source);
}

int exit_from_win32(DWORD error) {
    if (error == ERROR_ACCESS_DENIED) {
        return 5;
    }
    if (error == ERROR_SERVICE_DOES_NOT_EXIST) {
        return 1060;
    }
    return 2;
}

std::wstring quote_arg(const std::wstring& value) {
    std::wstring quoted = L"\"";
    for (wchar_t c : value) {
        if (c == L'"') {
            quoted += L'\\';
        }
        quoted += c;
    }
    quoted += L'"';
    return quoted;
}

std::wstring service_command(const Options& opt) {
    std::wstring command = quote_arg(executable_path());
    command += L" --db ";
    command += quote_arg(utf8_to_wide(opt.db_path));
    command += L" --port ";
    command += std::to_wstring(opt.port);
    command += L" --host ";
    command += quote_arg(utf8_to_wide(opt.host));
    return command;
}

bool wait_for_state(SC_HANDLE service, DWORD wanted, DWORD timeout_ms) {
    ULONGLONG deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        SERVICE_STATUS_PROCESS status{};
        DWORD bytes = 0;
        if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                                  reinterpret_cast<LPBYTE>(&status), sizeof(status), &bytes)) {
            return false;
        }
        if (status.dwCurrentState == wanted) {
            return true;
        }
        if (wanted == SERVICE_RUNNING && status.dwCurrentState == SERVICE_STOPPED) {
            return false;
        }
        if (GetTickCount64() >= deadline) {
            return false;
        }
        Sleep(200);
    }
}

void register_event_source() {
    HKEY key = nullptr;
    LONG result = RegCreateKeyExW(
        HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Services\\EventLog\\Application\\IdRegistryService",
        0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (result != ERROR_SUCCESS) {
        return;
    }
    DWORD types = EVENTLOG_ERROR_TYPE | EVENTLOG_WARNING_TYPE | EVENTLOG_INFORMATION_TYPE;
    RegSetValueExW(key, L"TypesSupported", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&types), sizeof(types));
    RegCloseKey(key);
}

void unregister_event_source() {
    RegDeleteKeyW(HKEY_LOCAL_MACHINE,
                  L"SYSTEM\\CurrentControlSet\\Services\\EventLog\\Application\\IdRegistryService");
}

void configure_service(SC_HANDLE service) {
    SERVICE_DESCRIPTIONW description{};
    description.lpDescription = const_cast<LPWSTR>(L"ID Registry HTTP service");
    ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &description);

    SC_ACTION action{};
    action.Type = SC_ACTION_RESTART;
    action.Delay = 5000;
    SERVICE_FAILURE_ACTIONSW failure{};
    failure.dwResetPeriod = 24 * 60 * 60;
    failure.cActions = 1;
    failure.lpsaActions = &action;
    ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, &failure);
}

int install_service(const Options& opt) {
    std::wstring command = service_command(opt);
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (manager == nullptr) {
        DWORD error = GetLastError();
        std::string message = "Could not open the Service Control Manager (error " + std::to_string(error) + ").";
        log_event_error(message);
        write_parent_console(message);
        return exit_from_win32(error);
    }

    SC_HANDLE service = CreateServiceW(
        manager,
        kServiceName,
        kServiceDisplay,
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        command.c_str(),
        nullptr, nullptr, nullptr, nullptr, nullptr);

    bool updated = false;
    if (service == nullptr) {
        DWORD error = GetLastError();
        if (error != ERROR_SERVICE_EXISTS) {
            std::string message = "Could not create the service (error " + std::to_string(error) + ").";
            log_event_error(message);
            write_parent_console(message);
            CloseServiceHandle(manager);
            return exit_from_win32(error);
        }
        service = OpenServiceW(manager, kServiceName, SERVICE_CHANGE_CONFIG | SERVICE_QUERY_STATUS);
        if (service == nullptr) {
            error = GetLastError();
            std::string message = "The service already exists but could not be opened (error " + std::to_string(error) + ").";
            log_event_error(message);
            write_parent_console(message);
            CloseServiceHandle(manager);
            return exit_from_win32(error);
        }
        if (!ChangeServiceConfigW(service, SERVICE_NO_CHANGE, SERVICE_NO_CHANGE, SERVICE_NO_CHANGE,
                                  command.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr)) {
            error = GetLastError();
            std::string message = "Could not update the service command (error " + std::to_string(error) + ").";
            log_event_error(message);
            write_parent_console(message);
            CloseServiceHandle(service);
            CloseServiceHandle(manager);
            return exit_from_win32(error);
        }
        updated = true;
    }

    configure_service(service);
    register_event_source();

    SERVICE_STATUS_PROCESS status{};
    DWORD bytes = 0;
    bool running = QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                                         reinterpret_cast<LPBYTE>(&status), sizeof(status), &bytes)
                   && status.dwCurrentState != SERVICE_STOPPED;

    CloseServiceHandle(service);
    CloseServiceHandle(manager);

    std::string message = updated ? "ID Registry service command updated." : "ID Registry service installed.";
    if (updated && running) {
        message += " Restart the service to use the new database path.";
    }
    log_event_info(message);
    write_parent_console(message);
    return 0;
}

int uninstall_service() {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) {
        DWORD error = GetLastError();
        std::string message = "Could not open the Service Control Manager (error " + std::to_string(error) + ").";
        log_event_error(message);
        write_parent_console(message);
        return exit_from_win32(error);
    }

    SC_HANDLE service = OpenServiceW(manager, kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (service == nullptr) {
        DWORD error = GetLastError();
        std::string message = error == ERROR_SERVICE_DOES_NOT_EXIST
            ? "The ID Registry service is not installed."
            : "Could not open the service (error " + std::to_string(error) + ").";
        log_event_error(message);
        write_parent_console(message);
        CloseServiceHandle(manager);
        return exit_from_win32(error);
    }

    SERVICE_STATUS_PROCESS status{};
    DWORD bytes = 0;
    if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                             reinterpret_cast<LPBYTE>(&status), sizeof(status), &bytes)
        && status.dwCurrentState != SERVICE_STOPPED) {
        SERVICE_STATUS control{};
        if (!ControlService(service, SERVICE_CONTROL_STOP, &control)) {
            DWORD error = GetLastError();
            if (error != ERROR_SERVICE_NOT_ACTIVE) {
                std::string message = "Could not stop the service (error " + std::to_string(error) + ").";
                log_event_error(message);
                write_parent_console(message);
                CloseServiceHandle(service);
                CloseServiceHandle(manager);
                return exit_from_win32(error);
            }
        } else if (!wait_for_state(service, SERVICE_STOPPED, 30000)) {
            std::string message = "The service did not stop.";
            log_event_error(message);
            write_parent_console(message);
            CloseServiceHandle(service);
            CloseServiceHandle(manager);
            return 2;
        }
    }

    if (!DeleteService(service)) {
        DWORD error = GetLastError();
        std::string message = "Could not delete the service (error " + std::to_string(error) + ").";
        log_event_error(message);
        write_parent_console(message);
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        return exit_from_win32(error);
    }

    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    unregister_event_source();
    std::string message = "ID Registry service removed. The database file was left in place.";
    log_event_info(message);
    write_parent_console(message);
    return 0;
}

int start_service() {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) {
        DWORD error = GetLastError();
        std::string message = "Could not open the Service Control Manager (error " + std::to_string(error) + ").";
        log_event_error(message);
        write_parent_console(message);
        return exit_from_win32(error);
    }

    SC_HANDLE service = OpenServiceW(manager, kServiceName, SERVICE_START | SERVICE_QUERY_STATUS);
    if (service == nullptr) {
        DWORD error = GetLastError();
        std::string message = error == ERROR_SERVICE_DOES_NOT_EXIST
            ? "The ID Registry service is not installed."
            : "Could not open the service (error " + std::to_string(error) + ").";
        log_event_error(message);
        write_parent_console(message);
        CloseServiceHandle(manager);
        return exit_from_win32(error);
    }

    if (!StartServiceW(service, 0, nullptr)) {
        DWORD error = GetLastError();
        if (error != ERROR_SERVICE_ALREADY_RUNNING) {
            std::string message = "Could not start the service (error " + std::to_string(error) + ").";
            log_event_error(message);
            write_parent_console(message);
            CloseServiceHandle(service);
            CloseServiceHandle(manager);
            return exit_from_win32(error);
        }
    }

    if (!wait_for_state(service, SERVICE_RUNNING, 30000)) {
        std::string message = "The service did not reach the running state. See logs\\idRegistry.log in the database directory and the Windows Application event log.";
        log_event_error(message);
        write_parent_console(message);
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        return 2;
    }

    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    write_parent_console("ID Registry service is running.");
    return 0;
}

int stop_service() {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) {
        DWORD error = GetLastError();
        std::string message = "Could not open the Service Control Manager (error " + std::to_string(error) + ").";
        log_event_error(message);
        write_parent_console(message);
        return exit_from_win32(error);
    }

    SC_HANDLE service = OpenServiceW(manager, kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (service == nullptr) {
        DWORD error = GetLastError();
        std::string message = error == ERROR_SERVICE_DOES_NOT_EXIST
            ? "The ID Registry service is not installed."
            : "Could not open the service (error " + std::to_string(error) + ").";
        log_event_error(message);
        write_parent_console(message);
        CloseServiceHandle(manager);
        return exit_from_win32(error);
    }

    SERVICE_STATUS_PROCESS status{};
    DWORD bytes = 0;
    if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                             reinterpret_cast<LPBYTE>(&status), sizeof(status), &bytes)
        && status.dwCurrentState == SERVICE_STOPPED) {
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        write_parent_console("ID Registry service is already stopped.");
        return 0;
    }

    SERVICE_STATUS control{};
    if (!ControlService(service, SERVICE_CONTROL_STOP, &control)) {
        DWORD error = GetLastError();
        std::string message = "Could not stop the service (error " + std::to_string(error) + ").";
        log_event_error(message);
        write_parent_console(message);
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        return exit_from_win32(error);
    }
    if (!wait_for_state(service, SERVICE_STOPPED, 30000)) {
        std::string message = "The service did not stop.";
        log_event_error(message);
        write_parent_console(message);
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        return 2;
    }

    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    write_parent_console("ID Registry service stopped.");
    return 0;
}

std::vector<std::string> command_line_args();

void WINAPI service_main(DWORD, LPWSTR*) {
    g_status_handle = RegisterServiceCtrlHandlerExW(kServiceName, service_control_handler, nullptr);
    if (g_status_handle == nullptr) {
        log_event_error("Could not register the service control handler.");
        return;
    }
    report_status(SERVICE_START_PENDING, 0, 20000);

    // The SCM starts this process with the full ImagePath, but ServiceMain receives only the service name.
    std::vector<std::string> owned = command_line_args();
    std::vector<char*> args;
    args.reserve(owned.size());
    for (std::string& arg : owned) {
        args.push_back(arg.data());
    }

    Options opt;
    std::string error;
    if (!parse_options(static_cast<int>(args.size()), args.data(), opt, error) || opt.db_path.empty()) {
        std::string message = error.empty() ? "The service command is missing --db <path>." : error;
        log_event_error(message);
        report_status(SERVICE_STOPPED, 1, 0);
        return;
    }
    opt.db_path = normalize_db_path(opt.db_path);

    int code = g_run_server(opt, true);
    if (g_status.dwCurrentState != SERVICE_STOPPED) {
        report_status(SERVICE_STOPPED, code == 0 ? 0 : code, 0);
    }
}

bool start_service_dispatcher() {
    SERVICE_TABLE_ENTRYW table[] = {
        {const_cast<LPWSTR>(kServiceName), service_main},
        {nullptr, nullptr}
    };
    if (StartServiceCtrlDispatcherW(table)) {
        return true;
    }
    return GetLastError() != ERROR_FAILED_SERVICE_CONTROLLER_CONNECT;
}

std::vector<std::string> command_line_args() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    if (argv == nullptr) {
        return args;
    }
    args.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        args.push_back(wide_to_utf8(argv[i]));
    }
    LocalFree(argv);
    return args;
}

#endif // _WIN32

} // namespace

std::string usage_text() {
    return
        "Usage: idRegistryServer --db <path> [options]\n"
        "       idRegistryServer --install --db <path> [options]\n"
        "       idRegistryServer --uninstall\n"
        "       idRegistryServer --start\n"
        "       idRegistryServer --stop\n"
        "\n"
        "Options:\n"
        "  --db <path>          SQLite database file. Required to run or install.\n"
        "  -p, --port <port>    Port to listen on (default: 8000)\n"
        "  -h, --host <host>    Host interface to bind (default: 0.0.0.0)\n"
        "  --install            Register the auto-start LocalSystem service\n"
        "  --uninstall          Stop and remove the service\n"
        "  --start              Start the service\n"
        "  --stop               Stop the service\n"
        "  --help               Show this help\n"
        "\n"
        "Logs are written to logs\\idRegistry.log in the database directory.\n";
}

bool parse_options(int argc, char** argv, Options& opt, std::string& error) {
    int commands = 0;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i] == nullptr ? "" : argv[i];
        if (arg == "--help") {
            opt.action = Options::Action::Help;
            return true;
        } else if (arg == "--install") {
            opt.action = Options::Action::Install;
            ++commands;
        } else if (arg == "--uninstall") {
            opt.action = Options::Action::Uninstall;
            ++commands;
        } else if (arg == "--start") {
            opt.action = Options::Action::Start;
            ++commands;
        } else if (arg == "--stop") {
            opt.action = Options::Action::Stop;
            ++commands;
        } else if (arg == "--db") {
            if (i + 1 >= argc) {
                error = "Missing value for --db.";
                return false;
            }
            opt.db_path = argv[++i];
            if (opt.db_path.empty()) {
                error = "The --db path is empty.";
                return false;
            }
        } else if (arg == "-p" || arg == "--port") {
            if (i + 1 >= argc) {
                error = "Missing value for --port.";
                return false;
            }
            try {
                int port = std::stoi(argv[++i]);
                if (port < 1 || port > 65535) {
                    error = "Port must be between 1 and 65535.";
                    return false;
                }
                opt.port = port;
            } catch (...) {
                error = "Invalid port.";
                return false;
            }
        } else if (arg == "-h" || arg == "--host") {
            if (i + 1 >= argc) {
                error = "Missing value for --host.";
                return false;
            }
            opt.host = argv[++i];
            if (opt.host.empty()) {
                error = "The host is empty.";
                return false;
            }
        } else if (!arg.empty() && arg[0] != '-') {
            try {
                int port = std::stoi(arg);
                if (port < 1 || port > 65535) {
                    error = "Port must be between 1 and 65535.";
                    return false;
                }
                opt.port = port;
            } catch (...) {
                error = "Unknown argument: " + arg;
                return false;
            }
        } else {
            error = "Unknown argument: " + arg;
            return false;
        }
    }

    if (commands > 1) {
        error = "Use only one of --install, --uninstall, --start, or --stop.";
        return false;
    }
    if ((opt.action == Options::Action::Run || opt.action == Options::Action::Install) && opt.db_path.empty()) {
        error = "Missing --db <path>.";
        return false;
    }
    return true;
}

std::string normalize_db_path(const std::string& utf8_path) {
#ifdef _WIN32
    std::filesystem::path path(utf8_to_wide(utf8_path));
    if (path.is_relative()) {
        std::filesystem::path exe(executable_path());
        path = exe.parent_path() / path;
    }
    path = path.lexically_normal();
    return wide_to_utf8(path.wstring());
#else
    std::filesystem::path path(utf8_path);
    if (path.is_relative()) {
        path = std::filesystem::current_path() / path;
    }
    return path.lexically_normal().string();
#endif
}

bool init_log(const std::string& db_path_utf8, std::string& log_path_utf8, std::string& error) {
    if (g_log != nullptr) {
        return true;
    }

    try {
#ifdef _WIN32
        std::filesystem::path db_path(utf8_to_wide(db_path_utf8));
#else
        std::filesystem::path db_path(db_path_utf8);
#endif
        std::filesystem::path log_path = db_path.parent_path() / "logs" / "idRegistry.log";
        std::error_code ec;
        std::filesystem::create_directories(log_path.parent_path(), ec);
        if (ec) {
#ifdef _WIN32
            log_path_utf8 = wide_to_utf8(log_path.parent_path().wstring());
#else
            log_path_utf8 = log_path.parent_path().string();
#endif
            error = "Could not create log directory " + log_path_utf8 + ": " + ec.message();
            return false;
        }

        auto state = std::make_unique<LogState>();
        state->file.open(log_path, std::ios::out | std::ios::app);
        if (!state->file) {
#ifdef _WIN32
            log_path_utf8 = wide_to_utf8(log_path.wstring());
#else
            log_path_utf8 = log_path.string();
#endif
            error = "Could not open log file " + log_path_utf8;
            return false;
        }

#ifdef _WIN32
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            auto console = std::make_unique<std::ofstream>("CONOUT$");
            if (*console) {
                state->console = std::move(console);
            } else {
                FreeConsole();
            }
        }
#endif

        std::streambuf* console_buf = state->console ? state->console->rdbuf() : nullptr;
        state->old_cout = std::cout.rdbuf();
        state->old_cerr = std::cerr.rdbuf();
        state->cout_buf = std::make_unique<TeeBuf>(state->mutex, state->file, console_buf);
        state->cerr_buf = std::make_unique<TeeBuf>(state->mutex, state->file, console_buf);
        std::cout.rdbuf(state->cout_buf.get());
        std::cerr.rdbuf(state->cerr_buf.get());
        g_log = state.release();

#ifdef _WIN32
        log_path_utf8 = wide_to_utf8(log_path.wstring());
#else
        log_path_utf8 = log_path.string();
#endif
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

void log_event_error(const std::string& message) {
#ifdef _WIN32
    report_event(EVENTLOG_ERROR_TYPE, message);
#else
    std::cerr << message << std::endl;
#endif
}

void log_event_info(const std::string& message) {
#ifdef _WIN32
    report_event(EVENTLOG_INFORMATION_TYPE, message);
#else
    std::cout << message << std::endl;
#endif
}

void service_report_start_pending() {
#ifdef _WIN32
    report_status(SERVICE_START_PENDING, 0, 20000);
#else
#endif
}

void service_report_running() {
#ifdef _WIN32
    report_status(SERVICE_RUNNING, 0, 0);
#endif
}

void service_report_stopped(int exit_code) {
#ifdef _WIN32
    report_status(SERVICE_STOPPED, exit_code, 0);
#else
    (void)exit_code;
#endif
}

void set_service_stop_handler(void (*fn)()) {
#ifdef _WIN32
    g_stop_handler = fn;
#else
    (void)fn;
#endif
}

int entry_point(int argc, char** argv, int (*run_server)(const Options&, bool)) {
#ifdef _WIN32
    (void)argc;
    (void)argv;
    g_run_server = run_server;
    std::vector<std::string> owned = command_line_args();
    std::vector<char*> args;
    args.reserve(owned.size());
    for (std::string& arg : owned) {
        args.push_back(arg.data());
    }

    Options opt;
    std::string error;
    bool parsed = parse_options(static_cast<int>(args.size()), args.empty() ? nullptr : args.data(), opt, error);
    if (parsed && opt.action == Options::Action::Help) {
        write_parent_console(usage_text());
        return 0;
    }
    if (parsed && opt.action != Options::Action::Run) {
        if (opt.action == Options::Action::Install) {
            opt.db_path = normalize_db_path(opt.db_path);
            return install_service(opt);
        }
        if (opt.action == Options::Action::Uninstall) {
            return uninstall_service();
        }
        if (opt.action == Options::Action::Start) {
            return start_service();
        }
        return stop_service();
    }

    // A service start must connect to the SCM before any console work, or the start times out.
    if (start_service_dispatcher()) {
        return 0;
    }

    if (!parsed) {
        write_parent_console(error + "\n\n" + usage_text());
        return 1;
    }
    opt.db_path = normalize_db_path(opt.db_path);
    return run_server(opt, false);
#else
    Options opt;
    std::string error;
    if (!parse_options(argc, argv, opt, error)) {
        std::cerr << error << "\n\n" << usage_text();
        return 1;
    }
    if (opt.action == Options::Action::Help) {
        std::cout << usage_text();
        return 0;
    }
    if (opt.action != Options::Action::Run) {
        std::cerr << "Service install and control are available on Windows only.\n";
        return 2;
    }
    opt.db_path = normalize_db_path(opt.db_path);
    return run_server(opt, false);
#endif
}
