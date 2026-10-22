#pragma once

#include <QString>
#include <filesystem>
#include <vector>

namespace Core::Search {

// скоринг подпоследовательности как в fzf и ноль если совпадений нет
// если запрос из нескольких слов то ищем каждое слово и суммируем очки
int fuzzyScore(const QString& query, const QString& text);

// относительный путь для вывода
QString displayPath(const std::filesystem::path& root, const std::filesystem::path& path);

std::vector<std::filesystem::path> searchRoots();
bool isMountPoint(const std::filesystem::path& path);

bool isVideoFile(const std::filesystem::path& path);
bool isPdfFile(const std::filesystem::path& path);

}
