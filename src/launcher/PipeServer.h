#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

#include "EventTypes.h"

namespace dx12track {

class Model;

class PipeServer {
public:
    PipeServer();
    ~PipeServer();

    // Allocate a fresh pipe name and create the server-side pipe handle.
    // Returns the full \\.\pipe\... name to expose to the child via env var.
    bool Create(std::wstring* out_pipe_name);

    // Wait (blocking, with timeout) for the injected DLL to connect, then
    // start a background reader thread that dispatches events into `model`.
    bool ConnectAndStart(Model& model, DWORD timeout_ms = 10000);

    // Signals the reader thread to exit and joins it. Called automatically
    // by the destructor if not done already.
    void Stop();

private:
    void ReaderLoop(Model* model);

    HANDLE              pipe_handle_   = INVALID_HANDLE_VALUE;
    std::wstring        pipe_name_;
    std::thread         reader_;
    std::atomic<bool>   stop_{false};
};

} // namespace dx12track
