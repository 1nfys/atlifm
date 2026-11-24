#pragma once

#include <QString>
#include <QUrl>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QRegularExpression>
#include <vector>
#include <unordered_set>
#include <filesystem>
#include <cstdlib>

namespace Core {

enum class BookmarkCategory {
    Place,
    Drive,
    UserBookmark
};

struct BookmarkItem {
    QString title;
    std::filesystem::path path;
    QString iconGlyph;
    QChar jumpKey{' '};
    BookmarkCategory category{BookmarkCategory::Place};
    QString extraInfo;
    bool isSystemPlace{false};
};

class BookmarksManager {
public:
    static std::vector<BookmarkItem> loadPlaces() {
        std::vector<BookmarkItem> list;

        const char* homeEnv = std::getenv("HOME");
        std::filesystem::path homePath = homeEnv ? homeEnv : "/";

        list.push_back(BookmarkItem{"Home", homePath, "\uf015", 'h', BookmarkCategory::Place, "~", true});

        std::filesystem::path trashFilesDir = std::filesystem::path(homeEnv ? homeEnv : "/") / ".local/share/Trash/files";
        std::error_code ec;
        std::filesystem::create_directories(trashFilesDir, ec);
        list.push_back(BookmarkItem{"Bin", trashFilesDir, "\uf1f8", 'b', BookmarkCategory::Place, "", true});

        // стандартные папки
        auto addIfMissing = [&](QStandardPaths::StandardLocation loc, const QString& name, const QString& icon, QChar key) {
            QString p = QStandardPaths::writableLocation(loc);
            if (!p.isEmpty()) {
                std::filesystem::path fsPath(p.toUtf8().toStdString());
                std::error_code existsEc;
                if (std::filesystem::exists(fsPath, existsEc)) {
                    for (const auto& existing : list) {
                        if (existing.path == fsPath) return;
                    }
                    list.push_back(BookmarkItem{name, fsPath, icon, key, BookmarkCategory::Place, "", true});
                }
            }
        };

        addIfMissing(QStandardPaths::DesktopLocation, "Desktop", "\uf108", 'D');
        addIfMissing(QStandardPaths::DocumentsLocation, "Documents", "\uf401", 'd');
        addIfMissing(QStandardPaths::DownloadLocation, "Downloads", "\uf498", 'w');
        addIfMissing(QStandardPaths::MusicLocation, "Music", "\uf025", 'm');
        addIfMissing(QStandardPaths::PicturesLocation, "Pictures", "\ue244", 'P');
        addIfMissing(QStandardPaths::MoviesLocation, "Videos", "\uf447", 'v');

        return list;
    }

