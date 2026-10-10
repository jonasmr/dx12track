#include "PipeServer.h"

#include "Elevation.h"
#include "Model.h"

#include <cstdio>
#include <cwchar>
#include <sddl.h>
#include <sstream>

namespace dx12track {

namespace {

bool ReadExact(HANDLE pipe, void* buffer, DWORD bytes) {
    BYTE* p = static_cast<BYTE*>(buffer);
    DWORD remaining = bytes;
    while (remaining) {
        DWORD got = 0;
        if (!ReadFile(pipe, p, remaining, &got, nullptr) || got == 0)
            return false;
        p += got;
        remaining -= got;
    }
    return true;
}

} // namespace

PipeServer::PipeServer() = default;

PipeServer::~PipeServer() {
    Stop();
    if (pipe_handle_ != INVALID_HANDLE_VALUE)
        CloseHandle(pipe_handle_);
}

bool PipeServer::Create(std::wstring* out_pipe_name,
                        const std::wstring& extra_client_sid) {
    std::wostringstream s;
    s << L"\\\\.\\pipe\\dx12track-" << GetCurrentProcessId() << L"-"
      << GetTickCount();
    pipe_name_ = s.str();

    // Explicit security descriptor: the default one of an elevated launcher
    // only lets SYSTEM/Administrators write, so the DLL in a non-elevated
    // target couldn't connect. Grant our user (+ the original launcher's user
    // when UAC elevated with other credentials), Administrators and SYSTEM,
    // and label the pipe medium integrity (no-write-up) so a medium-IL client
    // may open it for writing.
    std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)";
    const std::wstring me = CurrentUserSid();
    if (!me.empty()) sddl += L"(A;;GA;;;" + me + L")";
    if (!extra_client_sid.empty() && _wcsicmp(extra_client_sid.c_str(), me.c_str()) != 0)
        sddl += L"(A;;GA;;;" + extra_client_sid + L")";
    sddl += L"S:(ML;;NW;;;ME)";
    PSECURITY_DESCRIPTOR sd = nullptr;
    SECURITY_ATTRIBUTES sa{};
    if (!me.empty() && ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) {
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = sd;
    }

    pipe_handle_ = CreateNamedPipeW(
        pipe_name_.c_str(),
        PIPE_ACCESS_INBOUND,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,                    // max instances
        0,                    // out buffer (unused — inbound only)
        4 * 1024 * 1024,      // in buffer (see below)
        0,
        sd ? &sa : nullptr);
    if (sd) LocalFree(sd);
    if (pipe_handle_ == INVALID_HANDLE_VALUE && sd) {
        // E.g. a launcher below medium integrity may not apply the medium
        // label; fall back to the default descriptor.
        pipe_handle_ = CreateNamedPipeW(pipe_name_.c_str(), PIPE_ACCESS_INBOUND,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 0,
            4 * 1024 * 1024, 0, nullptr);
    }
    // 4 MiB in-buffer. Under --callstacks the DLL's DllMain dumps a
    // ModuleLoaded event for every module already loaded in the target
    // (often 200+ for real games, ~1 KiB per event); meanwhile the launcher
    // hasn't called ResumeThread yet so its reader thread isn't draining the
    // pipe. With a small buffer the DLL's WriteFile blocks, DllMain hangs,
    // the remote LoadLibraryW never returns, and the launcher's
    // WaitForSingleObject times out — total deadlock. 4 MiB comfortably
    // absorbs a few thousand events; if a real workload exceeds that we
    // need overlapped I/O on the writer side, not just a bigger buffer.

    if (pipe_handle_ == INVALID_HANDLE_VALUE) {
        fwprintf(stderr, L"CreateNamedPipeW failed: %lu\n", GetLastError());
        return false;
    }
    *out_pipe_name = pipe_name_;
    return true;
}

bool PipeServer::ConnectAndStart(Model& model, DWORD timeout_ms) {
    // ConnectNamedPipe in blocking mode waits until a client connects. We don't
    // really need an overlapped wait here because the child is already running
    // and will connect within the LoadLibrary call chain.
    (void)timeout_ms;
    BOOL connected = ConnectNamedPipe(pipe_handle_, nullptr);
    if (!connected) {
        DWORD err = GetLastError();
        if (err != ERROR_PIPE_CONNECTED) {
            fwprintf(stderr, L"ConnectNamedPipe failed: %lu\n", err);
            return false;
        }
    }

    stop_ = false;
    reader_ = std::thread(&PipeServer::ReaderLoop, this, &model);
    return true;
}

void PipeServer::Stop() {
    stop_ = true;
    if (pipe_handle_ != INVALID_HANDLE_VALUE) {
        // Unblock the reader by closing the read end. DisconnectNamedPipe also
        // works but we want the thread to fall out of ReadFile cleanly.
        CancelIoEx(pipe_handle_, nullptr);
    }
    if (reader_.joinable()) reader_.join();
}

void PipeServer::ReaderLoop(Model* model) {
    EventHeader hdr{};
    std::vector<BYTE> payload;

    while (!stop_) {
        if (!ReadExact(pipe_handle_, &hdr, sizeof(hdr)))
            break;
        if (hdr.magic != kProtocolMagic) {
            fwprintf(stderr, L"Bad magic on pipe: %08x\n", hdr.magic);
            break;
        }
        payload.resize(hdr.payload_bytes);
        if (hdr.payload_bytes > 0 &&
            !ReadExact(pipe_handle_, payload.data(), hdr.payload_bytes))
            break;
        model->OnEvent(hdr, payload.data(), payload.size());
    }
}

} // namespace dx12track
