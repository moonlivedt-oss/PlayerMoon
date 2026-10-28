#pragma once
// Чтение файлов из assets: сначала с диска (папка assets рядом с exe - её можно
// править без пересборки), иначе из копии, зашитой в exe при сборке.

#include <filesystem>
#include <string>
#include <string_view>

namespace theme {

// rel - путь как в репозитории, например "assets/themes/glass.json".
// fromDisk (если задан) = true, если файл прочитан с диска.
bool ReadAsset(const std::filesystem::path& assetsDir, std::string_view rel, std::string& out,
               bool* fromDisk = nullptr);

// Прочитать файл целиком. false, если не удалось открыть или файл больше maxBytes.
bool ReadFile(const std::filesystem::path& path, std::string& out, size_t maxBytes = 16u << 20);
// Записать файл через временный и переименование (не оставит полупустой файл при сбое)
bool WriteFileAtomic(const std::filesystem::path& path, std::string_view data);

}  // namespace theme
