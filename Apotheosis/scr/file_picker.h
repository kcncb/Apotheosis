#ifndef SCR_FILE_PICKER_H
#define SCR_FILE_PICKER_H

#include <optional>
#include <string>
#include <vector>

namespace file_picker
{
// Options describing a "filetype" entry for IFileDialog.
struct FilterSpec
{
    std::wstring name;    // e.g. L"ONNX models"
    std::wstring pattern; // e.g. L"*.onnx"
};

// Opens a native Win32 file-open dialog filtered by the provided spec list.
// The first filter is the default. Returns the selected absolute path on
// success, or std::nullopt on cancel / error.
std::optional<std::string> open_file(const std::wstring& title,
                                     const std::vector<FilterSpec>& filters,
                                     void* parent_hwnd = nullptr);

// Convenience helper: copies the given absolute path into the project's
// "models/" directory (creating the directory if needed). Returns the
// destination filename on success (not full path) so callers can push it
// into config.ai_model.
std::optional<std::string> import_onnx_into_models_dir(const std::string& source_path);
} // namespace file_picker

#endif // SCR_FILE_PICKER_H
