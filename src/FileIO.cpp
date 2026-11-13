#include "FileIO.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <type_traits>

namespace {

constexpr uint32_t kMagic = 0x44524257;  // "WBRD"
constexpr uint32_t kVersion = 3;
constexpr uint32_t kFlagSession = 1;
constexpr uint8_t kStrokeAutoInk = 1;
constexpr uint32_t kMaxPathChars = 32768;

struct Writer {
    std::vector<uint8_t> buf;
    template <class T> void Put(const T& v)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        Bytes(&v, sizeof(T));
    }
    void Bytes(const void* data, size_t n)
    {
        const auto* p = static_cast<const uint8_t*>(data);
        buf.insert(buf.end(), p, p + n);
    }
};

struct Reader {
    const uint8_t* data;
    size_t size;
    size_t pos = 0;
    size_t Remaining() const { return size - pos; }
    template <class T> bool Get(T& v)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        return Bytes(&v, sizeof(T));
    }
    bool Bytes(void* out, size_t n)
    {
        if (n > Remaining())
            return false;
        memcpy(out, data + pos, n);
        pos += n;
        return true;
    }
};

uint64_t Fnv1a(const uint8_t* p, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

std::wstring LastErrorText(const wchar_t* what)
{
    wchar_t* msg = nullptr;
    const DWORD err = GetLastError();
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, err, 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
    std::wstring text = what;
    if (msg) {
        text += L"\n\n";
        text += msg;
        LocalFree(msg);
    }
    return text;
}

}  // namespace

bool WriteFileAtomic(const std::wstring& path, const std::vector<uint8_t>& data, bool flush, std::wstring& error)
{
    const std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        error = LastErrorText(L"Could not create the file.");
        return false;
    }
    size_t done = 0;
    bool ok = true;
    while (ok && done < data.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(data.size() - done, 64u << 20));
        DWORD written = 0;
        ok = WriteFile(f, data.data() + done, chunk, &written, nullptr) && written == chunk;
        done += written;
    }
    if (ok && flush)
        ok = FlushFileBuffers(f) != FALSE;
    if (!ok)
        error = LastErrorText(L"Could not write the file.");
    CloseHandle(f);
    if (ok && !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = LastErrorText(L"Could not replace the file.");
        ok = false;
    }
    if (!ok)
        DeleteFileW(tmp.c_str());
    return ok;
}

namespace {

bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>& data, std::wstring& error)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        error = LastErrorText(L"Could not open the file.");
        return false;
    }
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) != FALSE;
    if (ok && size.QuadPart > (2ll << 30)) {
        error = L"The file is too large (over 2 GB).";
        CloseHandle(f);
        return false;
    }
    if (ok) {
        data.resize(static_cast<size_t>(size.QuadPart));
        size_t done = 0;
        while (ok && done < data.size()) {
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(data.size() - done, 64u << 20));
            DWORD read = 0;
            ok = ReadFile(f, data.data() + done, chunk, &read, nullptr) && read == chunk;
            done += read;
        }
    }
    if (!ok)
        error = LastErrorText(L"Could not read the file.");
    CloseHandle(f);
    return ok;
}

inline bool Finite(double v) { return std::isfinite(v); }

}  // namespace

bool SaveBoardFile(const std::wstring& path, const Board& board, const DocView& view,
                   const SessionInfo* session, bool flush, std::wstring& error)
{
    std::vector<uint8_t> data;
    SerializeBoard(board, view, session, data);
    return WriteFileAtomic(path, data, flush, error);
}

struct BackgroundWriter::Impl {
    std::mutex mutex;
    std::condition_variable wake, idle;
    std::wstring path;
    std::vector<uint8_t> data;
    bool pending = false, busy = false, stop = false;
    std::thread thread;

    void Run()
    {
        std::unique_lock lock(mutex);
        for (;;) {
            wake.wait(lock, [&] { return pending || stop; });
            if (!pending && stop)
                return;
            std::wstring p = std::move(path);
            std::vector<uint8_t> d = std::move(data);
            pending = false;
            busy = true;
            lock.unlock();
            std::wstring error;
            WriteFileAtomic(p, d, false, error);  // autosave: failures are retried next time
            lock.lock();
            busy = false;
            idle.notify_all();
        }
    }
};

