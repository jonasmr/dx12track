#pragma once

#include <string>
#include <vector>
#include <windows.h>

#include "Model.h"

namespace dx12track {

// Double-buffered console renderer. Pushes a fixed-size CHAR_INFO buffer to the
// output console with a single WriteConsoleOutputW call per Render() to avoid
// flicker and intermediate state.
class Renderer {
public:
    Renderer();

    // Discovers the console buffer size (or falls back to 120x40 if there is no
    // attached console). Must be called once before Render().
    bool Init();

    void Render(const Model::Snapshot& snap);

    // Debug aid (--dump-console): also write the composed frame as UTF-8 text
    // to `path` (overwritten, at most once per second and on the final frame).
    // Lets automated runs without a real console see what would be drawn.
    void SetDumpPath(const std::wstring& path) { dump_path_ = path; }
    void DumpNow();

private:
    void Resize(SHORT cols, SHORT rows);
    void Clear(WORD attr = 0x07);
    void Put(SHORT x, SHORT y, const wchar_t* s, WORD attr = 0x07);
    void PutF(SHORT x, SHORT y, WORD attr, const wchar_t* fmt, ...);
    SHORT RenderEtwStatus(SHORT row, const Model::Snapshot& snap);

    HANDLE              stdout_ = INVALID_HANDLE_VALUE;
    SHORT               cols_   = 120;
    SHORT               rows_   = 40;
    std::vector<CHAR_INFO> buffer_;
    std::wstring        dump_path_;
    ULONGLONG           last_dump_ms_ = 0;
};

} // namespace dx12track
