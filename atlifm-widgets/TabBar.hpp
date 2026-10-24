#pragma once

#include <QWidget>
#include <vector>
#include <filesystem>
#include <memory>
#include <QTimer>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include "../atlifm-core/TabState.hpp"

namespace Widgets {

class EngineTabBar : public QWidget {
    Q_OBJECT

public:
    // обычная высота шапки вкладок, компакт при низком окне (см. mainwindow::resizeevent)
    static constexpr int kNormalHeight = 30;

    explicit EngineTabBar(QWidget* parent = nullptr);

    int addTab(const std::filesystem::path& path, const QString& title = "");
    void removeTab(int index);
    void setCurrentIndex(int index);
    int currentIndex() const { return m_currentIndex; }
    int count() const { return static_cast<int>(m_tabs.size()); }

    Core::TabState& tabState(int index) { return m_tabs[index]; }
    const Core::TabState& tabState(int index) const { return m_tabs[index]; }
    Core::TabState& currentTabState() { return m_tabs[m_currentIndex]; }

    void updateTabTitle(int index, const QString& title);

    // close other tabs из контекстного меню, закреплённые вкладки сохраняются
    void closeOtherTabs(int keepIndex);

signals:
    void currentChanged(int index);
    void tabCloseRequested(int index);
    void newTabRequested(const std::filesystem::path& path);
    void duplicateTabRequested(int index);
    void pinTabRequested(int index);
    void closeOtherTabsRequested(int index);
    void filesDroppedOnTab(int tabIndex, const std::vector<std::filesystem::path>& sources, Qt::DropAction action, Qt::KeyboardModifiers modifiers);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    QSize sizeHint() const override;

private:
    struct TabRect {
        QRect fullRect;
        QRect closeButtonRect;
    };

    std::vector<TabRect> calculateTabRects() const;
    QRect plusButtonRect() const;
    int tabIndexAt(const QPoint& pos) const;
    void showContextMenu(int tabIndex, const QPoint& globalPos);
    void ensureTabVisible(int index);

    std::vector<Core::TabState> m_tabs;
    int m_currentIndex{-1};
    int m_hoveredTabIndex{-1};
    int m_dragHoveredTabIndex{-1};
    bool m_hoveredCloseBtn{false};
    bool m_hoveredPlusBtn{false};
    int m_scrollOffset{0};
    int m_nextTabId{1};
    QTimer* m_dragSwitchTimer{nullptr};
};

}