BackgroundWriter::BackgroundWriter() : m_impl(new Impl)
{
    m_impl->thread = std::thread([this] { m_impl->Run(); });
}

BackgroundWriter::~BackgroundWriter()
{
    {
        std::lock_guard lock(m_impl->mutex);
        m_impl->stop = true;
    }
    m_impl->wake.notify_all();
    m_impl->thread.join();  // a pending write still completes first
    delete m_impl;
}

void BackgroundWriter::Submit(std::wstring path, std::vector<uint8_t>&& data)
{
    {
        std::lock_guard lock(m_impl->mutex);
        m_impl->path = std::move(path);
        m_impl->data = std::move(data);
        m_impl->pending = true;
    }
    m_impl->wake.notify_one();
}

void BackgroundWriter::Flush()
{
    std::unique_lock lock(m_impl->mutex);
    m_impl->idle.wait(lock, [&] { return !m_impl->pending && !m_impl->busy; });
}

void SerializeBoard(const Board& board, const DocView& view, const SessionInfo* session, std::vector<uint8_t>& out)
{
    Writer w;
    w.buf.reserve(64 + board.TotalPoints() * 8 + board.LiveStrokes() * 32);
    w.Put(kMagic);
    w.Put(kVersion);
    w.Put(session ? kFlagSession : 0u);

    w.Put(view.center.x);
    w.Put(view.center.y);
    w.Put(view.zoom);
    w.Put(static_cast<uint8_t>(view.gridMode));
    w.Put(static_cast<int32_t>(view.gridSize));

    if (session) {
        w.Put(static_cast<uint8_t>(session->dark));
        w.Put(static_cast<int32_t>(session->colorIndex));
        w.Put(static_cast<int32_t>(session->widthIndex));
        w.Put(static_cast<uint8_t>(session->hasCustom));
        w.Put(session->customRgb);
        w.Put(static_cast<uint8_t>(session->docModified));
        const uint32_t len = static_cast<uint32_t>(std::min<size_t>(session->docPath.size(), kMaxPathChars));
        w.Put(len);
        w.Bytes(session->docPath.data(), len * sizeof(wchar_t));
        w.Put(static_cast<uint8_t>(session->hasWindow));
        for (int32_t v : session->windowRect)
            w.Put(v);
        w.Put(static_cast<uint8_t>(session->maximized));
        w.Put(static_cast<uint8_t>(session->showHelp));
        w.Put(static_cast<uint8_t>(session->swapButtons));
    }

    w.Put(static_cast<uint32_t>(board.LiveStrokes()));
    for (const Stroke& s : board.strokes) {
        if (!s.alive)
            continue;
        w.Put(s.origin.x);
        w.Put(s.origin.y);
        w.Put(s.width);
        w.Put(s.rgb);
        w.Put(static_cast<uint8_t>(s.autoInk ? kStrokeAutoInk : 0));
        w.Put(static_cast<uint32_t>(s.points.size()));
        w.Bytes(s.points.data(), s.points.size() * sizeof(D2D1_POINT_2F));
    }

    w.Put(Fnv1a(w.buf.data(), w.buf.size()));
    out = std::move(w.buf);
}

