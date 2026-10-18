#include "DragDropHandler.hpp"
#include "../atlifm-models/FileSystemModel.hpp"
#include "../atlifm-widgets/StatusBar.hpp"
#include "../atlifm-widgets/TuiInputPopup.hpp"
#include "../atlifm-theme/Theme.hpp"
#include "../atlifm-core/XdgUtils.hpp"
#include "../atlifm-core/UndoManager.hpp"
#include "../atlifm-widgets/MillerColumns.hpp"
#include <QMenu>
#include <QPainter>
#include <QApplication>
#include <QUrl>
#include <QThreadPool>
#include <QCursor>
#include <QTimer>
#include <QPointer>
#include <QPushButton>
#include <QBuffer>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QProcess>
#include <QRegularExpression>
#include <QDateTime>
#include <sys/stat.h>
#include <atomic>
#include <mutex>
#include <memory>
#include <regex>

namespace Dnd {

static std::shared_ptr<std::atomic<bool>> s_cancelToken;
static std::mutex s_opMutex;

// MIME текст может приходить в UTF-16 (в стиле Windows) или UTF-8
static QString decodeMimeText(const QByteArray& d) {
    return (d.size() >= 2 && d[1] == '\0')
        ? QString::fromUtf16(reinterpret_cast<const char16_t*>(d.constData()), d.size() / 2)
        : QString::fromUtf8(d);
}

// единая точка вывода прогресса и сообщений операции в статус бар
static void postStatusMessage(QWidget* ctx, const QString& msg, bool final) {
    QWidget* win = ctx ? ctx->window() : qApp->activeWindow();
    if (!win) return;
    if (auto* sb = win->findChild<Widgets::TuiStatusBar*>()) {
        sb->setProgressMessage(msg, final);
    }
}

// каталоги считаем рекурсивно, иначе бар висит на 100%
static uintmax_t contentSizeBytes(const std::filesystem::path& p) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(p, ec)) return std::filesystem::file_size(p, ec);
    if (!std::filesystem::is_directory(p, ec)) return 0;
    uintmax_t total = 0;
    std::filesystem::recursive_directory_iterator it(
        p, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;
    while (!ec && it != end) {
        if (it->is_regular_file(ec)) {
            total += it->file_size(ec);
        }
        it.increment(ec);
    }
    return total;
}

// targetDir внутри src уводит рекурсивный copyItem в бесконечную рекурсию,
// поэтому сравнение лексическое без canonical (может висеть на недоступных каталогах)
static bool isSelfOrDescendant(const std::filesystem::path& targetDir,
                               const std::filesystem::path& src) {
    const std::filesystem::path a = targetDir.lexically_normal();
    const std::filesystem::path b = src.lexically_normal();
    auto ai = a.begin();
    for (auto bi = b.begin(); bi != b.end(); ++bi, ++ai) {
        if (ai == a.end() || *ai != *bi) return false;
    }
    return true;
}

// откат завершённых элементов делает только воркер как владелец операции,
// иначе UI гонял бы rename'ы по тем же путям параллельно с копированием
static void rollbackCompletedItems(const std::vector<Core::UndoItem>& items) {
    for (const auto& item : items) {
        std::error_code ec;
        if (std::filesystem::exists(item.dest, ec)) {
            if (!item.src.empty() && !std::filesystem::exists(item.src, ec)) {
                std::filesystem::rename(item.dest, item.src, ec);
            } else {
                std::filesystem::remove_all(item.dest, ec);
            }
        }
        if (!item.backupPath.empty()) {
            Core::restoreFromUndo(item.backupPath, item.dest);
        }
        Models::FileSystemModel::removeInProgressPath(item.src);
        Models::FileSystemModel::removeInProgressPath(item.dest);
    }
}

bool DragDropHandler::hasActiveOperation() {
    std::lock_guard<std::mutex> lock(s_opMutex);
    return s_cancelToken != nullptr && !s_cancelToken->load();
}

void DragDropHandler::cancelActiveOperation() {
    std::lock_guard<std::mutex> lock(s_opMutex);
    if (s_cancelToken) {
        s_cancelToken->store(true);
    }
    // откат и сброс токена делает воркер: новая операция не стартует,
    // пока executeCopyMove видит живой отменённый токен
}

QDrag* DragDropHandler::createDrag(const std::vector<std::filesystem::path>& paths, QWidget* dragSource) {
    if (paths.empty() || !dragSource) return nullptr;

    auto drag = new QDrag(dragSource);
    auto mimeData = new QMimeData();

    QList<QUrl> urls;
    urls.reserve(paths.size());
    QByteArray gnomeCopiedData = "copy\n";
    QString mozUrlText;

    for (const auto& p : paths) {
        std::error_code ec;
        std::filesystem::path absPath = std::filesystem::absolute(p, ec);
        if (ec) absPath = p;

        QString pathQ = QString::fromUtf8(absPath.c_str());
        QUrl url = QUrl::fromLocalFile(pathQ);
        urls.append(url);
        gnomeCopiedData += url.toEncoded() + "\n";

        if (mozUrlText.isEmpty()) {
            QString fnameQ = QString::fromUtf8(absPath.filename().c_str());
            mozUrlText = url.toString() + "\n" + fnameQ;
        }
    }

    // стандартный RFC 2483 text/uri-list + FileTransfer из портала XDG (используется Chrome, Firefox, Electron, Wayland/X11)
    mimeData->setUrls(urls);
    // совместимость с GTK / Nautilus / Thunar / Nemo
    mimeData->setData("x-special/gnome-copied-files", gnomeCopiedData.trimmed());
    // совместимость с Gecko / Firefox / Zen Browser (url\nname в UTF-16)
    if (!mozUrlText.isEmpty()) {
        QByteArray mozBytes(reinterpret_cast<const char*>(mozUrlText.utf16()), mozUrlText.size() * 2);
        mimeData->setData("text/x-moz-url", mozBytes);
    }

    drag->setMimeData(mimeData);

    QString firstItem = QString::fromUtf8(paths.front().filename().c_str());
    QPixmap pix = createDragPixmap(paths.size(), firstItem);
    drag->setPixmap(pix);
    drag->setHotSpot(QPoint(16, 16));

    return drag;
}

