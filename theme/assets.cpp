#include "theme/assets.h"

#include <fstream>
#include <system_error>

#include "builtin_assets.h"

namespace theme {

bool ReadFile(const std::filesystem::path& path, std::string& out, size_t maxBytes) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > maxBytes) return false;
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(static_cast<size_t>(size), '\0');
    f.read(out.data(), static_cast<std::streamsize>(size));
    return static_cast<size_t>(f.gcount()) == size;
}

bool WriteFileAtomic(const std::filesystem::path& path, std::string_view data) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!f) return false;
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

bool ReadAsset(const std::filesystem::path& assetsDir, std::string_view rel, std::string& out, bool* fromDisk) {
    // assetsDir указывает на саму папку assets, а rel начинается с "assets/"
    std::string_view inside = rel;
    if (inside.rfind("assets/", 0) == 0) inside.remove_prefix(7);
    if (!assetsDir.empty() && ReadFile(assetsDir / std::filesystem::path(std::string(inside)), out, 1u << 20)) {
        if (fromDisk) *fromDisk = true;
        return true;
    }
    for (int i = 0; i < pm::builtin::kAssetCount; ++i) {
        const auto& a = pm::builtin::kAssets[i];
        if (rel != a.path) continue;
        out.clear();
        for (int p = 0; p < a.partCount; ++p) out += a.parts[p];
        if (fromDisk) *fromDisk = false;
        return true;
    }
    return false;
}

}  // namespace theme
