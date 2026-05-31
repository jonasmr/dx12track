#pragma once

#include <mutex>
#include <string>
#include <windows.h>

#include "EventTypes.h"

namespace dx12track {

class PipeClient {
public:
    bool Connect(const std::wstring& pipe_name, DWORD retry_ms = 5000);
    void Close();

    // Send one event. Header.payload_bytes must match `payload_bytes`.
    void Send(EventKind kind, const void* payload, size_t payload_bytes);

    bool IsConnected() const { return pipe_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE     pipe_ = INVALID_HANDLE_VALUE;
    std::mutex mu_;
};

PipeClient& GlobalPipe();

} // namespace dx12track
