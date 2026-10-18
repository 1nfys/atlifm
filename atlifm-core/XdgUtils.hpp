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
    QString id;          // например, "org.gnome.TextEditor.desktop"
    QString name;        // например, "Text Editor"
    QString execCommand; // например, "gnome-text-editor %U"
    QString iconName;
};

class XdgUtils {
public:
    // форматирует размер в байтах в удобочитаемую строку (например, 4.2 KB, 1.8 GB)
    static QString formatSize(uintmax_t bytes);

    // форматирует права доступа в формат rwxr-xr-x
    static QString formatPermissions(std::filesystem::perms p, bool isDirectory);

    // запрашивает тип MIME через QMimeDatabase
    static QString getMimeType(const std::filesystem::path& path);

    // запрашивает установленные приложения, поддерживающие заданный тип MIME или путь к файлу
    static std::vector<DesktopApp> getAppsForFile(const std::filesystem::path& path);

    // дефолтный обработчик: сначала xdg-mime query default, затем
    // [Default Applications] из mimeapps.list; пустой id значит не найден
    static DesktopApp getDefaultAppForFile(const std::filesystem::path& path);

    // точечно пишет mime=app.desktop в [Default Applications]
    // ~/.config/mimeapps.list, остальное содержимое сохраняется
    static bool setDefaultAppForFile(const std::filesystem::path& path,
                                     const DesktopApp& app,
                                     QString& outError);

    // запускает файл обработчиком рабочего стола по умолчанию (gio / xdg-open / QDesktopServices)
    static bool openWithDefault(const std::filesystem::path& path);

    // запускает файл указанным приложением рабочего стола
    static bool openWithApp(const DesktopApp& app, const std::filesystem::path& path);

    // приложение из произвольного пути: *.desktop разбирается как Desktop
    // Entry (id = имя файла), иначе прямой запуск; пустой execCommand непригоден
    static DesktopApp appFromFile(const std::filesystem::path& filePath);

    // определяет и запускает системный терминал в указанном каталоге
    static bool openTerminalAt(const std::filesystem::path& dirPath);
    static bool openFileWithSudo(const std::filesystem::path& path, QString& outError);

    // инлайн авторизация привилегированного действия (попап пароля):
    // живой тикет sudo → true молча, иначе пароль из TuiInputPopup и
    // sudoApplyPassword (строгая проверка + свежий тикет); правила sudoers
    // не создаются, для действия с одним системным диалогом есть pkexecArgs()
    static bool ensureSudoAuthenticated(QWidget* parentWidget, QString& outError);

    // pkexec установлен? (системный диалог полкита доступен)
    static bool polkitAvailable();

    // аргументы через живой тикет sudo: {"sudo","-n",...}; без тикета
    // команда падает сразу без диалога. Для разового действия с системным
    // диалогом pkexecArgs()
    static QStringList rootArgs(const QStringList& cmd);

    // аргументы через pkexec: {"pkexec",<full-path>,...}, т.к. pkexec
    // требует полный путь к программе; политика auth_admin без keep, каждый
    // pkexec это новый диалог пароля, поэтому вся работа root одного действия
    // укладывается в один вызов
    static QStringList pkexecArgs(const QStringList& cmd);

    // полный путь исполняемого только из доверенных каталогов (/usr/bin,
    // /usr/local/* и т.п.): пользовательский PATH не должен выбирать, какой
    // бинарник запустится от root; пусто если файл не найден
    static QString resolveTrustedExe(const QString& bare);

    // разовый запуск с ожиданием завершения, GUI жив (вложенный event loop
    // на системный диалог). true процесс завершился сам, false таймаут
    // или ошибка запуска (процесс убит)
    static bool runCommand(const QStringList& argv, int timeoutMs,
                           QByteArray* out = nullptr, QString* outErr = nullptr,
                           int* outExitCode = nullptr);

    // живой тикет sudo? true значит пароль спрашивать не нужно и `sudo -n`
    // уже работает
    static bool sudoTicketAlive(int timeoutMs = 2500, QString* outError = nullptr);

    // глобальный кэш листингов root (сырой вывод `find -printf ...`):
    // наполняется авторизованным листингом Sudo Open и sudoFileOp, фоновый
    // скан FileSystemModel читает его для закрытых каталогов
    static void provideSudoListing(const std::filesystem::path& dir, const QString& findOutput);
    // есть запись? (запись «пусто» тоже валидный ответ, поэтому bool+out)
    static bool sudoListing(const std::filesystem::path& dir, QString* out);

    // рекурсивный листинг root одним вызовом: разбирает вывод
    // `find ... -printf "%y\t%s\t%T@\t%m\t%p\0"`, группирует по родителям
    // и кладёт листинг для каждого каталога дерева; после одной авторизации
    // навигация работает на любую глубину без повторных диалогов pkexec
    static void provideSudoListingTree(const std::filesystem::path& root, const QString& recursiveOutput);
    // сброс поддерева кэша при мутации root, иначе после удаления или
    // переименования остаётся устаревший листинг
    static void invalidateSudoListingsUnder(const std::filesystem::path& dir);

