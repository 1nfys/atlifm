#pragma once

#include <QDrag>
#include <QMimeData>
#include <QPixmap>
#include <QDropEvent>
#include <vector>
#include <filesystem>
#include <functional>

namespace Dnd {

// только статические функции, класс не инстанцируется
class DragDropHandler {
public:
    // создаёт исходящий объект QDrag с правильными MIME типами и собственным визуальным бейджем
    static QDrag* createDrag(const std::vector<std::filesystem::path>& paths, QWidget* dragSource);

    // создаёт pixmap превью перетаскивания: тёмный бейдж терминала "📄 [X items selected]"
    static QPixmap createDragPixmap(size_t itemCount, const QString& firstItemName);

    // проверяет, что событие сброса содержит допустимый файл, URI, изображение или веб данные
    static bool canAcceptDrop(const QMimeData* mimeData);

    // извлекает список локальных путей к файлам из QMimeData
    static std::vector<std::filesystem::path> extractPaths(const QMimeData* mimeData);

    // обрабатывает любой сброс или вставку (локальные файлы, изображения из браузера и чатов, URI data:, удалённые URL http/https)
    static bool handleMimeDrop(const QMimeData* mimeData,
                               const std::filesystem::path& targetDir,
                               Qt::DropAction proposedAction,
                               Qt::KeyboardModifiers modifiers,
                               QWidget* parentWidget,
                               std::function<void()> onFinished = nullptr);

    // фоновые операции copy/move/symlink: активна максимум одна одновременно
    static bool hasActiveOperation();
    static void cancelActiveOperation();

    static void handleDrop(const std::vector<std::filesystem::path>& sources,
                           const std::filesystem::path& targetDir,
                           Qt::DropAction proposedAction,
                           Qt::KeyboardModifiers modifiers,
                           QWidget* parentWidget,
                           std::function<void()> onFinished = nullptr);
};

}