QPixmap DragDropHandler::createDragPixmap(size_t itemCount, const QString& firstItemName) {
    QFont font = Theme::getMonospaceFont(9, QFont::Bold);
    QFontMetrics fm(font);

    QString badgeText;
    if (itemCount == 1) {
        badgeText = QString("📄 %1").arg(firstItemName);
    } else {
        badgeText = QString("📄 %1 (+%2 items)").arg(firstItemName).arg(itemCount - 1);
    }

    int textWidth = fm.horizontalAdvance(badgeText);
    int padX = 10;
    int padY = 6;
    int w = textWidth + padX * 2;
    int h = fm.height() + padY * 2;

    QPixmap pixmap(w, h);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, false);

    // высококонтрастный бейдж с острыми прямоугольными углами на цветах палитры (руководство §3.5)
    QRect rect(0, 0, w, h);
    QColor bgCol = Theme::Colors::BadgeBg();
    bgCol.setAlpha(240);
    painter.setBrush(bgCol);
    painter.setPen(QPen(Theme::Colors::BadgeBorder(), 1));
    painter.drawRect(rect.adjusted(0, 0, -1, -1));

    painter.setFont(font);
    painter.setPen(Theme::Colors::BadgeFg());
    painter.drawText(rect, Qt::AlignCenter, badgeText);

    return pixmap;
}

static bool hasSupportedImageFormat(const QMimeData* mimeData) {
    if (!mimeData) return false;
    if (mimeData->hasImage()) return true;
    static const char* const kImgMimes[] = {
        "image/png", "image/jpeg", "image/jpg", "image/webp",
        "image/gif", "image/svg+xml", "image/bmp", "image/avif",
        "application/x-qt-image"
    };
    for (const char* m : kImgMimes) {
        if (mimeData->hasFormat(QString::fromLatin1(m))) return true;
    }
    return false;
}

bool DragDropHandler::canAcceptDrop(const QMimeData* mimeData) {
    if (!mimeData) return false;
    if (mimeData->hasUrls() ||
        mimeData->hasFormat("text/uri-list") ||
        mimeData->hasFormat("x-special/gnome-copied-files") ||
        mimeData->hasFormat("text/x-moz-url") ||
        mimeData->hasFormat("application/x-moz-file-promise-url") ||
        hasSupportedImageFormat(mimeData)) {
        return true;
    }
    if (mimeData->hasHtml()) {
        const QString html = mimeData->html();
        if (html.contains("<img", Qt::CaseInsensitive) ||
            html.contains("data:image/", Qt::CaseInsensitive)) {
            return true;
        }
    }
    if (mimeData->hasText()) {
        const QString t = mimeData->text().trimmed();
        if (t.startsWith("file://") || t.startsWith("data:image/") ||
            t.startsWith("http://") || t.startsWith("https://")) {
            return true;
        }
    }
    return false;
}

std::vector<std::filesystem::path> DragDropHandler::extractPaths(const QMimeData* mimeData) {
    std::vector<std::filesystem::path> paths;
    if (!mimeData) return paths;

    auto addLocalUrl = [&paths](const QUrl& url) {
        if (url.isValid() && url.isLocalFile()) {
            QString localStr = url.toLocalFile();
            if (!localStr.isEmpty()) {
                std::filesystem::path p(localStr.toUtf8().toStdString());
                if (std::find(paths.begin(), paths.end(), p) == paths.end()) {
                    paths.push_back(std::move(p));
                }
            }
        }
    };

    if (mimeData->hasUrls()) {
        for (const auto& url : mimeData->urls()) {
            addLocalUrl(url);
        }
    }
    if (paths.empty() && mimeData->hasFormat("text/uri-list")) {
        QByteArray data = mimeData->data("text/uri-list");
        auto lines = QString::fromUtf8(data).split('\n', Qt::SkipEmptyParts);
        for (const auto& line : lines) {
            QString trimmed = line.trimmed();
            if (trimmed.startsWith('#')) continue;
            addLocalUrl(QUrl(trimmed));
        }
    }
    if (paths.empty() && mimeData->hasFormat("x-special/gnome-copied-files")) {
        QByteArray data = mimeData->data("x-special/gnome-copied-files");
        auto lines = QString::fromUtf8(data).split('\n', Qt::SkipEmptyParts);
        for (const auto& line : lines) {
            QString trimmed = line.trimmed();
            if (trimmed == "copy" || trimmed == "cut") continue;
            addLocalUrl(QUrl(trimmed));
        }
    }
    if (paths.empty() && mimeData->hasText()) {
        auto lines = mimeData->text().split('\n', Qt::SkipEmptyParts);
        for (const auto& line : lines) {
            QString trimmed = line.trimmed();
            if (trimmed.startsWith("file://")) {
                addLocalUrl(QUrl(trimmed));
            }
        }
    }

    return paths;
}

static bool isDifferentFilesystem(const std::filesystem::path& p1, const std::filesystem::path& p2) {
    struct stat st1, st2;
    if (::stat(p1.c_str(), &st1) != 0) return false;
    std::filesystem::path checkTarget = p2;
    std::error_code ec;
    while (!std::filesystem::exists(checkTarget, ec) && checkTarget.has_parent_path() && checkTarget != checkTarget.parent_path()) {
        checkTarget = checkTarget.parent_path();
    }
    if (::stat(checkTarget.c_str(), &st2) != 0) return false;
    return st1.st_dev != st2.st_dev;
}

