#ifndef APP_LOG_H
#define APP_LOG_H

#include <iosfwd>
#include <mutex>
#include <streambuf>
#include <string>
#include <vector>

namespace AppLog
{
void InstallStdStreamCapture();
void AddLine(const std::string& line);
void Clear();
std::vector<std::string> Snapshot();
}

class AppLogStreamBuf : public std::streambuf
{
public:
    AppLogStreamBuf(std::streambuf* original, const char* prefix);

protected:
    int overflow(int ch) override;
    std::streamsize xsputn(const char* s, std::streamsize count) override;
    int sync() override;

private:
    void AppendChar(char ch);
    void FlushLine();

    std::streambuf* original_;
    const char* prefix_;
    std::string pending_;
    std::mutex mutex_;
};

#endif // APP_LOG_H
