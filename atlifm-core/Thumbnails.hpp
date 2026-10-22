#pragma once

// превью видео и pdf через сторонние утилиты с кэшированием на диске
// если утилит нет то отдаем ошибку и не падаем

#include <QImage>
#include <QString>
#include <atomic>
#include <filesystem>
#include <memory>

namespace Core::Thumbnails {

struct Result {
    QImage image;
    QString info;
    QString error;
    bool cancelled{false};
};

QString cacheDirectory();
QString cacheKey(const std::filesystem::path& path, int page = 0);
QImage loadCached(const std::filesystem::path& path, int page = 0);
void storeCached(const std::filesystem::path& path, const QImage& image, int page = 0);
void pruneCache(qint64 maxBytes = 192LL * 1024 * 1024, int maxFiles = 600);

Result renderVideo(const std::filesystem::path& path, int width = 640,
                   const std::shared_ptr<std::atomic_bool>& cancelToken = {});
Result renderPdfPage(const std::filesystem::path& path, int page, int width = 900,
                     const std::shared_ptr<std::atomic_bool>& cancelToken = {});
int pdfPageCount(const std::filesystem::path& path,
                 const std::shared_ptr<std::atomic_bool>& cancelToken = {});

} // namespace Core::Thumbnails