static std::filesystem::path generateCopyName(const std::filesystem::path& src, const std::filesystem::path& targetDir) {
    std::string filename = src.filename().string();
    std::string stem = filename;
    std::string ext;

    static const std::vector<std::string> compositeExts = {
        ".tar.gz", ".tar.xz", ".tar.bz2", ".tar.zst"
    };
    bool foundComp = false;
    for (const auto& ce : compositeExts) {
        if (filename.size() > ce.size() && filename.rfind(ce) == filename.size() - ce.size()) {
            stem = filename.substr(0, filename.size() - ce.size());
            ext = ce;
            foundComp = true;
            break;
        }
    }
    if (!foundComp) {
        size_t dot = filename.rfind('.');
        if (dot != std::string::npos && dot > 0) {
            stem = filename.substr(0, dot);
            ext = filename.substr(dot);
        } else {
            stem = filename;
            ext = "";
        }
    }

    static const std::regex reg(R"(^(.*)_(\d+)$)");
    std::smatch match;
    std::string baseStem = stem;
    int num = 1;
    if (std::regex_match(stem, match, reg)) {
        baseStem = match[1].str();
        try {
            num = std::stoi(match[2].str()) + 1;
        } catch (...) {
            num = 1;
        }
    }

    std::filesystem::path candidate;
    std::error_code ec;
    do {
        std::string newFilename = baseStem + "_" + std::to_string(num) + ext;
        candidate = targetDir / newFilename;
        num++;
    } while (std::filesystem::exists(candidate, ec));

    return candidate;
}

static std::string sanitizeFilenameStem(QString rawName) {
    rawName = QUrl::fromPercentEncoding(rawName.toUtf8()).trimmed();
    int slashIdx = std::max(rawName.lastIndexOf('/'), rawName.lastIndexOf('\\'));
    if (slashIdx >= 0) rawName = rawName.mid(slashIdx + 1);
    int qIdx = rawName.indexOf('?');
    if (qIdx >= 0) rawName = rawName.left(qIdx);
    int hashIdx = rawName.indexOf('#');
    if (hashIdx >= 0) rawName = rawName.left(hashIdx);

    // отбрасываем известные расширения изображений и файлов, чтобы выделить stem
    int dotIdx = rawName.lastIndexOf('.');
    if (dotIdx > 0 && rawName.size() - dotIdx <= 6) {
        rawName = rawName.left(dotIdx);
    }

    QString cleaned;
    cleaned.reserve(rawName.size());
    for (QChar ch : rawName) {
        if (ch.isLetterOrNumber() || ch == '_' || ch == '-' || ch == '.' || ch == ' ') {
            cleaned.append(ch == ' ' ? QChar('_') : ch);
        }
    }
    while (cleaned.startsWith('.')) cleaned.remove(0, 1);
    if (cleaned.size() > 80) cleaned.truncate(80);

    std::string res = cleaned.toUtf8().toStdString();
    if (res.empty() || res == "blob" || res == "image" || res == "download" || res == "index") {
        return "";
    }
    return res;
}

static QString extractHtmlImgAttribute(const QString& html, const QString& attrName) {
    QRegularExpression re(QString("<img\\b[^>]*\\b%1\\s*=\\s*[\"']([^\"']+)[\"']").arg(attrName),
                          QRegularExpression::CaseInsensitiveOption);
    auto m = re.match(html);
    if (m.hasMatch()) {
        return m.captured(1).trimmed();
    }
    return QString();
}

static std::string extractFilenameHint(const QMimeData* mimeData, std::string& outHintExt) {
    outHintExt.clear();
    if (!mimeData) return "";

    auto inspectCandidate = [&](const QString& candidate) -> std::string {
        if (candidate.isEmpty() || candidate.startsWith("data:", Qt::CaseInsensitive)) return "";
        QString clean = QUrl::fromPercentEncoding(candidate.toUtf8());
        int q = clean.indexOf('?');
        if (q >= 0) clean = clean.left(q);
        int h = clean.indexOf('#');
        if (h >= 0) clean = clean.left(h);
        int sl = std::max(clean.lastIndexOf('/'), clean.lastIndexOf('\\'));
        if (sl >= 0) clean = clean.mid(sl + 1);

        int dot = clean.lastIndexOf('.');
        if (dot > 0 && clean.size() - dot <= 6) {
            std::string ext = clean.mid(dot).toLower().toUtf8().toStdString();
            if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp" ||
                ext == ".gif" || ext == ".svg" || ext == ".bmp" || ext == ".avif") {
                outHintExt = (ext == ".jpeg") ? ".jpg" : ext;
            }
        }
        return sanitizeFilenameStem(clean);
    };

    if (mimeData->hasFormat("application/x-moz-file-promise-dest-filename")) {
        QByteArray d = mimeData->data("application/x-moz-file-promise-dest-filename");
        QString s = decodeMimeText(d);
        auto stem = inspectCandidate(s);
        if (!stem.empty()) return stem;
    }

    if (mimeData->hasFormat("text/x-moz-url")) {
        QByteArray d = mimeData->data("text/x-moz-url");
        QString s = decodeMimeText(d);
        auto lines = s.split('\n', Qt::SkipEmptyParts);
        for (const auto& line : lines) {
            auto stem = inspectCandidate(line.trimmed());
            if (!stem.empty()) return stem;
        }
    }

    if (mimeData->hasHtml()) {
        QString src = extractHtmlImgAttribute(mimeData->html(), "src");
        auto stem = inspectCandidate(src);
        if (!stem.empty()) return stem;

        QString alt = extractHtmlImgAttribute(mimeData->html(), "alt");
        auto altStem = sanitizeFilenameStem(alt);
        if (!altStem.empty()) return altStem;
    }

    if (mimeData->hasUrls()) {
        for (const auto& u : mimeData->urls()) {
            auto stem = inspectCandidate(u.toString());
            if (!stem.empty()) return stem;
        }
    }

    return "";
}

