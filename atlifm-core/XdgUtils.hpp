class QWidget;
#pragma once

#include <QString>
#include <QStringList>
#include <QByteArray>
#include <filesystem>
#include <vector>
#include <functional>
#include <atomic>
#include <memory>

namespace Core {

struct DesktopApp {
    QString id;
    QString name;
    QString execCommand;
    QString iconName;
};

class XdgUtils {
public:
    static QString formatSize(uintmax_t bytes);
    static QString formatPermissions(std::filesystem::perms p, bool isDirectory);
    static QString getMimeType(const std::filesystem::path& path);
    static std::vector<DesktopApp> getAppsForFile(const std::filesystem::path& path);

    // все установленные приложения из .desktop (NoDisplay/Hidden пропускаются)
    static std::vector<DesktopApp> getAllApps();

    // дефолтный хэндлер: сначала xdg mime query default а затем mimeapps list
    static DesktopApp getDefaultAppForFile(const std::filesystem::path& path);

    // пишет mime ассоциацию в default applications внутри mimeapps list
    static bool setDefaultAppForFile(const std::filesystem::path& path,
                                     const DesktopApp& app,
                                     QString& outError);

    static bool openWithDefault(const std::filesystem::path& path);
    static bool openWithApp(const DesktopApp& app, const std::filesystem::path& path);

    // desktop файл парсится как desktop entry иначе идет прямой запуск
    static DesktopApp appFromFile(const std::filesystem::path& filePath);

    static bool openTerminalAt(const std::filesystem::path& dirPath);
    static bool openFileWithSudo(const std::filesystem::path& path, QString& outError);

    // проверка привилегий: если тикет sudo живой то молча true иначе диалог пароля
    static bool ensureSudoAuthenticated(QWidget* parentWidget, QString& outError);

    static bool polkitAvailable();

    // аргументы через живой тикет sudo без диалога
    static QStringList rootArgs(const QStringList& cmd);

    // аргументы через pkexec где нужен полный путь к утилите
    static QStringList pkexecArgs(const QStringList& cmd);

    // берем бинарники только из системных bin чтобы не зависеть от юзерского path
    static QString resolveTrustedExe(const QString& bare);

    static bool runCommand(const QStringList& argv, int timeoutMs,
                           QByteArray* out = nullptr, QString* outErr = nullptr,
                           int* outExitCode = nullptr);

    static bool sudoTicketAlive(int timeoutMs = 2500, QString* outError = nullptr);

    // глобальный кэш рутового листинга от find
    static void provideSudoListing(const std::filesystem::path& dir, const QString& findOutput);
    static bool sudoListing(const std::filesystem::path& dir, QString* out);

    // рекурсивный рутовый листинг дерева за один вызов find
    static void provideSudoListingTree(const std::filesystem::path& root, const QString& recursiveOutput);
    static void invalidateSudoListingsUnder(const std::filesystem::path& dir);

    enum class SudoListStatus { Ok, Dismissed, NotAuthorized, Failed };
    static SudoListStatus sudoListTree(const std::filesystem::path& dir, int maxDepth,
                                       QString& outList, QString& outErr,
                                       QWidget* parentWidget = nullptr);

    // файловая операция под рутом и сразу обновление кэша листинга
    enum class PrivOp { MakeDir, MakeFile, Rename, Copy, Move, Remove };
    static bool sudoFileOp(PrivOp op,
                           const std::vector<std::filesystem::path>& targets,
                           const std::filesystem::path& listingDir,
                           QString& outError, QWidget* parentWidget = nullptr);

    // валидация пароля sudo через сброс кэша и проверку ключом S v
    static bool sudoApplyPassword(const QString& password, QString& outError);

    static void cleanupSudoSession();

    // отладочный лог авторизации в cache каталоге
    static void sudoDebugLog(const QString& line);

    static bool moveToTrash(const std::vector<std::filesystem::path>& paths, QString& outError,
                            std::vector<std::pair<std::filesystem::path, std::filesystem::path>>* outMoved = nullptr);

    // удаление без корзины с поддержкой отмены через токен
    static bool deletePermanently(const std::vector<std::filesystem::path>& paths, QString& outError,
                                  std::shared_ptr<std::atomic<bool>> cancelToken = nullptr);
    static bool clearTrash();

    static bool createArchive(const std::vector<std::filesystem::path>& sources,
                              const std::filesystem::path& destinationArchive,
                              QString& outError,
                              std::shared_ptr<std::atomic<bool>> cancelToken = nullptr);

    // распаковка архива и разрешение коллизий имен если папка уже занята
    static bool extractArchive(const std::filesystem::path& archivePath,
                               const std::filesystem::path& destinationFolder,
                               QString& outError,
                               std::filesystem::path* outActualDestination = nullptr,
                               std::shared_ptr<std::atomic<bool>> cancelToken = nullptr);

    static bool isArchive(const std::filesystem::path& path);
    static bool isSingleStreamArchive(const std::filesystem::path& path);

    // точечная распаковка выбранных файлов из архива
    static bool extractArchiveEntries(const std::filesystem::path& archivePath,
                                      const QStringList& entryPaths,
                                      const std::filesystem::path& destinationFolder,
                                      QString& outError,
                                      std::shared_ptr<std::atomic<bool>> cancelToken = nullptr);

    static std::filesystem::path archiveExtractDirName(const std::filesystem::path& archivePath);

    struct ArchiveEntry {
        QString path;
        quint64 size{0};
        bool isDir{false};
        QString modified;
    };

    // листинг файлов архива для превью с защитой от переполнения памяти
    static bool listArchive(const std::filesystem::path& archivePath,
                            std::vector<ArchiveEntry>& outEntries,
                            bool& outTruncated, QString& outError);

    static bool copyItem(const std::filesystem::path& src, const std::filesystem::path& dest, QString& outError,
                         std::function<void(uintmax_t bytesCopied, uintmax_t totalBytes)> progressCallback = nullptr,
                         std::shared_ptr<std::atomic<bool>> cancelToken = nullptr);
    static bool moveItem(const std::filesystem::path& src, const std::filesystem::path& dest, QString& outError,
                         std::function<void(uintmax_t bytesCopied, uintmax_t totalBytes)> progressCallback = nullptr,
                         std::shared_ptr<std::atomic<bool>> cancelToken = nullptr);
    static bool createSymlink(const std::filesystem::path& target, const std::filesystem::path& linkPath, QString& outError);
};

}