bool LoadBoardFile(const std::wstring& path, std::vector<Stroke>& strokes, DocView& view,
                   SessionInfo* session, bool& hasSession, std::wstring& error)
{
    hasSession = false;
    std::vector<uint8_t> data;
    if (!ReadWholeFile(path, data, error))
        return false;

    const wchar_t* kNotBoard = L"This is not a whiteboard file.";
    const wchar_t* kDamaged = L"The file is damaged and can't be opened.";
    if (data.size() < 12 + 8) {
        error = kNotBoard;
        return false;
    }
    Reader r{ data.data(), data.size() - 8 };
    uint32_t magic = 0, version = 0, flags = 0;
    r.Get(magic);
    r.Get(version);
    r.Get(flags);
    if (magic != kMagic) {
        error = kNotBoard;
        return false;
    }
    if (version > kVersion) {
        error = L"This file was made by a newer version of Whiteboard.";
        return false;
    }
    uint64_t stored = 0;
    memcpy(&stored, data.data() + data.size() - 8, 8);
    if (stored != Fnv1a(data.data(), data.size() - 8)) {
        error = kDamaged;
        return false;
    }

    uint8_t gridMode = 0;
    int32_t gridSize = 32;
    if (!r.Get(view.center.x) || !r.Get(view.center.y) || !r.Get(view.zoom) || !r.Get(gridMode) || !r.Get(gridSize)) {
        error = kDamaged;
        return false;
    }
    if (!Finite(view.center.x) || !Finite(view.center.y) || !Finite(view.zoom) || view.zoom <= 0.0) {
        view.center = {};
        view.zoom = 1.0;
    }
    view.gridMode = gridMode <= static_cast<uint8_t>(GridMode::Off) ? static_cast<GridMode>(gridMode) : GridMode::Lines;
    view.gridSize = std::clamp<int>(gridSize, GridSettings::kMinSize, GridSettings::kMaxSize);

    if (flags & kFlagSession) {
        SessionInfo tmp;
        uint8_t dark = 0, hasCustom = 0, modified = 0;
        int32_t color = 0, width = 1;
        uint32_t len = 0;
        if (!r.Get(dark) || !r.Get(color) || !r.Get(width) || !r.Get(hasCustom) || !r.Get(tmp.customRgb) ||
            !r.Get(modified) || !r.Get(len) || len > kMaxPathChars || len * sizeof(wchar_t) > r.Remaining()) {
            error = kDamaged;
            return false;
        }
        tmp.docPath.resize(len);
        r.Bytes(tmp.docPath.data(), len * sizeof(wchar_t));
        if (version >= 2) {
            uint8_t hasWindow = 0, maximized = 0, showHelp = 1;
            if (!r.Get(hasWindow) || !r.Get(tmp.windowRect[0]) || !r.Get(tmp.windowRect[1]) ||
                !r.Get(tmp.windowRect[2]) || !r.Get(tmp.windowRect[3]) || !r.Get(maximized) || !r.Get(showHelp)) {
                error = kDamaged;
                return false;
            }
            tmp.hasWindow = hasWindow != 0;
            tmp.maximized = maximized != 0;
            tmp.showHelp = showHelp != 0;
        }
        if (version >= 3) {
            uint8_t swap = 0;
            if (!r.Get(swap)) {
                error = kDamaged;
                return false;
            }
            tmp.swapButtons = swap != 0;
        }
        tmp.dark = dark != 0;
        tmp.colorIndex = color;
        tmp.widthIndex = width;
        tmp.hasCustom = hasCustom != 0;
        tmp.docModified = modified != 0;
        hasSession = true;
        if (session)
            *session = std::move(tmp);
    }

    uint32_t count = 0;
    constexpr size_t kMinStrokeBytes = 8 + 8 + 4 + 4 + 1 + 4;
    if (!r.Get(count) || count > r.Remaining() / kMinStrokeBytes) {
        error = kDamaged;
        return false;
    }
    std::vector<Stroke> loaded;
    loaded.resize(count);
    for (Stroke& s : loaded) {
        uint8_t sflags = 0;
        uint32_t n = 0;
        if (!r.Get(s.origin.x) || !r.Get(s.origin.y) || !r.Get(s.width) || !r.Get(s.rgb) || !r.Get(sflags) ||
            !r.Get(n) || n == 0 || n > r.Remaining() / sizeof(D2D1_POINT_2F)) {
            error = kDamaged;
            return false;
        }
        s.points.resize(n);
        r.Bytes(s.points.data(), n * sizeof(D2D1_POINT_2F));
        if (!Finite(s.origin.x) || !Finite(s.origin.y) || !std::isfinite(s.width) || s.width <= 0.0f) {
            error = kDamaged;
            return false;
        }
        for (const auto& p : s.points)
            if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
                error = kDamaged;
                return false;
            }
        s.autoInk = (sflags & kStrokeAutoInk) != 0;
        s.rgb &= 0xFFFFFF;
        s.ComputeBounds();
    }
    strokes = std::move(loaded);
    return true;
}
