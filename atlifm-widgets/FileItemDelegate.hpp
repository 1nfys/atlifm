#pragma once

#include <QStyledItemDelegate>
#include <QPainter>
#include <filesystem>

namespace Widgets {

class FileItemDelegate : public QStyledItemDelegate {
    Q_OBJECT

public:
    explicit FileItemDelegate(bool isDimmed = false, QObject* parent = nullptr);

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;

    void setDropTargetRow(int row) { m_dropTargetRow = row; }
    int dropTargetRow() const { return m_dropTargetRow; }
    void setContextMenuRow(int row) { m_contextMenuRow = row; }
    void setHighlightedPath(const std::filesystem::path& path) { m_highlightedPath = path; }
    // галочка только при явном множественном выделении, боковые панели её не показывают, центральная при selCount > 1
    void setCheckmarksEnabled(bool enabled) { m_checkmarksEnabled = enabled; }

private:
    bool isHighlighted(const QModelIndex& index) const;

    bool m_isDimmed{false};
    bool m_checkmarksEnabled{true};
    int m_dropTargetRow{-1};
    int m_contextMenuRow{-1};
    std::filesystem::path m_highlightedPath;
};

}
