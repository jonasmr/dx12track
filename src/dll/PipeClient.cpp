#include "PipeClient.h"

#include "Clock.h"

#include <chrono>
#include <thread>

namespace dx12track {

bool PipeClient::Connect(const std::wstring& pipe_name, DWORD retry_ms) {
    using namespace std::chrono;
    auto deadline = steady_clock::now() + milliseconds(retry_ms);
    for (;;) {
        pipe_ = CreateFileW(pipe_name.c_str(), GENERIC_WRITE, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr);
        if (pipe_ != INVALID_HANDLE_VALUE) break;
        DWORD err = GetLastError();
        if (err == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(pipe_name.c_str(), 500);
        } else {
            if (steady_clock::now() > deadline) return false;
            std::this_thread::sleep_for(milliseconds(50));
        }
    }
    return true;
}

void PipeClient::Close() {
    std::lock_guard<std::mutex> lock(mu_);
    if (pipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
}

void PipeClient::Send(EventKind kind, const void* payload, size_t payload_bytes) {
    if (pipe_ == INVALID_HANDLE_VALUE) return;
    EventHeader hdr{};
    hdr.magic         = kProtocolMagic;
    hdr.payload_bytes = (uint32_t)payload_bytes;
    hdr.ts_ns         = NowNs();
    hdr.kind          = kind;

    std::lock_guard<std::mutex> lock(mu_);
    DWORD written = 0;
    if (!WriteFile(pipe_, &hdr, sizeof(hdr), &written, nullptr) ||
        written != sizeof(hdr)) {
        CloseHandle(pipe_); pipe_ = INVALID_HANDLE_VALUE; return;
    }
    if (payload_bytes > 0) {
        if (!WriteFile(pipe_, payload, (DWORD)payload_bytes, &written, nullptr) ||
            written != payload_bytes) {
            CloseHandle(pipe_); pipe_ = INVALID_HANDLE_VALUE;
        }
    }
}

PipeClient& GlobalPipe() {
    static PipeClient g;
    return g;
}

} // namespace dx12track