static bool tryDecodeDataUri(const QString& uri, QByteArray& outBytes, std::string& outExt) {
    QString trimmed = uri.trimmed();
    if (!trimmed.startsWith("data:image/", Qt::CaseInsensitive)) return false;

    int commaIdx = trimmed.indexOf(',');
    if (commaIdx < 0) return false;

    QString header = trimmed.left(commaIdx).toLower();
    QString payload = trimmed.mid(commaIdx + 1);

    if (header.contains("image/png")) outExt = ".png";
    else if (header.contains("image/jpeg") || header.contains("image/jpg")) outExt = ".jpg";
    else if (header.contains("image/webp")) outExt = ".webp";
    else if (header.contains("image/gif")) outExt = ".gif";
    else if (header.contains("image/svg+xml")) outExt = ".svg";
    else if (header.contains("image/bmp")) outExt = ".bmp";
    else outExt = ".png";

    if (header.contains(";base64")) {
        outBytes = QByteArray::fromBase64(payload.toLatin1());
    } else {
        outBytes = QUrl::fromPercentEncoding(payload.toUtf8()).toUtf8();
    }
    return !outBytes.isEmpty();
}

static std::string detectExtensionFromMagicBytes(const QByteArray& bytes, const std::string& fallbackExt) {
    if (bytes.size() >= 8 && bytes.startsWith("\x89PNG\r\n\x1a\n")) return ".png";
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
        static_cast<unsigned char>(bytes[1]) == 0xD8 &&
        static_cast<unsigned char>(bytes[2]) == 0xFF) return ".jpg";
    if (bytes.size() >= 12 && bytes.startsWith("RIFF") && bytes.mid(8, 4) == "WEBP") return ".webp";
    if (bytes.size() >= 6 && (bytes.startsWith("GIF87a") || bytes.startsWith("GIF89a"))) return ".gif";
    if (bytes.size() >= 2 && bytes.startsWith("BM")) return ".bmp";
    if (bytes.size() >= 4 && bytes.startsWith("%PDF")) return ".pdf";
    QByteArray head = bytes.left(256).trimmed();
    if (head.startsWith("<svg") || (head.startsWith("<?xml") && head.contains("<svg"))) return ".svg";
    return fallbackExt;
}

static void notifyDropCompletedOnUi(const QPointer<QWidget>& safeParent,
                                    const std::filesystem::path& createdPath,
                                    const QString& statusMsg,
                                    const std::function<void()>& onFinished) {
    if (safeParent) {
        if (auto* mvc = qobject_cast<Widgets::MillerViewContainer*>(safeParent.data())) {
            mvc->selectPathAfterRefresh(createdPath);
            mvc->refresh();
        } else if (safeParent->window()) {
            for (auto* c : safeParent->window()->findChildren<Widgets::MillerViewContainer*>()) {
                c->selectPathAfterRefresh(createdPath);
                c->refresh();
            }
        }
    }
    if (onFinished) {
        onFinished();
    }
    postStatusMessage(safeParent.data(), statusMsg, true);
}

