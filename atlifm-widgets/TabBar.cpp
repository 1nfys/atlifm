#include "TabBar.hpp"
#include "../atlifm-theme/Theme.hpp"
#include "../atlifm-core/Icons.hpp"
#include "../atlifm-dnd/DragDropHandler.hpp"
#include <QPainter>
#include <QMouseEvent>
#include <QMenu>
#include <QApplication>
#include <QClipboard>
#include <algorithm>
#include <cstdlib>

namespace Widgets {

EngineTabBar::EngineTabBar(QWidget* parent)
    : QWidget(parent) {
    setFixedHeight(kNormalHeight);
    setMouseTracking(true);
    setAcceptDrops(true);

    m_dragSwitchTimer = new QTimer(this);
    m_dragSwitchTimer->setSingleShot(true);
    connect(m_dragSwitchTimer, &QTimer::timeout, this, [this]() {
        if (m_dragHoveredTabIndex >= 0 && m_dragHoveredTabIndex < static_cast<int>(m_tabs.size())) {
            setCurrentIndex(m_dragHoveredTabIndex);
        }
    });
}

int EngineTabBar::addTab(const std::filesystem::path& path, const QString& title) {
    Core::TabState tab;
    tab.id = m_nextTabId++;
    tab.currentPath = path;
    if (title.isEmpty()) {
        std::string fname = path.filename().string();
        tab.title = fname.empty() ? QString::fromUtf8(path.c_str()) : QString::fromUtf8(fname.c_str());
    } else {
        tab.title = title;
    }

    m_tabs.push_back(std::move(tab));
    int newIdx = static_cast<int>(m_tabs.size()) - 1;
    setCurrentIndex(newIdx);
    ensureTabVisible(newIdx);
    update();
    return newIdx;
}

void EngineTabBar::removeTab(int index) {
    if (index < 0 || index >= static_cast<int>(m_tabs.size())) return;
    if (m_tabs[index].isPinned) return;

    m_tabs.erase(m_tabs.begin() + index);

    if (m_tabs.empty()) {
        m_currentIndex = -1;
        const char* home = std::getenv("HOME");
        addTab(home ? home : "/");
        return;
    }

    // индекс текущей вкладки правим до emit: удаление вкладки левее текущей
    // сдвигает её позицию, без декремента currentChanged указывал бы на соседнюю
    if (index < m_currentIndex) {
        --m_currentIndex;
    } else if (m_currentIndex >= static_cast<int>(m_tabs.size())) {
        m_currentIndex = static_cast<int>(m_tabs.size()) - 1;
    }
    emit currentChanged(m_currentIndex);
    ensureTabVisible(m_currentIndex);
    update();
}

void EngineTabBar::setCurrentIndex(int index) {
    if (index < 0 || index >= static_cast<int>(m_tabs.size())) return;
    if (m_currentIndex != index) {
        m_currentIndex = index;
        emit currentChanged(m_currentIndex);
        ensureTabVisible(m_currentIndex);
        update();
    }
}

void EngineTabBar::updateTabTitle(int index, const QString& title) {
    if (index >= 0 && index < static_cast<int>(m_tabs.size())) {
        m_tabs[index].title = title;
        update();
    }
}

QSize EngineTabBar::sizeHint() const {
    return QSize(400, kNormalHeight);
}

int EngineTabBar::tabIndexAt(const QPoint& pos) const {
    auto tabRects = calculateTabRects();
    for (size_t i = 0; i < tabRects.size(); ++i) {
        if (tabRects[i].fullRect.contains(pos)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::vector<EngineTabBar::TabRect> EngineTabBar::calculateTabRects() const {
    std::vector<TabRect> rects;
    if (m_tabs.empty()) return rects;

    int totalAvailW = std::max(60, width() - 36);
    int nTabs = static_cast<int>(m_tabs.size());

    int minTabW = (width() < 420) ? 45 : 75;
    int tabW = std::clamp(totalAvailW / std::max(1, nTabs), minTabW, 175);
    int x = -m_scrollOffset;
    int h = height();

    for (size_t i = 0; i < m_tabs.size(); ++i) {
        TabRect tr;
        tr.fullRect = QRect(x, 0, tabW, h);
        tr.closeButtonRect = QRect(x + tabW - 18, (h - 12) / 2, 12, 12);
        rects.push_back(tr);
        x += tabW;
    }

    return rects;
}

QRect EngineTabBar::plusButtonRect() const {
    auto rects = calculateTabRects();
    if (rects.empty()) {
        return QRect(0, 0, 30, height());
    }
    int lastRight = rects.back().fullRect.right() + 1;
    if (lastRight + 30 <= width()) {
        return QRect(lastRight, 0, 30, height());
    }
    return QRect(width() - 30, 0, 30, height());
}

void EngineTabBar::ensureTabVisible(int index) {
    if (index < 0 || index >= static_cast<int>(m_tabs.size())) return;
    int totalAvailW = std::max(60, width() - 36);
    int nTabs = static_cast<int>(m_tabs.size());
    int minTabW = (width() < 420) ? 45 : 75;
    int tabW = std::clamp(totalAvailW / std::max(1, nTabs), minTabW, 175);

    int tabLeft = index * tabW;
    int tabRight = tabLeft + tabW;

    if (tabLeft < m_scrollOffset) {
        m_scrollOffset = tabLeft;
    } else if (tabRight > m_scrollOffset + totalAvailW) {
        m_scrollOffset = tabRight - totalAvailW;
    }

    int maxScroll = std::max(0, nTabs * tabW - totalAvailW);
    m_scrollOffset = std::clamp(m_scrollOffset, 0, maxScroll);
}

void EngineTabBar::wheelEvent(QWheelEvent* event) {
    int delta = event->angleDelta().y();
    if (delta == 0) delta = event->angleDelta().x();

    int totalAvailW = std::max(60, width() - 36);
    int nTabs = static_cast<int>(m_tabs.size());
    int minTabW = (width() < 420) ? 45 : 75;
    int tabW = std::clamp(totalAvailW / std::max(1, nTabs), minTabW, 175);
    int maxScroll = std::max(0, nTabs * tabW - totalAvailW);

    if (maxScroll > 0) {
        m_scrollOffset -= (delta / 2);
        m_scrollOffset = std::clamp(m_scrollOffset, 0, maxScroll);
        update();
        event->accept();
        return;
    }
    QWidget::wheelEvent(event);
}

void EngineTabBar::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    using namespace Theme::Colors;

    // сплошной фон шапки как у колонок с файлами, панель вкладок не отдельная полоса другого цвета
    painter.fillRect(rect(), TabBg());

    // шрифт вкладок как у строк файлов: действующий шрифт виджета из application QSS,
    // фолбэк без QSS тот же моноширинный шрифт в 13px
    QFont tabFont = font();
    if (tabFont.pixelSize() <= 0 && tabFont.pointSize() <= 0) {
        tabFont = Theme::getMonospaceFont(9);
        tabFont.setPixelSize(13);
    }
    painter.setFont(tabFont);
    QFontMetrics fm(tabFont);

    auto tabRects = calculateTabRects();
    QRect plusRect = plusButtonRect();

    int clipW = std::min(width(), plusRect.left());
    if (clipW > 0) {
        painter.setClipRect(0, 0, clipW, height());
    }

    for (size_t i = 0; i < m_tabs.size(); ++i) {
        const auto& tab = m_tabs[i];
        const auto& tr = tabRects[i];
        bool isActive = (static_cast<int>(i) == m_currentIndex);
        bool isHovered = (static_cast<int>(i) == m_hoveredTabIndex);
        bool isDragHovered = (static_cast<int>(i) == m_dragHoveredTabIndex);

        // активная вкладка отличается только цветом текста, без подчёркивания и рамок
        if (isActive) {
            painter.fillRect(tr.fullRect, TabActiveBg());
        } else if (isDragHovered) {
            painter.fillRect(tr.fullRect, DropBg());
            painter.fillRect(QRect(tr.fullRect.left(), tr.fullRect.top(), tr.fullRect.width(), 2), DropBorder());
        } else if (isHovered) {
            painter.fillRect(tr.fullRect, TabHoverBg());
        }

        QString tabPrefix = QString("%1: ").arg(i + 1);

        // выбираем иконку по реальному каталогу вкладки
        auto iconInfo = Icons::getIcon(tab.currentPath, true, false, isActive);
        QString icon = tab.isPinned ? "\U000F0403 " : (iconInfo.glyph + " ");
        QColor textColor = isActive ? TabActiveText() : TabFg();

        int textX = tr.fullRect.left() + 8;
        int yMid = (tr.fullRect.height() + fm.ascent() - fm.descent()) / 2;

        painter.setPen(isActive ? TabActiveFg() : TabFg());
        painter.drawText(textX, yMid, tabPrefix + icon);
        textX += fm.horizontalAdvance(tabPrefix + icon);

        int availWidth = tr.closeButtonRect.left() - textX - 4;
        if (availWidth > 10) {
            painter.setPen(textColor);
            QString elidedTitle = fm.elidedText(tab.title, Qt::ElideRight, availWidth);
            painter.drawText(textX, yMid, elidedTitle);
        }

        if (!tab.isPinned) {
            bool closeHovered = isHovered && m_hoveredCloseBtn;
            painter.setPen(closeHovered ? TabCloseHoverFg() : (isActive ? TabCloseFg() : TabFg()));
            painter.drawText(tr.closeButtonRect, Qt::AlignCenter, "✕");
        }
    }

    painter.setClipping(false);

    if (m_hoveredPlusBtn) {
        painter.fillRect(plusRect, TabHoverBg());
    } else {
        painter.fillRect(plusRect, TabBg());
    }
    painter.setPen(m_hoveredPlusBtn ? TabActiveFg() : TabFg());
    painter.drawText(plusRect, Qt::AlignCenter, "+");
    // без линий разделителей, панель вкладок сливается с фоном колонок
}

void EngineTabBar::mousePressEvent(QMouseEvent* event) {
    auto tabRects = calculateTabRects();
    QRect plusRect = plusButtonRect();

    if (plusRect.contains(event->pos())) {
        if (event->button() == Qt::LeftButton) {
            const char* home = std::getenv("HOME");
            emit newTabRequested(home ? home : "/");
            return;
        }
    }

    for (size_t i = 0; i < tabRects.size(); ++i) {
        if (tabRects[i].fullRect.contains(event->pos())) {
            int idx = static_cast<int>(i);
            if (event->button() == Qt::LeftButton) {
                if (tabRects[i].closeButtonRect.contains(event->pos())) {
                    emit tabCloseRequested(idx);
                } else {
                    setCurrentIndex(idx);
                }
            } else if (event->button() == Qt::MiddleButton) {
                emit tabCloseRequested(idx);
            } else if (event->button() == Qt::RightButton) {
                showContextMenu(idx, event->globalPosition().toPoint());
            }
            return;
        }
    }
}

void EngineTabBar::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        if (plusButtonRect().contains(event->pos()) || tabIndexAt(event->pos()) < 0) {
            const char* home = std::getenv("HOME");
            emit newTabRequested(home ? home : "/");
        }
    }
}

void EngineTabBar::mouseMoveEvent(QMouseEvent* event) {
    auto tabRects = calculateTabRects();
    QRect plusRect = plusButtonRect();

    int prevHoveredTab = m_hoveredTabIndex;
    bool prevCloseHovered = m_hoveredCloseBtn;
    bool prevPlusHovered = m_hoveredPlusBtn;

    m_hoveredTabIndex = -1;
    m_hoveredCloseBtn = false;
    m_hoveredPlusBtn = plusRect.contains(event->pos());

    if (!m_hoveredPlusBtn) {
        for (size_t i = 0; i < tabRects.size(); ++i) {
            if (tabRects[i].fullRect.contains(event->pos())) {
                m_hoveredTabIndex = static_cast<int>(i);
                m_hoveredCloseBtn = tabRects[i].closeButtonRect.contains(event->pos());
                break;
            }
        }
    }

    if (prevHoveredTab != m_hoveredTabIndex || prevCloseHovered != m_hoveredCloseBtn || prevPlusHovered != m_hoveredPlusBtn) {
        update();
    }
}

void EngineTabBar::leaveEvent(QEvent* /*event*/) {
    m_hoveredTabIndex = -1;
    m_hoveredCloseBtn = false;
    m_hoveredPlusBtn = false;
    m_dragHoveredTabIndex = -1;
    update();
}

void EngineTabBar::dragEnterEvent(QDragEnterEvent* event) {
    if (Dnd::DragDropHandler::canAcceptDrop(event->mimeData())) {
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

void EngineTabBar::dragMoveEvent(QDragMoveEvent* event) {
    if (!Dnd::DragDropHandler::canAcceptDrop(event->mimeData())) {
        event->ignore();
        return;
    }

    int idx = tabIndexAt(event->position().toPoint());
    if (idx != m_dragHoveredTabIndex) {
        m_dragHoveredTabIndex = idx;
        update();
        if (idx >= 0 && idx != m_currentIndex) {
            m_dragSwitchTimer->start(280); // переключение вкладки после 280ms наведения
        } else {
            m_dragSwitchTimer->stop();
        }
    }
    event->acceptProposedAction();
}

void EngineTabBar::dragLeaveEvent(QDragLeaveEvent* /*event*/) {
    m_dragHoveredTabIndex = -1;
    m_dragSwitchTimer->stop();
    update();
}

void EngineTabBar::dropEvent(QDropEvent* event) {
    m_dragSwitchTimer->stop();
    int targetIdx = m_dragHoveredTabIndex;
    m_dragHoveredTabIndex = -1;
    update();

    if (!Dnd::DragDropHandler::canAcceptDrop(event->mimeData())) {
        event->ignore();
        return;
    }

    if (targetIdx < 0 || targetIdx >= static_cast<int>(m_tabs.size())) {
        targetIdx = m_currentIndex;
    }

    if (targetIdx >= 0 && targetIdx < static_cast<int>(m_tabs.size())) {
        auto paths = Dnd::DragDropHandler::extractPaths(event->mimeData());
        if (!paths.empty()) {
            emit filesDroppedOnTab(targetIdx, paths, event->proposedAction(), event->modifiers());
            event->acceptProposedAction();
            return;
        }
        if (Dnd::DragDropHandler::handleMimeDrop(event->mimeData(), m_tabs[targetIdx].currentPath,
                                                     event->proposedAction(), event->modifiers(),
                                                     window())) {
            event->acceptProposedAction();
            return;
        }
    }
    event->ignore();
}

void EngineTabBar::showContextMenu(int tabIndex, const QPoint& globalPos) {
    if (tabIndex < 0 || tabIndex >= static_cast<int>(m_tabs.size())) return;

    QMenu menu(this);
    menu.setStyleSheet(Theme::getApplicationStyleSheet());
    menu.setFont(Theme::getMonospaceFont(9));

    auto actClose = menu.addAction("Close Tab\tCtrl+W");
    connect(actClose, &QAction::triggered, this, [this, tabIndex]() {
        emit tabCloseRequested(tabIndex);
    });

    auto actCloseOthers = menu.addAction("Close Other Tabs\tCtrl+Alt+W");
    connect(actCloseOthers, &QAction::triggered, this, [this, tabIndex]() {
        emit closeOtherTabsRequested(tabIndex);
    });

    auto actDuplicate = menu.addAction("Duplicate Tab\tCtrl+Shift+T");
    connect(actDuplicate, &QAction::triggered, this, [this, tabIndex]() {
        emit duplicateTabRequested(tabIndex);
    });

    menu.addSeparator();

    auto actPin = menu.addAction(m_tabs[tabIndex].isPinned ? "Unpin Tab\tCtrl+P" : "Pin Tab\tCtrl+P");
    connect(actPin, &QAction::triggered, this, [this, tabIndex]() {
        emit pinTabRequested(tabIndex);
    });

    menu.exec(globalPos);
}

void EngineTabBar::closeOtherTabs(int keepIndex) {
    if (keepIndex < 0 || keepIndex >= static_cast<int>(m_tabs.size())) return;
    std::vector<Core::TabState> kept;
    int newCurrent = 0;
    for (int i = 0; i < static_cast<int>(m_tabs.size()); ++i) {
        if (i == keepIndex || m_tabs[i].isPinned) {
            if (i == keepIndex) newCurrent = static_cast<int>(kept.size());
            kept.push_back(std::move(m_tabs[i]));
        }
    }
    m_tabs = std::move(kept);
    m_currentIndex = newCurrent;
    emit currentChanged(m_currentIndex);
    update();
}

}
