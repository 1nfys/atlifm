#pragma once

#include <QWidget>
#include <QListWidget>
#include <QLabel>
#include <QPushButton>
#include <filesystem>
#include <vector>
#include "../atlifm-core/Bookmarks.hpp"

namespace Widgets {

class BookmarkItemDelegate;

class BookmarksListWidget : public QListWidget {
    Q_OBJECT
public:
    using QListWidget::QListWidget;

signals:
    void itemRightClicked(const QPoint& globalPos, QListWidgetItem* item);
    void itemMiddleClicked(QListWidgetItem* item);

protected:
    void mousePressEvent(QMouseEvent* event) override;
};

class BookmarksBar : public QWidget {
    Q_OBJECT

public:
    explicit BookmarksBar(QWidget* parent = nullptr);

    void addBookmark(const std::filesystem::path& path, const QString& title = "");
    void refresh();
    void setActiveDirectory(const std::filesystem::path& path);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void navigateRequested(const std::filesystem::path& path);
    void openInNewTabRequested(const std::filesystem::path& path);
    void filesDropped(const std::vector<std::filesystem::path>& sources,
                      const std::filesystem::path& targetDir,
                      Qt::DropAction action,
                      Qt::KeyboardModifiers modifiers);
    // действия меню ПКМ вынесены в сигналы: окно сначала даёт плагину перехватить, потом выполняет встроенное поведение
    void copyPathRequested(const std::filesystem::path& path);
    void removeBookmarkRequested(const std::filesystem::path& path);
    void trashClearRequested();
    void propertiesRequested(const std::filesystem::path& path);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private slots:
    void onItemClicked(QListWidgetItem* item);
    void onCustomContextMenu(const QPoint& pos);
    void onAddCurrentClicked();

private:
    void setupUi();
    void populateItems();

    BookmarksListWidget* m_listWidget{nullptr};
    BookmarkItemDelegate* m_delegate{nullptr};
    std::vector<Core::BookmarkItem> m_places;
    std::vector<Core::BookmarkItem> m_drives;
    std::vector<Core::BookmarkItem> m_userBookmarks;

    std::filesystem::path m_activeDir;
    int m_dragHoverRow{-1};
};

}
