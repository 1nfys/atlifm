#pragma once

#include <QString>
#include <vector>
#include <filesystem>
#include <utility>
#include <mutex>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <unistd.h>
#include <QStandardPaths>
#include <QFile>
#include <signal.h>
#include <sys/types.h>
#include <cerrno>
#include "XdgUtils.hpp"

namespace Core {

enum class UndoOpType {
    Move,       // перемещение из src в dest (или переименование)
    Copy,       // копирование из src в dest
    Create,     // создание нового файла, папки, архива или извлечённого элемента в dest
    Trash       // удаление в корзину из src в dest (dest это корзина)
};

struct UndoItem {
    std::filesystem::path src;        // исходный путь
    std::filesystem::path dest;       // путь назначения или создания
    std::filesystem::path backupPath; // куда отложен заменённый элемент, если dest перезаписал существующий
};

struct UndoAction {
    UndoOpType type{UndoOpType::Move};
    std::vector<UndoItem> items;
    QString description;
};

inline std::filesystem::path undoBackupsBaseDir() {
    const char* home = std::getenv("HOME");
    if (home) {
        return std::filesystem::path(home) / ".cache/atlifm/undo_backups";
    }
    return std::filesystem::temp_directory_path() / "atlifm_undo_backups";
}

inline std::filesystem::path sessionBackupRootDir() {
    static std::filesystem::path s_dir = []() {
        std::error_code ec;
        std::filesystem::path base = undoBackupsBaseDir();
        pid_t pid = ::getpid();
        std::filesystem::path sessionDir = base / ("session_" + std::to_string(pid));
        std::filesystem::create_directories(sessionDir, ec);
        return sessionDir;
    }();
    return s_dir;
}

// сохраняем файл перед перезаписью
inline std::filesystem::path stashForUndo(const std::filesystem::path& target) {
    std::error_code ec;

    // если папка закрыта то stat врет и проверяем доступность
    const auto parentDir = target.parent_path().empty() ? std::filesystem::path("/")
                                                        : target.parent_path();
    if (!std::filesystem::exists(target, ec) && ::access(parentDir.c_str(), R_OK | X_OK) == 0) {
        return {};
    }

    static std::atomic<uint64_t> s_counter{1};
    auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    std::string uniqueFolder = std::to_string(timestamp) + "_" + std::to_string(s_counter.fetch_add(1));

    std::filesystem::path backupDir = sessionBackupRootDir() / uniqueFolder;
    std::filesystem::create_directories(backupDir, ec);

    std::filesystem::path backupPath = backupDir / target.filename();

    // пробуем быстрый перенос
    std::filesystem::rename(target, backupPath, ec);
    if (ec) {
        if (ec == std::errc::permission_denied || ec == std::errc::operation_not_permitted) {
            // если файл под рутом то двигаем через sudo и чистим временную папку
            std::error_code cleanEc;
            std::filesystem::remove_all(backupPath, cleanEc);
            QString sudoErr;
            if (!XdgUtils::sudoFileOp(XdgUtils::PrivOp::Move, {target, backupPath}, parentDir, sudoErr)) {
                return {};
            }
        } else {
            // при переносе между файловыми системами сначала копируем и только потом удаляем
            QString err;
            if (!XdgUtils::copyItem(target, backupPath, err)) {
                return {};
            }
            std::error_code rmEc;
            std::filesystem::remove_all(target, rmEc);
            // удаление через sudo если не хватило прав
            if (rmEc == std::errc::permission_denied || rmEc == std::errc::operation_not_permitted) {
                QString sudoErr;
                XdgUtils::sudoFileOp(XdgUtils::PrivOp::Remove, {target}, parentDir, sudoErr);
            }
        }
    }
    if (!std::filesystem::exists(backupPath, ec)) {
        return {};
    }
    return backupPath;
}

// возвращаем сохраненный файл на место
inline bool restoreFromUndo(const std::filesystem::path& backupPath, const std::filesystem::path& target) {
    std::error_code ec;
    if (backupPath.empty() || !std::filesystem::exists(backupPath, ec)) {
        return false;
    }
    if (std::filesystem::exists(target, ec)) {
        std::error_code rmEc;
        std::filesystem::remove_all(target, rmEc);
        // удаление через sudo если цель под рутом
        if (rmEc == std::errc::permission_denied || rmEc == std::errc::operation_not_permitted) {
            QString sudoErr;
            XdgUtils::sudoFileOp(XdgUtils::PrivOp::Remove, {target},
                                 target.parent_path().empty() ? std::filesystem::path("/") : target.parent_path(),
                                 sudoErr);
        }
    }
    std::filesystem::rename(backupPath, target, ec);
    if (ec) {
        QString err;
        if (!XdgUtils::copyItem(backupPath, target, err)) {
            return false; // бэкап не теряем, иначе undo/redo останется без данных
        }
        std::filesystem::remove_all(backupPath, ec);
    }
    return true;
}

class UndoManager {
public:
    static UndoManager& instance() {
        static UndoManager s_inst;
        return s_inst;
    }

