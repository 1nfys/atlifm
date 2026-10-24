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
    static constexpr const char* kMovePreferredMime = "application/x-atlifm-prefer-move";

    // создаёт исходящий объект qdrag с правильными mime типами и собственным визуальным бейджем
    static QDrag* createDrag(const std::vector<std::filesystem::path>& paths, QWidget* dragSource);

    static bool prefersMove(const QDropEvent* event);
    static Qt::DropAction effectiveAction(const QDropEvent* event);
    static void acceptDrag(QDropEvent* event);

    // создаёт pixmap превью перетаскивания: тёмный бейдж терминала "📄 [x items selected]"
    static QPixmap createDragPixmap(size_t itemCount, const QString& firstItemName);

    // проверяет, что событие сброса содержит допустимый файл, uri, изображение или веб данные
    static bool canAcceptDrop(const QMimeData* mimeData);

    // извлекает список локальных путей к файлам из qmimedata
    static std::vector<std::filesystem::path> extractPaths(const QMimeData* mimeData);

    // обрабатывает любой сброс или вставку (локальные файлы, изображения из браузера и чатов, uri data:, удалённые url http/https)
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