    static std::vector<BookmarkItem> loadDrives() {
        struct DriveEntry {
            BookmarkItem item;
            QString baseTitle;
            QString devPath;
            QString devName;
            QString parentDisk;
            QString subvolume;
            QString mountFolder;
        };

        std::vector<DriveEntry> entries;

        for (const auto& storage : QStorageInfo::mountedVolumes()) {
            if (!storage.isValid() || !storage.isReady()) continue;

            QString root = storage.rootPath();
            if (root.isEmpty()) continue;

            QString fsType = QString::fromLatin1(storage.fileSystemType()).toLower();
            if (fsType == "devtmpfs" || fsType == "tmpfs" || fsType == "efivarfs" ||
                fsType == "proc" || fsType == "sysfs" || fsType == "cgroup" ||
                fsType == "securityfs" || fsType == "configfs" || fsType == "debugfs") {
                continue;
            }

            // берем только корень и подключенные диски
            if (root != "/" && !root.startsWith("/run/media") && !root.startsWith("/media") && !root.startsWith("/mnt")) {
                continue;
            }

            BookmarkItem item;
            item.path = std::filesystem::path(root.toUtf8().toStdString());
            item.category = BookmarkCategory::Drive;
            item.isSystemPlace = true;

            qint64 freeBytes = storage.bytesAvailable();
            double freeGb = static_cast<double>(freeBytes) / (1024.0 * 1024.0 * 1024.0);
            if (freeGb >= 1.0) {
                item.extraInfo = QString("%1 GB free").arg(QString::number(freeGb, 'f', 0));
            } else {
                double freeMb = static_cast<double>(freeBytes) / (1024.0 * 1024.0);
                item.extraInfo = QString("%1 MB free").arg(QString::number(freeMb, 'f', 0));
            }

            qint64 totalBytes = storage.bytesTotal();
            double totalGb = static_cast<double>(totalBytes) / (1024.0 * 1024.0 * 1024.0);

            if (root == "/") {
                item.title = "Root (/)";
                item.iconGlyph = "\uf0a0";
                item.jumpKey = '/';
            } else {
                QString name = storage.displayName();
                QString rawFolder = QString::fromUtf8(item.path.filename().c_str());

                // если том назван сырым хексом то выводим размер
                static const QRegularExpression uuidRegex("^[0-9A-Fa-f]{8,}$");
                bool isRawUuid = uuidRegex.match(name).hasMatch() || uuidRegex.match(rawFolder).hasMatch();

                if (isRawUuid || name.isEmpty() || name == root) {
                    if (totalGb >= 1.0) {
                        item.title = QString("%1 GB Volume").arg(QString::number(totalGb, 'f', 0));
                    } else {
                        item.title = "Volume";
                    }
                } else {
                    item.title = name;
                }

                item.iconGlyph = "\uf0a0";
                item.jumpKey = (!item.title.isEmpty()) ? item.title.front().toLower() : 'd';
            }

            QString devPath = QString::fromUtf8(storage.device());
            QString devName;
            QString parentDisk;

            if (devPath.startsWith("/dev/")) {
                devName = devPath.mid(5);
                static const QRegularExpression nvmeRegex("^(nvme\\d+n\\d+|mmcblk\\d+)p(\\d+)$");
                static const QRegularExpression sdRegex("^([a-z]+)(\\d+)$");
                auto mNvme = nvmeRegex.match(devName);
                if (mNvme.hasMatch()) {
                    parentDisk = mNvme.captured(1);
                } else {
                    auto mSd = sdRegex.match(devName);
                    if (mSd.hasMatch()) {
                        parentDisk = mSd.captured(1);
                    }
                }
            }

            QString subvolume = QString::fromUtf8(storage.subvolume());
            QString mountFolder = QString::fromUtf8(item.path.filename().c_str());

            // дедупликация точных дубликатов того же пути или того же раздела и подтома
            bool isDuplicate = false;
            for (auto& existing : entries) {
                if (existing.item.path == item.path) {
                    isDuplicate = true;
                    break;
                }
                if (!devPath.isEmpty() && devPath.startsWith("/dev/") &&
                    existing.devPath == devPath && existing.subvolume == subvolume) {
                    // точный дубликат отдаем предпочтение стандартным путям монтирования перед mnt
                    if ((root.startsWith("/run/media") || root.startsWith("/media")) &&
                        existing.item.path.string().rfind("/mnt", 0) == 0) {
                        existing.item.path = item.path;
                        existing.item.extraInfo = item.extraInfo;
                        existing.mountFolder = mountFolder;
                    }
                    isDuplicate = true;
                    break;
                }
            }

            if (!isDuplicate) {
                QString baseTitle = item.title;
                entries.push_back(DriveEntry{std::move(item), baseTitle, devPath, devName, parentDisk, subvolume, mountFolder});
            }
        }

        // подсчет смонтированных подтомов одного устройства
        std::unordered_map<std::string, int> devCounts;
        // подсчет разделов одного диска
        std::unordered_map<std::string, int> parentCounts;
        // подсчет одинаковых названий
        std::unordered_map<std::string, int> titleCounts;

        for (const auto& e : entries) {
            if (!e.devPath.isEmpty()) {
                devCounts[e.devPath.toStdString()]++;
            }
            if (!e.parentDisk.isEmpty()) {
                parentCounts[e.parentDisk.toStdString()]++;
            }
            titleCounts[e.baseTitle.toStdString()]++;
        }

        // уточнение названий чтобы не было одинаковых дисков
        std::vector<BookmarkItem> list;
        list.reserve(entries.size());
        for (auto& e : entries) {
            if (e.item.path != "/") {
                // 1. если один и тот же том смонтирован в разные точки например подтома btrfs
                if (!e.devPath.isEmpty() && devCounts[e.devPath.toStdString()] > 1) {
                    QString tag = e.subvolume;
                    if (tag.startsWith('/') && tag.length() > 1) {
                        tag = tag.mid(1);
                    } else if (tag == "/" || tag.isEmpty()) {
                        tag = e.mountFolder;
                    }
                    e.item.title = QString("%1 (%2)").arg(e.baseTitle, tag);
                }
                // 2. если разные разделы одного диска например sdb1 и sdb2
                else if (!e.parentDisk.isEmpty() && parentCounts[e.parentDisk.toStdString()] > 1) {
                    e.item.title = QString("%1 (%2)").arg(e.baseTitle, e.devName);
                }
                // 3. если одинаковые названия на разных дисках
                else if (titleCounts[e.baseTitle.toStdString()] > 1 && !e.devName.isEmpty()) {
                    e.item.title = QString("%1 (%2)").arg(e.baseTitle, e.devName);
                }
                e.item.jumpKey = (!e.item.title.isEmpty()) ? e.item.title.front().toLower() : 'd';
            }
            list.push_back(std::move(e.item));
        }

        return list;
    }