    // один авторизованный рекурсивный листинг (дефолт maxdepth 5):
    // живой тикет → `sudo -n`, иначе один диалог pkexec, без полкита
    // инлайн пароль. Ok (пустой листинг значит каталог пуст),
    // Dismissed (126/127), NotAuthorized, Failed
    enum class SudoListStatus { Ok, Dismissed, NotAuthorized, Failed };
    static SudoListStatus sudoListTree(const std::filesystem::path& dir, int maxDepth,
                                       QString& outList, QString& outErr,
                                       QWidget* parentWidget = nullptr);

    // привилегированная файловая мутация в каталоге root: ровно один вызов
    // (живой тикет → `sudo -n`, иначе pkexec, иначе инлайн пароль только из
    // GUI потока). Тем же вызовом снимается свежий листинг find каталога
    // listingDir и кладётся в кэш, иначе refresh показал бы старые данные
    // targets: MakeDir/MakeFile один путь; Rename/Copy/Move {src, dst};
    // Remove все пути; ведущий «-» безопасно передаётся позиционным «$@»
    enum class PrivOp { MakeDir, MakeFile, Rename, Copy, Move, Remove };
    static bool sudoFileOp(PrivOp op,
                           const std::vector<std::filesystem::path>& targets,
                           const std::filesystem::path& listingDir,
                           QString& outError, QWidget* parentWidget = nullptr);

    // строгая проверка пароля sudo без попапа (инлайн поле Disk Usage):
    // `sudo -k` (сброс кэша, т.к. живой тикет «съел» бы неверный пароль)
    // → `sudo -S -v` (проверка и свежий тикет) → контроль тикета
    static bool sudoApplyPassword(const QString& password, QString& outError);

    static void cleanupSudoSession();

    // одна строка диагностики авторизации sudo в ~/.cache/atlifm/sudo-debug.log
    // (шаги, stderr sudo), пароли в лог не пишутся
    static void sudoDebugLog(const QString& line);

    // перемещает элементы в корзину FreeDesktop
    static bool moveToTrash(const std::vector<std::filesystem::path>& paths, QString& outError,
                            std::vector<std::pair<std::filesystem::path, std::filesystem::path>>* outMoved = nullptr);

    // удаляет элементы безвозвратно
    static bool deletePermanently(const std::vector<std::filesystem::path>& paths, QString& outError);
    static bool clearTrash();

    // создаёт архив (.tar.gz, .zip, .7z, и т.п.)
    static bool createArchive(const std::vector<std::filesystem::path>& sources,
                              const std::filesystem::path& destinationArchive,
                              QString& outError);

    // распаковывает архив в целевой каталог; если destinationFolder занят
    // файлом с тем же именем, берётся свободное имя «имя (2)», т.к. вывод
    // tar/7z на файл-назначение падает; реальный каталог возвращается через
    // outActualDestination. tar.* однопроходно bsdtar/tar, т.к. 7z x на
    // .tar.gz даёт лишь внутренний .tar
    static bool extractArchive(const std::filesystem::path& archivePath,
                               const std::filesystem::path& destinationFolder,
                               QString& outError,
                               std::filesystem::path* outActualDestination = nullptr);

    // архив ли это (по суффиксу): tar.*, zip, 7z, rar, jar, iso, gz, xz…
    static bool isArchive(const std::filesystem::path& path);

    // одиночный поток без внутреннего архива (.gz/.xz/.bz2/…): ровно одна
    // запись, выборочная распаковка невозможна и он всегда разворачивается
    // целиком
    static bool isSingleStreamArchive(const std::filesystem::path& path);

    // выборочная распаковка записей архива в каталог: tar.* через
    // bsdtar/tar `-xf arc -C dest -- entry…`, zip/7z через `7z x`,
    // одиночные потоки разворачиваются целиком. Имена записей как в
    // листинге (tar хранит «./sub/…», 7z/zip без префикса), ведущий «-»
    // защищается «./»
    static bool extractArchiveEntries(const std::filesystem::path& archivePath,
                                      const QStringList& entryPaths,
                                      const std::filesystem::path& destinationFolder,
                                      QString& outError);

    // имя папки для «Extract In»: a.tar.gz → a, b.zip → b, c → c
    static std::filesystem::path archiveExtractDirName(const std::filesystem::path& archivePath);

    struct ArchiveEntry {
        QString path;      // путь внутри архива
        quint64 size{0};   // байты (0 у каталогов)
        bool isDir{false};
        QString modified;  // как отдаёт листер (yyyy-MM-dd hh:mm:ss или Oct 17 00:40)
    };

    // листинг содержимого для превью: zip/7z/rar через `7z l -slt -ba`,
    // tar.* через `bsdtar -tvf`; список обрезается (truncated=true), чтобы
    // гигантские архивы не съедали память
    static bool listArchive(const std::filesystem::path& archivePath,
                            std::vector<ArchiveEntry>& outEntries,
                            bool& outTruncated, QString& outError);

    // вспомогательный обработчик копирования и перемещения файлов
    static bool copyItem(const std::filesystem::path& src, const std::filesystem::path& dest, QString& outError,
                         std::function<void(uintmax_t bytesCopied, uintmax_t totalBytes)> progressCallback = nullptr,
                         std::shared_ptr<std::atomic<bool>> cancelToken = nullptr);
    static bool moveItem(const std::filesystem::path& src, const std::filesystem::path& dest, QString& outError);
    static bool createSymlink(const std::filesystem::path& target, const std::filesystem::path& linkPath, QString& outError);
};

}