    void recordMove(const std::filesystem::path& from, const std::filesystem::path& to,
                    const std::filesystem::path& backup = {}, const QString& desc = "") {
        std::lock_guard<std::mutex> lock(m_mutex);
        UndoAction act;
        act.type = UndoOpType::Move;
        act.items.push_back({from, to, backup});
        act.description = desc.isEmpty() ? QString("Move %1").arg(QString::fromUtf8(from.filename().c_str())) : desc;
        m_history.push_back(std::move(act));
        m_redoStack.clear();
    }

    void recordMultiMove(const std::vector<UndoItem>& moves, const QString& desc = "") {
        if (moves.empty()) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        UndoAction act;
        act.type = UndoOpType::Move;
        act.items = moves;
        act.description = desc.isEmpty() ? QString("Move %1 items").arg(moves.size()) : desc;
        m_history.push_back(std::move(act));
        m_redoStack.clear();
    }

    
    void recordMultiMove(const std::vector<std::pair<std::filesystem::path, std::filesystem::path>>& moves, const QString& desc = "") {
        if (moves.empty()) return;
        std::vector<UndoItem> items;
        for (const auto& [to, from] : moves) {
            items.push_back({from, to, {}});
        }
        recordMultiMove(items, desc);
    }

    void recordCopy(const std::filesystem::path& from, const std::filesystem::path& to,
                    const std::filesystem::path& backup = {}, const QString& desc = "") {
        std::lock_guard<std::mutex> lock(m_mutex);
        UndoAction act;
        act.type = UndoOpType::Copy;
        act.items.push_back({from, to, backup});
        act.description = desc.isEmpty() ? QString("Copy %1").arg(QString::fromUtf8(from.filename().c_str())) : desc;
        m_history.push_back(std::move(act));
        m_redoStack.clear();
    }

    void recordMultiCopy(const std::vector<UndoItem>& copies, const QString& desc = "") {
        if (copies.empty()) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        UndoAction act;
        act.type = UndoOpType::Copy;
        act.items = copies;
        act.description = desc.isEmpty() ? QString("Copy %1 items").arg(copies.size()) : desc;
        m_history.push_back(std::move(act));
        m_redoStack.clear();
    }

    void recordCreate(const std::filesystem::path& created, const QString& desc = "") {
        std::lock_guard<std::mutex> lock(m_mutex);
        UndoAction act;
        act.type = UndoOpType::Create;
        act.items.push_back({{}, created, {}});
        act.description = desc.isEmpty() ? QString("Create %1").arg(QString::fromUtf8(created.filename().c_str())) : desc;
        m_history.push_back(std::move(act));
        m_redoStack.clear();
    }

    void recordMultiCreate(const std::vector<UndoItem>& items, const QString& desc = "") {
        if (items.empty()) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        UndoAction act;
        act.type = UndoOpType::Create;
        act.items = items;
        act.description = desc.isEmpty() ? QString("Create %1 items").arg(items.size()) : desc;
        m_history.push_back(std::move(act));
        m_redoStack.clear();
    }

    void recordTrash(const std::filesystem::path& orig, const std::filesystem::path& inTrash, const QString& desc = "") {
        std::lock_guard<std::mutex> lock(m_mutex);
        UndoAction act;
        act.type = UndoOpType::Trash;
        act.items.push_back({orig, inTrash, {}});
        act.description = desc.isEmpty() ? QString("Trash %1").arg(QString::fromUtf8(orig.filename().c_str())) : desc;
        m_history.push_back(std::move(act));
        m_redoStack.clear();
    }

    void recordMultiTrash(const std::vector<UndoItem>& items, const QString& desc = "") {
        if (items.empty()) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        UndoAction act;
        act.type = UndoOpType::Trash;
        act.items = items;
        act.description = desc.isEmpty() ? QString("Trash %1 items").arg(items.size()) : desc;
        m_history.push_back(std::move(act));
        m_redoStack.clear();
    }

    bool canUndo() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return !m_history.empty();
    }

