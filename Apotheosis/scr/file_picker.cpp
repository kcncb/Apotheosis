#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <filesystem>
#include <iostream>
#include <system_error>

#include "file_picker.h"
#include "other_tools.h"

using Microsoft::WRL::ComPtr;

namespace file_picker
{
namespace
{
// RAII guard to pair CoInitializeEx with CoUninitialize. Initialization is
// per-thread so we only pay the cost when a picker is actually opened.
class ComApartment
{
public:
    ComApartment() noexcept
    {
        hr_ = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        owns_ = SUCCEEDED(hr_) && hr_ != RPC_E_CHANGED_MODE;
    }

    ~ComApartment()
    {
        if (owns_) ::CoUninitialize();
    }

    bool usable() const noexcept
    {
        return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE || hr_ == S_FALSE;
    }

private:
    HRESULT hr_ = S_OK;
    bool owns_ = false;
};
} // namespace

std::optional<std::string> open_file(const std::wstring& title,
                                     const std::vector<FilterSpec>& filters,
                                     void* parent_hwnd)
{
    ComApartment com;
    if (!com.usable())
        return std::nullopt;

    ComPtr<IFileOpenDialog> dialog;
    HRESULT hr = ::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || !dialog)
        return std::nullopt;

    if (!filters.empty())
    {
        std::vector<COMDLG_FILTERSPEC> specs;
        specs.reserve(filters.size());
        for (const auto& f : filters)
        {
            specs.push_back({ f.name.c_str(), f.pattern.c_str() });
        }
        dialog->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());
        dialog->SetFileTypeIndex(1);
    }

    if (!title.empty())
        dialog->SetTitle(title.c_str());

    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options)))
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST);

    hr = dialog->Show(static_cast<HWND>(parent_hwnd));
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        return std::nullopt;
    if (FAILED(hr))
        return std::nullopt;

    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item)) || !item)
        return std::nullopt;

    PWSTR wide_path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &wide_path)) || !wide_path)
        return std::nullopt;

    const std::wstring ws(wide_path);
    ::CoTaskMemFree(wide_path);
    return WideToUtf8(ws);
}

std::optional<std::string> import_onnx_into_models_dir(const std::string& source_path)
{
    namespace fs = std::filesystem;

    const fs::path src(Utf8ToWide(source_path));
    if (!fs::exists(src))
    {
        std::cerr << "[FilePicker] Source does not exist: " << source_path << std::endl;
        return std::nullopt;
    }

    const fs::path models_dir(L"models");
    std::error_code ec;
    fs::create_directories(models_dir, ec);
    if (ec)
    {
        std::cerr << "[FilePicker] Could not create models dir: " << ec.message() << std::endl;
        return std::nullopt;
    }

    const fs::path dst = models_dir / src.filename();

    // If the source already lives in the models directory, skip the copy and
    // just return its filename — avoids the "source and destination are the
    // same" error from fs::copy.
    if (fs::equivalent(src, dst, ec))
    {
        return WideToUtf8(src.filename().wstring());
    }
    ec.clear();

    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec)
    {
        std::cerr << "[FilePicker] Copy failed: " << ec.message() << std::endl;
        return std::nullopt;
    }

    return WideToUtf8(dst.filename().wstring());
}

} // namespace file_picker
