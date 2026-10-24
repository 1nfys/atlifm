#pragma once

#include <QTreeWidget>
#include <QHash>
#include <filesystem>
#include <vector>
#include "../atlifm-core/XdgUtils.hpp"
#include "../atlifm-core/tasks/TaskManager.hpp"

class QPainter;
class QRubberBand;
class QFocusEvent;

namespace Models {
class ArchiveModel;
}

namespace Widgets {

// дерево архива с раскрытыми папками и размерами файлов двойной клик распаковывает в кэш а перетаскивание кидает во временный каталог
class ArchiveTreeWidget : public QTreeWidget {
    Q_OBJECT

public:
    explicit ArchiveTreeWidget(QWidget* parent = nullptr);

    void setArchiveModel(Models::ArchiveModel* model);
    void setArchivePath(const std::filesystem::path& archivePath);
    void buildFromEntries(const std::vector<Core::XdgUtils::ArchiveEntry>& entries);
    void clearArchive();
    // сообщение вместо дерева если ошибка листинга или пустой архив
    void showMessage(const QString& message);
    void selectAllEntries();
    void selectFirstEntry();
    void selectLastEntry();
    void clearEntrySelection();
    bool hasSelectedEntries() const;
    void copySelectedEntries();
    bool extractEntryForPreview(const QString& normalizedPath, const QString& memberPath,
                                std::filesystem::path& outPath, QString& error);
    bool isExtracting() const { return m_extracting; }

signals:
    void leaveArchiveRequested();
    void selectionInteractionStarted();
    void messageRequested(const QString& message);
    void entrySelected(const QString& normalizedPath, const QString& memberPath, bool isDir);

protected:
    void startDrag(Qt::DropActions supportedActions) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void drawBranches(QPainter* painter, const QRect& rect,
                      const QModelIndex& index) const override;

private:
    void activateItem(QTreeWidgetItem* item);
    void openEntry(QTreeWidgetItem* item);
    bool extractToCache(const QStringList& members, const QStringList& normalizedPaths,
                        bool abortOnRelease,
                        std::vector<std::filesystem::path>& outRealPaths, QString& outError);
    void showArchiveMenu(const QPoint& pos);
    void beginBoxSelection(const QPoint& origin, bool additive);
    void updateBoxSelection(const QPoint& pos);
    void finalizeBoxSelection();
    void selectRangeTo(QTreeWidgetItem* target, bool additive);
    void resetSelectionState();

    Models::ArchiveModel* m_model{nullptr};
    std::filesystem::path m_archivePath;

    // пока кнопка зажата ждем распаковку а отпускание отменяет старт drag но кэш догревается
    bool m_gestureButtonDown{false};
    bool m_extracting{false};
    QHash<QString, Core::Tasks::TaskId> m_pendingExtractions;

    QRubberBand* m_rubberBand{nullptr};
    bool m_isRubberBanding{false};
    bool m_boxGesturePending{false};
    bool m_boxAdditive{false};
    bool m_preserveAnchor{false};
    bool m_shiftSelecting{false};
    QTreeWidgetItem* m_selectionAnchor{nullptr};
    QPoint m_rubberBandOrigin;
    QPoint m_pressPos;
};

}