    static std::vector<BookmarkItem> loadUserBookmarks(
        const std::vector<BookmarkItem>* preloadedPlaces = nullptr,
        const std::vector<BookmarkItem>* preloadedDrives = nullptr) {
        std::vector<BookmarkItem> list;
        const char* homeEnv = std::getenv("HOME");
        if (!homeEnv) return list;

        std::vector<BookmarkItem> localPlaces;
        std::vector<BookmarkItem> localDrives;
        const auto& places = preloadedPlaces ? *preloadedPlaces : (localPlaces = loadPlaces());
        const auto& drives = preloadedDrives ? *preloadedDrives : (localDrives = loadDrives());

        std::unordered_set<std::string> knownPaths;
        for (const auto& p : places) {
            std::error_code ec;
            auto canon = std::filesystem::canonical(p.path, ec);
            if (!ec) knownPaths.insert(canon.string());
            knownPaths.insert(p.path.string());
        }
        for (const auto& d : drives) {
            std::error_code ec;
            auto canon = std::filesystem::canonical(d.path, ec);
            if (!ec) knownPaths.insert(canon.string());
            knownPaths.insert(d.path.string());
        }

        QString homeQ = QString::fromUtf8(homeEnv);
        QStringList candidatePaths = {
            QString("%1/.config/gtk-4.0/bookmarks").arg(homeQ),
            QString("%1/.config/gtk-3.0/bookmarks").arg(homeQ)
        };

        for (const auto& configPath : candidatePaths) {
            QFile file(configPath);
            if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QTextStream in(&file);
                while (!in.atEnd()) {
                    QString line = in.readLine().trimmed();
                    if (line.isEmpty() || line.startsWith('#')) continue;

                    QStringList parts = line.split(' ', Qt::SkipEmptyParts);
                    if (!parts.isEmpty()) {
                        QUrl url(parts.first());
                        if (url.isLocalFile()) {
                            std::filesystem::path p(url.toLocalFile().toUtf8().toStdString());
                            std::error_code ec;
                            if (std::filesystem::exists(p, ec) && std::filesystem::is_directory(p, ec)) {
                                auto canonPath = std::filesystem::canonical(p, ec);
                                std::string canon = ec ? p.string() : canonPath.string();
                                if (knownPaths.find(canon) != knownPaths.end() ||
                                    knownPaths.find(p.string()) != knownPaths.end()) {
                                    continue;
                                }

                                QString label = parts.size() > 1 ? parts.mid(1).join(' ') : QString::fromUtf8(p.filename().c_str());
                                QChar key = (!label.isEmpty()) ? label.front().toLower() : 'b';

                                bool exists = false;
                                for (const auto& b : list) {
                                    if (b.path == p) {
                                        exists = true;
                                        break;
                                    }
                                }
                                if (!exists) {
                                    knownPaths.insert(canon);
                                    list.push_back(BookmarkItem{label, p, "\ue5ff", key, BookmarkCategory::UserBookmark, "", false});
                                }
                            }
                        }
                    }
                }
                file.close();
                break;
            }
        }

