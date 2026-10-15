#pragma once
#include <filesystem>
#include <string>
#include <system_error>

namespace svenios {
inline void mergeLegacyDirectory(const std::filesystem::path &source, const std::filesystem::path &destination) {
    namespace fs = std::filesystem;
    for (const auto &entry : fs::directory_iterator(source)) {
        fs::path target = destination / entry.path().filename();
        auto targetStatus = fs::symlink_status(target);
        if (!fs::exists(targetStatus)) fs::rename(entry.path(), target);
        else if (fs::is_directory(entry.symlink_status()) && fs::is_directory(targetStatus))
            mergeLegacyDirectory(entry.path(), target);
    }
    std::error_code error;
    fs::remove(source, error);
    if (error && error != std::errc::directory_not_empty) throw fs::filesystem_error("Remove legacy directory", source, error);
}

inline bool migrateLegacyRoot(const std::filesystem::path &documents, std::string &message) {
    namespace fs = std::filesystem;
    try {
        fs::create_directories(documents);
        fs::path legacy = documents / "svenclient";
        if (!fs::is_directory(fs::symlink_status(legacy))) return true;
        fs::path backup = documents / ".svenclient-legacy";
        for (unsigned index = 1; fs::exists(fs::symlink_status(backup)); ++index)
            backup = documents / (".svenclient-legacy-" + std::to_string(index));
        fs::rename(legacy, backup);
        mergeLegacyDirectory(backup, documents);
        if (fs::exists(backup)) message = "Existing files were kept; duplicate legacy files remain in " + backup.string();
        return true;
    } catch (const fs::filesystem_error &error) {
        message = error.what();
        return false;
    }
}
}
