#include "overlay/app_log.h"

#include <algorithm>
#include <iostream>
#include <mutex>

namespace
{
constexpr size_t kMaxLogLines = 2000;

std::mutex g_logMutex;
std::vector<std::string> g_logLines;

AppLogStreamBuf g_coutCapture(std::cout.rdbuf(), "");
AppLogStreamBuf g_cerrCapture(std::cerr.rdbuf(), "[错误] ");
bool g_captureInstalled = false;
}

namespace AppLog
{
void InstallStdStreamCapture()
{
    if (g_captureInstalled)
        return;

    std::cout.rdbuf(&g_coutCapture);
    std::cerr.rdbuf(&g_cerrCapture);
    g_captureInstalled = true;
}

void AddLine(const std::string& line)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_logLines.push_back(line);
    if (g_logLines.size() > kMaxLogLines)
        g_logLines.erase(g_logLines.begin(), g_logLines.begin() + static_cast<std::ptrdiff_t>(g_logLines.size() - kMaxLogLines));
}

void Clear()
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_logLines.clear();
}

std::vector<std::string> Snapshot()
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    return g_logLines;
}
}

AppLogStreamBuf::AppLogStreamBuf(std::streambuf* original, const char* prefix)
    : original_(original), prefix_(prefix)
{
}

int AppLogStreamBuf::overflow(int ch)
{
    if (ch == traits_type::eof())
        return traits_type::not_eof(ch);

    std::lock_guard<std::mutex> lock(mutex_);
    if (original_)
        original_->sputc(static_cast<char>(ch));
    AppendChar(static_cast<char>(ch));
    return ch;
}

std::streamsize AppLogStreamBuf::xsputn(const char* s, std::streamsize count)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (original_)
        original_->sputn(s, count);

    for (std::streamsize i = 0; i < count; ++i)
        AppendChar(s[i]);

    return count;
}

int AppLogStreamBuf::sync()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (original_)
        original_->pubsync();
    if (!pending_.empty())
        FlushLine();
    return 0;
}

void AppLogStreamBuf::AppendChar(char ch)
{
    if (ch == '\r')
        return;

    if (ch == '\n')
    {
        FlushLine();
        return;
    }

    pending_.push_back(ch);
}

void AppLogStreamBuf::FlushLine()
{
    if (pending_.empty())
        return;

    AppLog::AddLine(std::string(prefix_) + pending_);
    pending_.clear();
}
