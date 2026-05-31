#include "PipeServer.h"

#include "Model.h"

#include <cstdio>
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

bool PipeServer::Create(std::wstring* out_pipe_name) {
    std::wostringstream s;
    s << L"\\\\.\\pipe\\dx12track-" << GetCurrentProcessId() << L"-"
      << GetTickCount();
    pipe_name_ = s.str();

    pipe_handle_ = CreateNamedPipeW(
        pipe_name_.c_str(),
        PIPE_ACCESS_INBOUND,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,           // max instances
        0,           // out buffer (unused — inbound only)
        64 * 1024,   // in buffer
        0,
        nullptr);

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