bool DragDropHandler::handleMimeDrop(const QMimeData* mimeData,
                                     const std::filesystem::path& targetDir,
                                     Qt::DropAction proposedAction,
                                     Qt::KeyboardModifiers modifiers,
                                     QWidget* parentWidget,
                                     std::function<void()> onFinished) {
    if (!mimeData || targetDir.empty()) return false;

    // 1. локальные пути файловой системы (внутренний drag&drop или внешние приложения)
    auto localPaths = extractPaths(mimeData);
    if (!localPaths.empty()) {
        handleDrop(localPaths, targetDir, proposedAction, modifiers, parentWidget, onFinished);
        return true;
    }

    QPointer<QWidget> safeParent(parentWidget);
    std::string hintExt;
    std::string stem = extractFilenameHint(mimeData, hintExt);
    if (stem.empty()) {
        stem = "image_" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss").toUtf8().toStdString();
    }

    // 2. uri data:image/... или байты изображения из браузера, чата с ИИ или буфера обмена
    QByteArray imageBytes;
    std::string imageExt = hintExt.empty() ? ".png" : hintExt;

    // 2a. проверка data:image/ в URL, HTML <img src> или обычном тексте
    if (mimeData->hasUrls()) {
        for (const auto& u : mimeData->urls()) {
            if (tryDecodeDataUri(u.toString(), imageBytes, imageExt)) break;
        }
    }
    if (imageBytes.isEmpty() && mimeData->hasHtml()) {
        QString src = extractHtmlImgAttribute(mimeData->html(), "src");
        tryDecodeDataUri(src, imageBytes, imageExt);
    }
    if (imageBytes.isEmpty() && mimeData->hasText()) {
        tryDecodeDataUri(mimeData->text(), imageBytes, imageExt);
    }

    // 2b. проверка прямых MIME форматов изображения (сохраняет исходные байты PNG/JPEG/WebP/GIF/SVG)
    if (imageBytes.isEmpty()) {
        struct MimePair {
            const char* mime;
            const char* ext;
        };
        static const MimePair kFormats[] = {
            {"image/svg+xml", ".svg"},
            {"image/gif",     ".gif"},
            {"image/webp",    ".webp"},
            {"image/png",     ".png"},
            {"image/jpeg",    ".jpg"},
            {"image/jpg",     ".jpg"},
            {"image/avif",    ".avif"},
            {"image/bmp",     ".bmp"}
        };

        // предпочитаем подходящий hintExt, если он доступен в mimeData
        if (!hintExt.empty()) {
            for (const auto& fp : kFormats) {
                if (hintExt == fp.ext && mimeData->hasFormat(QString::fromLatin1(fp.mime))) {
                    QByteArray candidate = mimeData->data(QString::fromLatin1(fp.mime));
                    if (!candidate.isEmpty()) {
                        imageBytes = std::move(candidate);
                        imageExt = fp.ext;
                        break;
                    }
                }
            }
        }
        // иначе выбираем первый доступный нативный формат изображения
        if (imageBytes.isEmpty()) {
            for (const auto& fp : kFormats) {
                if (mimeData->hasFormat(QString::fromLatin1(fp.mime))) {
                    QByteArray candidate = mimeData->data(QString::fromLatin1(fp.mime));
                    if (!candidate.isEmpty()) {
                        imageBytes = std::move(candidate);
                        imageExt = fp.ext;
                        break;
                    }
                }
            }
        }
    }

    // 2c. запасной вариант QMimeData::imageData() (QImage)
    if (imageBytes.isEmpty() && mimeData->hasImage()) {
        QImage img = qvariant_cast<QImage>(mimeData->imageData());
        if (!img.isNull()) {
            QBuffer buf(&imageBytes);
            buf.open(QIODevice::WriteOnly);
            img.save(&buf, "PNG");
            imageExt = ".png";
        }
    }

    if (!imageBytes.isEmpty()) {
        imageExt = detectExtensionFromMagicBytes(imageBytes, imageExt);
        std::filesystem::path dest = targetDir / (stem + imageExt);
        std::error_code ec;
        if (std::filesystem::exists(dest, ec)) {
            dest = generateCopyName(dest, targetDir);
        }

        QFile outFile(QString::fromUtf8(dest.c_str()));
        if (outFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            outFile.write(imageBytes);
            outFile.close();
            Core::UndoManager::instance().recordCreate(dest, "Drop Image");
            notifyDropCompletedOnUi(safeParent, dest, "Saved image", onFinished);
            return true;
        }
    }

    // 3. проверка удалённого URL http:// или https://, сброшенного из браузера (изображение или ссылка на файл)
    QString remoteUrlStr;
    if (mimeData->hasFormat("application/x-moz-file-promise-url")) {
        QByteArray d = mimeData->data("application/x-moz-file-promise-url");
        remoteUrlStr = decodeMimeText(d);
        remoteUrlStr = remoteUrlStr.trimmed();
    }
    if (remoteUrlStr.isEmpty() && mimeData->hasHtml()) {
        QString imgSrc = extractHtmlImgAttribute(mimeData->html(), "src");
        if (imgSrc.startsWith("http://", Qt::CaseInsensitive) || imgSrc.startsWith("https://", Qt::CaseInsensitive)) {
            remoteUrlStr = imgSrc;
        }
    }
    if (remoteUrlStr.isEmpty() && mimeData->hasUrls()) {
        for (const auto& u : mimeData->urls()) {
            if (u.scheme() == "http" || u.scheme() == "https") {
                remoteUrlStr = u.toString();
                break;
            }
        }
    }
    if (remoteUrlStr.isEmpty() && mimeData->hasText()) {
        QString t = mimeData->text().trimmed();
        if ((t.startsWith("http://", Qt::CaseInsensitive) || t.startsWith("https://", Qt::CaseInsensitive)) && !t.contains('\n')) {
            remoteUrlStr = t;
        }
    }

    if (!remoteUrlStr.isEmpty()) {
        enum class DownloadFormat {
            BestVideo,
            Video1080p,
            Video720p,
            AudioMp3,
            AudioFlac,
            DirectCurl
        };

        DownloadFormat chosenFormat = DownloadFormat::BestVideo;
        bool isDirectFileUrl = false;

        // прямой файл по расширению качаем через curl сразу, меню yt-dlp
        // только для страниц видеохостингов и url без расширения файла
        {
            QUrl u(remoteUrlStr);
            QString pathPart = u.path();
            int slash = pathPart.lastIndexOf('/');
            QString lastSeg = (slash >= 0) ? pathPart.mid(slash + 1) : pathPart;
            int dot = lastSeg.lastIndexOf('.');
            QString urlExt;
            if (dot > 0 && lastSeg.size() - dot >= 2 && lastSeg.size() - dot <= 6) {
                urlExt = lastSeg.mid(dot).toLower();
            }
            static const char* const kDirectExts[] = {
                ".png", ".jpg", ".jpeg", ".webp", ".gif", ".svg", ".bmp", ".avif", ".ico",
                ".pdf", ".txt", ".md", ".csv", ".json", ".xml",
                ".zip", ".tar", ".gz", ".bz2", ".xz", ".zst", ".7z", ".rar",
                ".mp3", ".flac", ".wav", ".ogg", ".m4a",
                ".mp4", ".mkv", ".avi", ".webm", ".mov",
                ".exe", ".dmg", ".iso", ".deb", ".rpm", ".epub"
            };
            for (const char* e : kDirectExts) {
                if (urlExt == QString::fromLatin1(e)) { isDirectFileUrl = true; break; }
            }
            // hint из имени распознал картинку, тоже прямой файл
            if (!isDirectFileUrl && !hintExt.empty()) {
                QString hx = QString::fromUtf8(hintExt.c_str()).toLower();
                if (hx == ".png" || hx == ".jpg" || hx == ".jpeg" || hx == ".webp" ||
                    hx == ".gif" || hx == ".svg" || hx == ".bmp" || hx == ".avif") {
                    isDirectFileUrl = true;
                }
            }
        }

        if (isDirectFileUrl) {
            chosenFormat = DownloadFormat::DirectCurl;
        } else if (parentWidget) {
            QMenu menu(parentWidget);
            menu.setFont(Theme::getMonospaceFont(9));
            menu.setStyleSheet(Theme::getApplicationStyleSheet());

            QString displayUrl = remoteUrlStr;
            if (displayUrl.size() > 40) {
                displayUrl = displayUrl.left(37) + "...";
            }
            auto* actTitle = menu.addAction(QString("Download: %1").arg(displayUrl));
            actTitle->setEnabled(false);
            menu.addSeparator();

            auto* actBest = menu.addAction("\U000F0208 Best Video (MP4)");
            auto* act1080 = menu.addAction("\U000F0208 1080p Video (MP4)");
            auto* act720  = menu.addAction("\U000F0208 720p Video (MP4)");
            menu.addSeparator();
            auto* actMp3  = menu.addAction("\U000F0382 Audio Only (MP3)");
            auto* actFlac = menu.addAction("\U000F0382 Audio Only (FLAC)");
            menu.addSeparator();
            auto* actDirect = menu.addAction("\U000F01DA Direct File Download (curl)");
            menu.addSeparator();
            auto* actCancel = menu.addAction("\U000F0156 Cancel");

            QAction* selected = menu.exec(QCursor::pos());
            if (!selected || selected == actCancel || selected == actTitle) {
                return false;
            }

            if (selected == actBest) chosenFormat = DownloadFormat::BestVideo;
            else if (selected == act1080) chosenFormat = DownloadFormat::Video1080p;
            else if (selected == act720) chosenFormat = DownloadFormat::Video720p;
            else if (selected == actMp3) chosenFormat = DownloadFormat::AudioMp3;
            else if (selected == actFlac) chosenFormat = DownloadFormat::AudioFlac;
            else if (selected == actDirect) chosenFormat = DownloadFormat::DirectCurl;
        }

        postStatusMessage(safeParent.data(), "Downloading...", true);

        QThreadPool::globalInstance()->start([remoteUrlStr, targetDir, stem, hintExt, chosenFormat, safeParent, onFinished]() {
            bool ok = false;
            std::filesystem::path finalDest;
            QString targetDirQ = QString::fromUtf8(targetDir.c_str());

                // 1. сначала yt-dlp, если он выбран
            if (chosenFormat != DownloadFormat::DirectCurl) {
                QStringList ytdlpArgs = {"-P", targetDirQ, "--no-playlist", "--no-warnings", "--print", "after_move:filepath"};
                if (chosenFormat == DownloadFormat::BestVideo) {
                    ytdlpArgs << "-f" << "bv*+ba/b" << "--merge-output-format" << "mp4";
                } else if (chosenFormat == DownloadFormat::Video1080p) {
                    ytdlpArgs << "-f" << "bv*[height<=1080]+ba/b[height<=1080]" << "--merge-output-format" << "mp4";
                } else if (chosenFormat == DownloadFormat::Video720p) {
                    ytdlpArgs << "-f" << "bv*[height<=720]+ba/b[height<=720]" << "--merge-output-format" << "mp4";
                } else if (chosenFormat == DownloadFormat::AudioMp3) {
                    ytdlpArgs << "-x" << "--audio-format" << "mp3";
                } else if (chosenFormat == DownloadFormat::AudioFlac) {
                    ytdlpArgs << "-x" << "--audio-format" << "flac";
                }
                ytdlpArgs << "--" << remoteUrlStr;

                QProcess ytdlpProc;
                ytdlpProc.start("yt-dlp", ytdlpArgs);
                if (ytdlpProc.waitForFinished(600000) && ytdlpProc.exitCode() == 0) {
                    QString outStr = QString::fromUtf8(ytdlpProc.readAllStandardOutput()).trimmed();
                    QStringList lines = outStr.split(u'\n', Qt::SkipEmptyParts);
                    for (int li = lines.size() - 1; li >= 0; --li) {
                        QString cand = lines[li].trimmed();
                        std::filesystem::path candPath(cand.toUtf8().toStdString());
                        std::error_code ec;
                        if (std::filesystem::exists(candPath, ec) && std::filesystem::file_size(candPath, ec) > 0) {
                            finalDest = candPath;
                            ok = true;
                            break;
                        }
                    }
                }
            }

            // 2. запасной вариант curl / wget, если yt-dlp не сработал или выбрана прямая загрузка
            if (!ok) {
                std::string dlExt = hintExt;
                if (dlExt.empty()) {
                    QUrl u(remoteUrlStr);
                    QString fn = u.fileName();
                    int dot = fn.lastIndexOf('.');
                    if (dot > 0 && fn.size() - dot <= 6) {
                        dlExt = fn.mid(dot).toLower().toUtf8().toStdString();
                    }
                }

                std::filesystem::path initialDest = targetDir / (stem + dlExt);
                std::error_code ec;
                if (std::filesystem::exists(initialDest, ec)) {
                    initialDest = generateCopyName(initialDest, targetDir);
                }

                Models::FileSystemModel::addInProgressPath(initialDest);
                QString destQ = QString::fromUtf8(initialDest.c_str());

                QProcess curlProc;
                curlProc.start("curl", {"-fsSL", "-L", "--max-time", "60", "-A", "Mozilla/5.0", "-o", destQ, "--", remoteUrlStr});
                if (curlProc.waitForFinished(65000) && curlProc.exitCode() == 0) {
                    ok = true;
                }
                if (!ok) {
                    QProcess wgetProc;
                    wgetProc.start("wget", {"-q", "--timeout=60", "-O", destQ, "--", remoteUrlStr});
                    if (wgetProc.waitForFinished(65000) && wgetProc.exitCode() == 0) {
                        ok = true;
                    }
                }

                Models::FileSystemModel::removeInProgressPath(initialDest);

                std::error_code fEc;
                if (!ok || !std::filesystem::exists(initialDest, fEc) || std::filesystem::file_size(initialDest, fEc) == 0) {
                    std::filesystem::remove(initialDest, fEc);
                    QMetaObject::invokeMethod(qApp, [safeParent]() {
                        postStatusMessage(safeParent.data(), "Download failed", true);
                    }, Qt::QueuedConnection);
                    return;
                }

                finalDest = initialDest;
                if (dlExt.empty()) {
                    QFile f(destQ);
                    if (f.open(QIODevice::ReadOnly)) {
                        QByteArray magic = f.read(256);
                        f.close();
                        std::string detected = detectExtensionFromMagicBytes(magic, "");
                        if (!detected.empty()) {
                            std::filesystem::path withExt = targetDir / (stem + detected);
                            if (std::filesystem::exists(withExt, fEc)) {
                                withExt = generateCopyName(withExt, targetDir);
                            }
                            std::filesystem::rename(initialDest, withExt, fEc);
                            if (!fEc) {
                                finalDest = withExt;
                            }
                        }
                    }
                }
            }

            Core::UndoManager::instance().recordCreate(finalDest, "Download");
            QMetaObject::invokeMethod(qApp, [safeParent, finalDest, onFinished]() {
                notifyDropCompletedOnUi(safeParent, finalDest, "Downloaded", onFinished);
            }, Qt::QueuedConnection);
        });

        return true;
    }

    return false;
}