    bool canRedo() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return !m_redoStack.empty();
    }

    bool undo(QString& outMessage, std::filesystem::path* outSelectPath = nullptr) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_history.empty()) {
            outMessage = "Nothing to undo";
            return false;
        }

        auto last = std::move(m_history.back());
        m_history.pop_back();

        if (outSelectPath && !last.items.empty()) {
            *outSelectPath = last.items.front().src.empty() ? last.items.front().dest : last.items.front().src;
        }

        int successCount = 0;
        std::vector<UndoItem> redoItems;

        if (last.type == UndoOpType::Move) {
            for (const auto& item : last.items) {
                // перенос файла без лишней проверки существования
                QString err;
                if (XdgUtils::moveItem(item.dest, item.src, err)) {
                    successCount++;
                }
                // восстанавливаем замененный файл если был бэкап
                if (!item.backupPath.empty()) {
                    restoreFromUndo(item.backupPath, item.dest);
                }
                redoItems.push_back({item.src, item.dest, item.backupPath});
            }
            if (successCount == 0 && !last.items.empty()) {
                outMessage = "Undo failed";
                // сохраняем действие для повторной попытки
                m_history.push_back(std::move(last));
                return false;
            }
            UndoAction redoAct;
            redoAct.type = UndoOpType::Move;
            redoAct.items = redoItems;
            redoAct.description = last.description;
            m_redoStack.push_back(std::move(redoAct));

            outMessage = "Undo";
            return true;
        }
        else if (last.type == UndoOpType::Copy) {
            for (const auto& item : last.items) {
                // удаление скопированного файла
                std::error_code rmEc;
                std::filesystem::remove_all(item.dest, rmEc);
                bool gone = !rmEc;
                if (!gone && (rmEc == std::errc::permission_denied || rmEc == std::errc::operation_not_permitted)) {
                    QString sudoErr;
                    gone = XdgUtils::sudoFileOp(XdgUtils::PrivOp::Remove, {item.dest},
                                                item.dest.parent_path().empty() ? std::filesystem::path("/") : item.dest.parent_path(),
                                                sudoErr);
                }
                if (gone) {
                    successCount++;
                }
                // восстанавливаем замененный файл если был бэкап
                if (!item.backupPath.empty()) {
                    restoreFromUndo(item.backupPath, item.dest);
                }
                redoItems.push_back({item.src, item.dest, item.backupPath});
            }
            if (successCount == 0 && !last.items.empty()) {
                outMessage = "Undo failed";
                // сохраняем действие для повторной попытки
                m_history.push_back(std::move(last));
                return false;
            }
            UndoAction redoAct;
            redoAct.type = UndoOpType::Copy;
            redoAct.items = redoItems;
            redoAct.description = last.description;
            m_redoStack.push_back(std::move(redoAct));

            // выбираем прошлый скопированный файл
            if (outSelectPath) {
                for (auto it = m_history.rbegin(); it != m_history.rend(); ++it) {
                    if (it->type == UndoOpType::Copy && !it->items.empty()) {
                        std::error_code ec;
                        if (std::filesystem::exists(it->items.front().dest, ec)) {
                            *outSelectPath = it->items.front().dest;
                            break;
                        }
                    }
                }
            }

            outMessage = "Undo";
            return true;
        }
        else if (last.type == UndoOpType::Create) {
            for (auto item : last.items) {
                // сохраняем файл перед удалением
                item.backupPath = stashForUndo(item.dest);
                if (!item.backupPath.empty()) {
                    redoItems.push_back(item);
                    successCount++;
                }
            }
            if (successCount == 0 && !last.items.empty()) {
                outMessage = "Undo failed";
                // сохраняем действие для повторной попытки
                m_history.push_back(std::move(last));
                return false;
            }
            UndoAction redoAct;
            redoAct.type = UndoOpType::Create;
            redoAct.items = redoItems;
            redoAct.description = last.description;
            m_redoStack.push_back(std::move(redoAct));

            outMessage = "Undo";
            return true;
        }
        else if (last.type == UndoOpType::Trash) {
            for (const auto& item : last.items) {
                std::error_code ec;
                if (std::filesystem::exists(item.dest, ec)) {
                    QString err;
                    if (!XdgUtils::moveItem(item.dest, item.src, err)) {
                        continue;
                    }
                    const char* home = std::getenv("HOME");
                    if (home) {
                        std::filesystem::path infoPath = std::filesystem::path(home) / ".local/share/Trash/info" / (item.dest.filename().string() + ".trashinfo");
                        std::filesystem::remove(infoPath, ec);
                    }
                    redoItems.push_back({item.src, item.dest, {}});
                    successCount++;
                }
            }
            if (successCount == 0 && !last.items.empty()) {
                outMessage = "Undo failed";
                // сохраняем действие для повторной попытки
                m_history.push_back(std::move(last));
                return false;
            }
            UndoAction redoAct;
            redoAct.type = UndoOpType::Trash;
            redoAct.items = redoItems;
            redoAct.description = last.description;
            m_redoStack.push_back(std::move(redoAct));

            outMessage = "Undo";
            return true;
        }
        return false;
    }

    bool redo(QString& outMessage, std::filesystem::path* outSelectPath = nullptr) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_redoStack.empty()) {
            outMessage = "Nothing to redo";
            return false;
        }

        auto last = std::move(m_redoStack.back());
        m_redoStack.pop_back();

        if (outSelectPath && !last.items.empty()) {
            *outSelectPath = last.items.front().dest;
        }

        int successCount = 0;
        std::vector<UndoItem> undoItems;

        if (last.type == UndoOpType::Move) {
            for (auto item : last.items) {
                // сохраняем файл перед перезаписью
                item.backupPath = stashForUndo(item.dest);
                QString err;
                if (!XdgUtils::moveItem(item.src, item.dest, err)) {
                    continue;
                }
                undoItems.push_back(item);
                successCount++;
            }
            if (successCount == 0 && !last.items.empty()) {
                outMessage = "Redo failed";
                // сохраняем действие для повторной попытки
                m_redoStack.push_back(std::move(last));
                return false;
            }
            UndoAction act;
            act.type = UndoOpType::Move;
            act.items = undoItems;
            act.description = last.description;
            m_history.push_back(std::move(act));

            outMessage = "Redo";
            return true;
        }
        else if (last.type == UndoOpType::Copy) {
            for (auto item : last.items) {
                item.backupPath = stashForUndo(item.dest);
                QString err;
                if (XdgUtils::copyItem(item.src, item.dest, err)) {
                    undoItems.push_back(item);
                    successCount++;
                }
            }
            if (successCount == 0 && !last.items.empty()) {
                outMessage = "Redo failed";
                // сохраняем действие для повторной попытки
                m_redoStack.push_back(std::move(last));
                return false;
            }
            UndoAction act;
            act.type = UndoOpType::Copy;
            act.items = undoItems;
            act.description = last.description;
            m_history.push_back(std::move(act));

            outMessage = "Redo";
            return true;
        }
        else if (last.type == UndoOpType::Create) {
            for (const auto& item : last.items) {
                if (restoreFromUndo(item.backupPath, item.dest)) {
                    undoItems.push_back({item.src, item.dest, {}});
                    successCount++;
                }
            }
            if (successCount == 0 && !last.items.empty()) {
                outMessage = "Redo failed";
                // сохраняем действие для повторной попытки
                m_redoStack.push_back(std::move(last));
                return false;
            }
            UndoAction act;
            act.type = UndoOpType::Create;
            act.items = undoItems;
            act.description = last.description;
            m_history.push_back(std::move(act));

            outMessage = "Redo";
            return true;
        }
        else if (last.type == UndoOpType::Trash) {
            for (const auto& item : last.items) {
                QString err;
                if (!XdgUtils::moveItem(item.src, item.dest, err)) {
                    continue;
                }
                undoItems.push_back(item);
                successCount++;
            }
            if (successCount == 0 && !last.items.empty()) {
                outMessage = "Redo failed";
                // сохраняем действие для повторной попытки
                m_redoStack.push_back(std::move(last));
                return false;
            }
            UndoAction act;
            act.type = UndoOpType::Trash;
            act.items = undoItems;
            act.description = last.description;
            m_history.push_back(std::move(act));

            outMessage = "Redo";
            return true;
        }
        return false;
    }

    ~UndoManager() {
        cleanupSessionBackups();
    }

    static void cleanupSessionBackups() {
        std::error_code ec;
        const auto dir = sessionBackupRootDir();
        if (std::filesystem::exists(dir, ec)) {
            std::filesystem::remove_all(dir, ec);
        }
        const auto base = undoBackupsBaseDir();
        if (std::filesystem::exists(base, ec) && std::filesystem::is_empty(base, ec)) {
            std::filesystem::remove(base, ec);
        }
    }

    static void cleanupStaleBackups() {
        std::error_code ec;
        const auto base = undoBackupsBaseDir();
        if (!std::filesystem::exists(base, ec)) return;

        pid_t myPid = ::getpid();
        for (const auto& entry : std::filesystem::directory_iterator(base, ec)) {
            if (ec) break;
            const auto filename = entry.path().filename().string();
            if (filename.rfind("session_", 0) == 0) {
                std::string pidStr = filename.substr(8);
                try {
                    pid_t pid = std::stoi(pidStr);
                    if (pid == myPid) continue;
                    if (::kill(pid, 0) == -1 && errno == ESRCH) {
                        std::error_code rmEc;
                        std::filesystem::remove_all(entry.path(), rmEc);
                    }
                } catch (...) {
                    std::error_code rmEc;
                    std::filesystem::remove_all(entry.path(), rmEc);
                }
            } else {
                std::error_code rmEc;
                std::filesystem::remove_all(entry.path(), rmEc);
            }
        }
        if (std::filesystem::exists(base, ec) && std::filesystem::is_empty(base, ec)) {
            std::filesystem::remove(base, ec);
        }
    }

private:
    UndoManager() {
        cleanupStaleBackups();
    }
    mutable std::mutex m_mutex;
    std::vector<UndoAction> m_history;
    std::vector<UndoAction> m_redoStack;
};

}
