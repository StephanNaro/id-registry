// SPDX-License-Identifier: GPL-3.0-or-later
#include "service.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

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

#ifdef _WIN32

QString wide_to_qstring(const wchar_t* text) {
    return text == nullptr ? QString() : QString::fromWCharArray(text);
}

QString win32_error(DWORD error) {
    wchar_t* buffer = nullptr;
    DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    QString message = length == 0 ? QString("Windows error %1").arg(error)
                                  : QString::fromWCharArray(buffer).trimmed();
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    return message;
}

QStringList split_command(const QString& command) {
    std::wstring wide = command.toStdWString();
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(wide.c_str(), &argc);
    QStringList args;
    if (argv == nullptr) {
        return args;
    }
    args.reserve(argc);
    for (int i = 0; i < argc; ++i) {
        args.append(QString::fromWCharArray(argv[i]));
    }
    LocalFree(argv);
    return args;
}

void read_service_command(const QStringList& args, ServiceInfo& info) {
    for (int i = 1; i < args.size(); ++i) {
        const QString& arg = args.at(i);
        if (arg == "--db" && i + 1 < args.size()) {
            info.db_path = args.at(++i);
        } else if ((arg == "--port" || arg == "-p") && i + 1 < args.size()) {
            bool ok = false;
            int port = args.at(++i).toInt(&ok);
            if (ok) {
                info.port = port;
            }
        } else if ((arg == "--host" || arg == "-h") && i + 1 < args.size()) {
            info.host = args.at(++i);
        }
    }
}

#endif

QString server_executable() {
    return QDir(QCoreApplication::applicationDirPath()).filePath("idRegistryServer.exe");
}

} // namespace

ServiceInfo query_service() {
    ServiceInfo info;
#ifndef _WIN32
    info.error = "Windows services are not available on this platform.";
    return info;
#else
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) {
        info.error = "Could not open the Service Control Manager: " + win32_error(GetLastError());
        return info;
    }

    SC_HANDLE service = OpenServiceW(manager, kServiceName, SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG);
    if (service == nullptr) {
        DWORD error = GetLastError();
        CloseServiceHandle(manager);
        if (error != ERROR_SERVICE_DOES_NOT_EXIST) {
            info.error = "Could not query the service: " + win32_error(error);
        }
        return info;
    }

    info.installed = true;

    SERVICE_STATUS_PROCESS status{};
    DWORD bytes = 0;
    if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                             reinterpret_cast<LPBYTE>(&status), sizeof(status), &bytes)) {
        info.running = status.dwCurrentState == SERVICE_RUNNING
                    || status.dwCurrentState == SERVICE_START_PENDING;
    }

    DWORD needed = 0;
    QueryServiceConfigW(service, nullptr, 0, &needed);
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && needed > 0) {
        std::wstring buffer(needed / sizeof(wchar_t) + 1, L'\0');
        auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
        if (QueryServiceConfigW(service, config, needed, &needed)) {
            read_service_command(split_command(wide_to_qstring(config->lpBinaryPathName)), info);
        }
    }

    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return info;
#endif
}

QString run_elevated_server(const QStringList& args) {
#ifndef _WIN32
    (void)args;
    return "Windows services are not available on this platform.";
#else
    const QString exe = server_executable();
    if (!QFileInfo::exists(exe)) {
        return "Could not find idRegistryServer.exe next to the config tool.";
    }

    QString parameters;
    for (const QString& arg : args) {
        if (!parameters.isEmpty()) {
            parameters += ' ';
        }
        parameters += '"';
        parameters += QString(arg).replace('"', "\\\"");
        parameters += '"';
    }

    SHELLEXECUTEINFOW execute{};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    execute.lpVerb = L"runas";
    std::wstring file = exe.toStdWString();
    std::wstring params = parameters.toStdWString();
    execute.lpFile = file.c_str();
    execute.lpParameters = params.c_str();
    execute.nShow = SW_HIDE;

    if (!ShellExecuteExW(&execute)) {
        DWORD error = GetLastError();
        if (error == ERROR_CANCELLED) {
            return "Administrator approval was cancelled.";
        }
        return "Could not start the elevated server command: " + win32_error(error);
    }

    if (execute.hProcess == nullptr) {
        return "The elevated server command did not report a process.";
    }

    DWORD wait = WaitForSingleObject(execute.hProcess, 60000);
    if (wait != WAIT_OBJECT_0) {
        CloseHandle(execute.hProcess);
        return "The elevated server command did not finish.";
    }

    DWORD code = 1;
    GetExitCodeProcess(execute.hProcess, &code);
    CloseHandle(execute.hProcess);
    if (code != 0) {
        return QString("The server command failed (exit code %1).").arg(code);
    }
    return {};
#endif
}