        return list;
    }

    static std::vector<BookmarkItem> loadAll() {
        auto places = loadPlaces();
        auto drives = loadDrives();
        auto userBms = loadUserBookmarks(&places, &drives);

        std::vector<BookmarkItem> all;
        all.reserve(places.size() + drives.size() + userBms.size());
        all.insert(all.end(), places.begin(), places.end());
        all.insert(all.end(), drives.begin(), drives.end());
        all.insert(all.end(), userBms.begin(), userBms.end());
        return all;
    }

    static void addBookmark(const std::filesystem::path& path, const QString& title = "") {
        const char* homeEnv = std::getenv("HOME");
        if (!homeEnv) return;

        // пишем в файл закладок гтк чтобы ничего не терялось
        std::filesystem::path homePath(homeEnv);
        std::filesystem::path configDir = homePath / ".config/gtk-4.0";
        std::error_code ec;
        if (!std::filesystem::exists(configDir / "bookmarks", ec)) {
            const std::filesystem::path gtk3Dir = homePath / ".config/gtk-3.0";
            if (std::filesystem::exists(gtk3Dir / "bookmarks", ec)) {
                configDir = gtk3Dir;
            } else {
                std::filesystem::create_directories(configDir, ec);
            }
        }

        QString targetConfig = QString::fromUtf8((configDir / "bookmarks").c_str());
        QString fileUrl = QUrl::fromLocalFile(QString::fromUtf8(path.c_str())).toString(QUrl::FullyEncoded);

        {
            QFile readFile(targetConfig);
            if (readFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QTextStream in(&readFile);
                while (!in.atEnd()) {
                    QString existingLine = in.readLine().trimmed();
                    if (existingLine.isEmpty()) continue;
                    QStringList parts = existingLine.split(' ', Qt::SkipEmptyParts);
                    if (!parts.isEmpty()) {
                        QUrl u(parts.first());
                        if (u.isLocalFile() &&
                            std::filesystem::path(u.toLocalFile().toUtf8().toStdString()) == path) {
                            return;
                        }
                    }
                }
            }
        }

        QFile file(targetConfig);
        if (file.open(QIODevice::Append | QIODevice::Text)) {
            QTextStream out(&file);
            QString line = fileUrl;
            // убираем переносы чтобы не сломать файл
            QString cleanTitle = title;
            cleanTitle.replace('\n', ' ');
            cleanTitle.replace('\r', ' ');
            cleanTitle = cleanTitle.trimmed();
            if (!cleanTitle.isEmpty()) {
                line += " " + cleanTitle;
            }
            out << line << "\n";
            file.close();
        }
    }

    static void removeBookmark(const std::filesystem::path& path) {
        const char* homeEnv = std::getenv("HOME");
        if (!homeEnv) return;

        QString homeQ = QString::fromUtf8(homeEnv);
        QStringList candidatePaths = {
            QString("%1/.config/gtk-4.0/bookmarks").arg(homeQ),
            QString("%1/.config/gtk-3.0/bookmarks").arg(homeQ)
        };

        std::string pathStr = path.string();

        for (const auto& configPath : candidatePaths) {
            QFile file(configPath);
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) continue;

            QStringList lines;
            QTextStream in(&file);
            while (!in.atEnd()) {
                QString line = in.readLine();
                if (line.trimmed().isEmpty()) continue;
                QStringList parts = line.trimmed().split(' ', Qt::SkipEmptyParts);
                if (!parts.isEmpty()) {
                    QUrl url(parts.first());
                    if (url.isLocalFile() && url.toLocalFile().toUtf8().toStdString() == pathStr) {
                        continue;
                    }
                }
                lines << line;
            }
            file.close();

            if (file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
                QTextStream out(&file);
                for (const auto& l : lines) {
                    out << l << "\n";
                }
                file.close();
            }
        }
    }
};

}