void DragDropHandler::handleDrop(const std::vector<std::filesystem::path>& sources,
                                 const std::filesystem::path& targetDir,
                                 Qt::DropAction proposedAction,
                                 Qt::KeyboardModifiers modifiers,
                                 QWidget* parentWidget,
                                 std::function<void()> onFinished) {
    if (sources.empty() || targetDir.empty()) return;

    enum ActionType { ActCopy, ActMove, ActSymlink };
    ActionType chosenAction = ActCopy;

    bool showMicroMenu = (modifiers & Qt::AltModifier) || (proposedAction == Qt::LinkAction);

    if (showMicroMenu && parentWidget) {
        QMenu menu(parentWidget);
        menu.setStyleSheet(Theme::getApplicationStyleSheet());

        QAction* actCopy = menu.addAction("\U000F018F Copy Here");
        QAction* actMove = menu.addAction("\U000F0190 Move Here");
        QAction* actSymlink = menu.addAction("\U000F0337 Create Symlink");
        menu.addSeparator();
        QAction* actCancel = menu.addAction("✕ Cancel");

        QAction* selected = menu.exec(QCursor::pos());
        if (!selected || selected == actCancel) return;

        if (selected == actCopy) chosenAction = ActCopy;
        else if (selected == actMove) chosenAction = ActMove;
        else if (selected == actSymlink) chosenAction = ActSymlink;
    } else {
        if (modifiers & Qt::ControlModifier) {
            chosenAction = ActCopy;
        } else if (modifiers & Qt::ShiftModifier) {
            chosenAction = ActMove;
        } else if (proposedAction == Qt::CopyAction) {
            chosenAction = ActCopy;
        } else if (proposedAction == Qt::MoveAction) {
            chosenAction = ActMove;
        } else {
            chosenAction = ActCopy;
        }
    }

    // перенос между устройствами заменяем на копирование
    if (chosenAction == ActMove) {
        for (const auto& s : sources) {
            if (isDifferentFilesystem(s, targetDir)) {
                chosenAction = ActCopy;
                break;
            }
        }
    }

    QPointer<QWidget> safeParent(parentWidget);

    // асинхронное копирование, перенос или симлинк для разрешённых источников
    auto executeCopyMove = [targetDir, chosenAction, safeParent, onFinished](const std::vector<std::pair<std::filesystem::path, std::filesystem::path>>& itemsToProcess) {
        if (itemsToProcess.empty()) return;

        auto cancelToken = std::make_shared<std::atomic<bool>>(false);
        {
            std::lock_guard<std::mutex> lock(s_opMutex);
            // отменённая операция может ещё дорабатывать и гасит токен сама,
            // вторую не запускаем
            if (s_cancelToken) {
                QMetaObject::invokeMethod(qApp, []() {
                    postStatusMessage(nullptr, "Another file operation is in progress", true);
                }, Qt::QueuedConnection);
                return;
            }
            s_cancelToken = cancelToken;
        }

        for (const auto& [src, dest] : itemsToProcess) {
            Models::FileSystemModel::addInProgressPath(src);
            Models::FileSystemModel::addInProgressPath(dest);
        }

        uintmax_t totalBytes = 0;
        for (const auto& [src, _] : itemsToProcess) {
            totalBytes += contentSizeBytes(src);
        }

        QThreadPool::globalInstance()->start([items = itemsToProcess, chosenAction, safeParent, onFinished, totalBytes, cancelToken]() {
            std::vector<Core::UndoItem> undoItems;
            int count = 0;
            QString lastErr;
            std::filesystem::path firstDest;
            uintmax_t overallCopiedBytes = 0;
            const auto startTime = std::chrono::steady_clock::now();
            auto lastProgress = startTime - std::chrono::milliseconds(120);
            auto progressMessage = [&](uintmax_t doneBytes) {
                auto now = std::chrono::steady_clock::now();
                if (now - startTime < std::chrono::milliseconds(1500)) {
                    return;
                }
                int pct = totalBytes > 0
                    ? std::clamp(static_cast<int>(doneBytes * 100 / totalBytes), 0, 100)
                    : static_cast<int>(count * 100 / std::max<size_t>(1, items.size()));
                int filled = (pct * 8) / 100;
                QString bar = QString(filled, QChar(0x2588)) + QString(8 - filled, QChar(0x2591));
                QString progMsg = QString("%1 [%2] %3%").arg(chosenAction == ActCopy ? "Copy" : "Move", bar).arg(pct);
                QMetaObject::invokeMethod(qApp, [progMsg]() {
                    postStatusMessage(nullptr, progMsg, false);
                }, Qt::QueuedConnection);
            };

            for (size_t i = 0; i < items.size(); ++i) {
                if (cancelToken->load()) break;
                const auto& [src, dest] = items[i];
                if (src == dest && chosenAction != ActCopy) continue;
                if (firstDest.empty()) firstDest = dest;
                QString err;

                std::filesystem::path backup;
                std::error_code ec;
                if (std::filesystem::exists(dest, ec)) {
                    backup = Core::stashForUndo(dest);
                }

                auto progressCb = [&](uintmax_t itemCopied, uintmax_t /*itemTotal*/) {
                    if (cancelToken->load()) return;
                    auto now = std::chrono::steady_clock::now();
                    if (now - lastProgress < std::chrono::milliseconds(120)) return;
                    lastProgress = now;
                    progressMessage(overallCopiedBytes + itemCopied);
                };

                bool ok = false;
                if (chosenAction == ActCopy) {
                    ok = Core::XdgUtils::copyItem(src, dest, err, progressCb, cancelToken);
                } else if (chosenAction == ActMove) {
                    ok = Core::XdgUtils::moveItem(src, dest, err);
                } else if (chosenAction == ActSymlink) {
                    ok = Core::XdgUtils::createSymlink(src, dest, err);
                }

                if (cancelToken->load()) break;

                if (ok) {
                    // локальный undoItems полный список и для undo, и для отката отмены
                    // (включая элементы, докатившиеся после cancel)
                    undoItems.push_back({src, dest, backup});
                    count++;
                    overallCopiedBytes += contentSizeBytes(src);
                } else {
                    if (!backup.empty()) {
                        Core::restoreFromUndo(backup, dest);
                    }
                    lastErr = err;
                }

                auto now = std::chrono::steady_clock::now();
                if (now - lastProgress >= std::chrono::milliseconds(120)) {
                    lastProgress = now;
                    progressMessage(overallCopiedBytes);
                }
            }

            for (const auto& [src, dest] : items) {
                Models::FileSystemModel::removeInProgressPath(src);
                Models::FileSystemModel::removeInProgressPath(dest);
            }

            const bool wasCancelled = cancelToken->load();
            if (wasCancelled) {
                // откат выполняет воркер: undoItems полный список завершённых,
                // токен ещё жив и новая операция не стартует до конца отката
                rollbackCompletedItems(undoItems);
            }

            {
                std::lock_guard<std::mutex> lock(s_opMutex);
                // гасим токен только если это наша операция: у новой в
                // s_cancelToken уже лежит свой токен
                if (s_cancelToken == cancelToken) {
                    s_cancelToken.reset();
                }
            }

            if (wasCancelled) {
                QMetaObject::invokeMethod(qApp, [safeParent]() {
                    postStatusMessage(safeParent.data(), "Cancelled", true);
                    if (safeParent) {
                        if (auto* mvc = qobject_cast<Widgets::MillerViewContainer*>(safeParent.data())) {
                            mvc->refresh();
                        }
                    }
                }, Qt::QueuedConnection);
                return;
            }

            const bool hadError = !lastErr.isEmpty();

            QMetaObject::invokeMethod(qApp, [safeParent, onFinished, firstDest, chosenAction, undoItems = std::move(undoItems), lastErr, hadError]() {
                if (!undoItems.empty()) {
                    if (chosenAction == ActCopy) {
                        Core::UndoManager::instance().recordMultiCopy(undoItems, "Copy");
                    } else if (chosenAction == ActMove) {
                        Core::UndoManager::instance().recordMultiMove(undoItems, "Move");
                    } else if (chosenAction == ActSymlink) {
                        Core::UndoManager::instance().recordMultiCreate(undoItems, "Create");
                    }
                }
                if (safeParent) {
                    if (auto* mvc = qobject_cast<Widgets::MillerViewContainer*>(safeParent.data())) {
                        if (!hadError && !firstDest.empty()) {
                            mvc->selectPathAfterRefresh(firstDest);
                        }
                        mvc->refresh();
                    }
                }
                if (hadError) {
                    // частичный успех с ошибкой обязан быть видимым
                    postStatusMessage(safeParent.data(), lastErr, true);
                    if (safeParent) {
                        Widgets::TuiConfirmPopup::alert(safeParent.data(), "Operation Error", lastErr);
                    }
                    if (onFinished) onFinished();
                    return;
                }
                if (onFinished) {
                    onFinished();
                } else if (safeParent && safeParent->window()) {
                    for (auto* c : safeParent->window()->findChildren<Widgets::MillerViewContainer*>()) {
                        if (c != safeParent.data()) {
                            c->refresh();
                        }
                    }
                }
                QString msg = (chosenAction == ActCopy) ? "Copy" : ((chosenAction == ActMove) ? "Move" : "Create");
                postStatusMessage(safeParent.data(), msg, true);
            }, Qt::QueuedConnection);
        });
    };

    // разделяем элементы на неконфликтующие и конфликтующие
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> nonConflicts;
    std::vector<Widgets::ConflictItem> conflicts;

    for (const auto& src : sources) {
        if (src == targetDir) continue;

        // каталог внутрь себя уводит copyItem в бесконечную рекурсию,
        // пропускаем с сообщением иначе зависание и мусор на диске
        std::error_code dirEc;
        if (std::filesystem::is_directory(src, dirEc) && isSelfOrDescendant(targetDir, src)) {
            postStatusMessage(parentWidget, "Cannot drop a folder into itself", true);
            continue;
        }

        std::error_code eqEc;
        bool isSameDir = (src == targetDir / src.filename());
        if (!isSameDir) {
            isSameDir = std::filesystem::equivalent(src.parent_path(), targetDir, eqEc);
        }

        if (isSameDir) {
            // copy в ту же папку обязан создать копию с суффиксом stem_1.ext,
            // иначе paste молча ничего не делает (руководство §5.3E)
            if (chosenAction == ActCopy) {
                std::filesystem::path dest = generateCopyName(src, targetDir);
                nonConflicts.push_back({src, dest});
            }
            continue;
        }

        std::filesystem::path dest = targetDir / src.filename();
        std::error_code ec;
        if (std::filesystem::exists(dest, ec)) {
            conflicts.push_back({src, dest, true});
        } else {
            nonConflicts.push_back({src, dest});
        }
    }

    if (nonConflicts.empty() && conflicts.empty()) {
        return;
    }

    if (conflicts.empty()) {
        executeCopyMove(nonConflicts);
        return;
    }

    // разрешение конфликтов прямо в окне
    Widgets::PreviewPane* previewPane = nullptr;
    if (parentWidget) {
        previewPane = parentWidget->findChild<Widgets::PreviewPane*>();
        if (!previewPane && parentWidget->window()) {
            previewPane = parentWidget->window()->findChild<Widgets::PreviewPane*>();
        }
    }

    if (previewPane) {
        previewPane->showConflictResolver(conflicts,
            [nonConflicts, executeCopyMove](const std::vector<Widgets::ConflictItem>& resolved) {
                auto finalItems = nonConflicts;
                for (const auto& item : resolved) {
                    if (item.replace) {
                        finalItems.push_back({item.source, item.dest});
                    }
                }
                executeCopyMove(finalItems);
            });
    } else {
        // previewPane недостижим (дроп из сайдбара/табов/chooser):
        // skip пропускает конфликты, переносится только остальное
        const bool replaceAll = Widgets::TuiConfirmPopup::ask(
            parentWidget,
            "Replace existing items?",
            QString("%1 item(s) already exist at the destination.\nReplace them?")
                .arg(conflicts.size()),
            "Replace All", "Skip");
        auto allItems = nonConflicts;
        if (replaceAll) {
            for (const auto& c : conflicts) {
                allItems.push_back({c.source, c.dest});
            }
        }
        executeCopyMove(allItems);
    }
}

}
