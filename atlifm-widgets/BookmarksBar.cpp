#include "BookmarksBar.hpp"
#include "../atlifm-theme/Theme.hpp"
#include "../atlifm-core/XdgUtils.hpp"
#include "../atlifm-dnd/DragDropHandler.hpp"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPainter>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QMenu>
#include <QMouseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QApplication>
#include <QClipboard>
#include <QPointer>
#include <QStyledItemDelegate>

namespace Widgets {

void BookmarksListWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::RightButton) {
        QListWidgetItem* item = itemAt(event->pos());
        if (item && !item->data(Qt::UserRole + 2).toBool()) {
            emit itemRightClicked(event->globalPosition().toPoint(), item);
            event->accept();
            return; // меню сразу по клику, активную папку не меняет
        }
    } else if (event->button() == Qt::MiddleButton) {
        QListWidgetItem* item = itemAt(event->pos());
        if (item && !item->data(Qt::UserRole + 2).toBool()) {
            emit itemMiddleClicked(item);
            event->accept();
            return;
        }
    }
    QListWidget::mousePressEvent(event);
}

class BookmarkItemDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit BookmarkItemDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent) {}

    void setContextMenuRow(int row) { m_contextMenuRow = row; }
    int contextMenuRow() const { return m_contextMenuRow; }

    void setActiveRow(int row) { m_activeRow = row; }
    int activeRow() const { return m_activeRow; }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        bool isSep = index.data(Qt::UserRole + 2).toBool();
        if (isSep) {
            painter->save();
            int y = option.rect.center().y();
            painter->setPen(QPen(Theme::Colors::Border(), 1));
            painter->drawLine(option.rect.left() + 10, y, option.rect.right() - 10, y);
            painter->restore();
            return;
        }

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, false);

        QRect r = option.rect.adjusted(3, 1, -3, -1);
        bool isHovered = (option.state & QStyle::State_MouseOver);
        bool isContextMenu = (index.row() == m_contextMenuRow);
        bool isActiveFolder = (index.row() == m_activeRow);

        QColor fgColor = Theme::Colors::Text();

        if (isContextMenu) {
            painter->fillRect(r, Theme::Colors::DropBg());
            painter->setPen(QPen(Theme::Colors::DropBorder(), 1));
            painter->drawRect(r.adjusted(0, 0, -1, -1));
            fgColor = Theme::Colors::DropBorder();
        } else if (isActiveFolder) {
            painter->fillRect(r, Theme::Colors::SelectionBg());
            fgColor = Theme::Colors::SelectionFg();
        } else if (isHovered) {
            painter->fillRect(r, Theme::Colors::HoverBg());
        }

        painter->setFont(option.font);
        painter->setPen(fgColor);
        const QString glyph = index.data(Qt::UserRole + 3).toString();
        if (!glyph.isEmpty() && r.width() < 78) {
            // узкая панель: рисуем только глиф по центру строки
            // центрируются чернила глифа по tightBoundingRect, т.к. глифы Nerd шире своей ячейки и прижаты к верху
            painter->setFont(Theme::getMonospaceFont(12));
            const QFontMetricsF fm(painter->font());
            const QRectF ink = fm.tightBoundingRect(glyph);
            // центр через QRectF: QRect::center() на чётной ширине округляет вниз, иконки уезжали на 1px
            const QPointF cellCenter = QRectF(r).center();
            painter->drawText(QPointF(cellCenter.x() - ink.center().x(),
                                      cellCenter.y() - ink.center().y()), glyph);
            painter->restore();
            return;
        }
        QRect textRect = option.rect.adjusted(10, 0, -5, 0);
        QString text = index.data(Qt::DisplayRole).toString();
        painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, text);

        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem& /*option*/, const QModelIndex& index) const override {
        if (index.data(Qt::UserRole + 2).toBool()) {
            return QSize(0, 12);
        }
        // компактный ритм строк
        return QSize(0, QFontMetrics(Theme::getMonospaceFont(11)).height() + 4);
    }

private:
    int m_contextMenuRow{-1};
    int m_activeRow{-1};
};

BookmarksBar::BookmarksBar(QWidget* parent)
    : QWidget(parent) {
    setAcceptDrops(true);
    setupUi();
    refresh();
}

QSize BookmarksBar::sizeHint() const {
    return QSize(160, 400);
}

QSize BookmarksBar::minimumSizeHint() const {
    return QSize(36, 40);
}

void BookmarksBar::setupUi() {
    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    using namespace Theme::Colors;

    m_listWidget = new BookmarksListWidget(this);
    m_listWidget->setFont(Theme::getMonospaceFont(11));
    m_listWidget->setFrameShape(QFrame::NoFrame);
    m_delegate = new BookmarkItemDelegate(this);
    m_listWidget->setItemDelegate(m_delegate);
    m_listWidget->setFocusPolicy(Qt::NoFocus);
    // скроллбары убраны, прокрутка колесом
    m_listWidget->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_listWidget->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_listWidget->setContextMenuPolicy(Qt::NoContextMenu);
    m_listWidget->setAcceptDrops(false);

    m_listWidget->setStyleSheet(QString(R"(
        QListWidget {
            background-color: %1;
            border: none;
            padding: 0px;
            margin: 0px;
        }
        QListWidget QScrollBar:vertical {
            background: %1;
            width: 5px;
            margin: 0px;
            border: none;
        }
        QListWidget QScrollBar::handle:vertical {
            background: %2;
            min-height: 20px;
            border-radius: 0px;
        }
        QListWidget QScrollBar::handle:vertical:hover {
            background: %3;
        }
        QListWidget QScrollBar::add-line:vertical,
        QListWidget QScrollBar::sub-line:vertical,
        QListWidget QScrollBar::add-page:vertical,
        QListWidget QScrollBar::sub-page:vertical {
            height: 0px;
            background: transparent;
        }
    )")
        .arg(Base().name())
        .arg(Surface1().name())
        .arg(Surface2().name()));

    connect(m_listWidget, &QListWidget::itemClicked, this, &BookmarksBar::onItemClicked);
    connect(m_listWidget, &BookmarksListWidget::itemMiddleClicked, this, [this](QListWidgetItem* item) {
        if (!item || item->data(Qt::UserRole + 2).toBool()) return;
        QString pathStr = item->data(Qt::UserRole).toString();
        if (!pathStr.isEmpty()) {
            emit openInNewTabRequested(std::filesystem::path(pathStr.toUtf8().toStdString()));
        }
    });
    connect(m_listWidget, &BookmarksListWidget::itemRightClicked, this, [this](const QPoint& globalPos, QListWidgetItem* item) {
        if (!item || item->data(Qt::UserRole + 2).toBool()) return;
        QString pathStr = item->data(Qt::UserRole).toString();
        if (pathStr.isEmpty()) return;

        std::filesystem::path p(pathStr.toUtf8().toStdString());
        int row = m_listWidget->row(item);
        m_delegate->setContextMenuRow(row);
        m_listWidget->viewport()->update();

        bool isUserBm = item->data(Qt::UserRole + 1).toBool();

        QMenu menu(this);
        menu.setFont(Theme::getMonospaceFont(9));
        menu.setStyleSheet(Theme::getApplicationStyleSheet());

        connect(&menu, &QMenu::aboutToHide, this, [this]() {
            m_delegate->setContextMenuRow(-1);
            m_listWidget->viewport()->update();
        });

        auto actOpen = menu.addAction("\U000F0214 Open\tEnter");
        connect(actOpen, &QAction::triggered, this, [this, p]() {
            emit navigateRequested(p);
        });

        auto actNewTab = menu.addAction("\U000F04E6 Open in New Tab\tCtrl+T");
        connect(actNewTab, &QAction::triggered, this, [this, p]() {
            emit openInNewTabRequested(p);
        });

        menu.addSeparator();

        auto actCopyPath = menu.addAction("\U000F0337 Copy Path\tCtrl+Shift+C");
        connect(actCopyPath, &QAction::triggered, this, [this, p]() {
            emit copyPathRequested(p);
        });

        if (pathStr.contains(".local/share/Trash")) {
            auto actClear = menu.addAction("\U000F0A7A Clear Bin");
            connect(actClear, &QAction::triggered, this, [this]() {
                emit trashClearRequested();
            });
        }

    auto actProps = menu.addAction("\U000F02FD Properties\tAlt+Enter");
    connect(actProps, &QAction::triggered, this, [this, p]() {
        // свойства с блоком Disk Usage открывает MillerViewContainer как внутреннее окно в центре
        emit propertiesRequested(p);
    });

        if (isUserBm) {
            menu.addSeparator();
            auto actRemove = menu.addAction("\U000F0A79 Remove Bookmark\tDel");
            connect(actRemove, &QAction::triggered, this, [this, p]() {
                emit removeBookmarkRequested(p);
            });
        }

        menu.exec(globalPos);

        m_delegate->setContextMenuRow(-1);
        m_listWidget->viewport()->update();
    });

    mainLayout->addWidget(m_listWidget, 1);
}

void BookmarksBar::refresh() {
    using namespace Theme::Colors;
    if (m_listWidget) {
        m_listWidget->setStyleSheet(QString(R"(
            QListWidget {
                background-color: %1;
                border: none;
                padding: 0px;
                margin: 0px;
            }
            QListWidget QScrollBar:vertical {
                background: %1;
                width: 5px;
                margin: 0px;
                border: none;
            }
            QListWidget QScrollBar::handle:vertical {
                background: %2;
                min-height: 20px;
                border-radius: 0px;
            }
            QListWidget QScrollBar::handle:vertical:hover {
                background: %3;
            }
            QListWidget QScrollBar::add-line:vertical,
            QListWidget QScrollBar::sub-line:vertical,
            QListWidget QScrollBar::add-page:vertical,
            QListWidget QScrollBar::sub-page:vertical {
                height: 0px;
                background: transparent;
            }
        )")
            .arg(Mantle().name())
            .arg(Surface1().name())
            .arg(Surface2().name()));
    }
    m_places = Core::BookmarksManager::loadPlaces();
    m_drives = Core::BookmarksManager::loadDrives();
    m_userBookmarks = Core::BookmarksManager::loadUserBookmarks(&m_places, &m_drives);
    populateItems();
    if (!m_activeDir.empty()) {
        setActiveDirectory(m_activeDir);
    }
}

void BookmarksBar::setActiveDirectory(const std::filesystem::path& path) {
    m_activeDir = path;
    int bestRow = -1;
    size_t longestMatchLen = 0;

    std::error_code ec;
    std::filesystem::path canonicalActive = std::filesystem::canonical(path, ec);
    if (ec) canonicalActive = path;

    for (int i = 0; i < m_listWidget->count(); ++i) {
        auto item = m_listWidget->item(i);
        if (!item || item->data(Qt::UserRole + 2).toBool()) continue;
        std::filesystem::path itemPath(item->data(Qt::UserRole).toString().toUtf8().toStdString());

        if (std::filesystem::equivalent(canonicalActive, itemPath, ec)) {
            bestRow = i;
            break;
        }

        std::filesystem::path canonicalItem = std::filesystem::canonical(itemPath, ec);
        if (ec) canonicalItem = itemPath;

        if (canonicalActive == canonicalItem) {
            bestRow = i;
            break;
        }

        std::string actStr = canonicalActive.string();
        std::string itmStr = canonicalItem.string();
        if (itmStr != "/" && actStr.rfind(itmStr, 0) == 0) {
            if (actStr.size() == itmStr.size() || actStr[itmStr.size()] == '/') {
                if (itmStr.length() > longestMatchLen) {
                    longestMatchLen = itmStr.length();
                    bestRow = i;
                }
            }
        }
    }

    m_delegate->setActiveRow(bestRow);
    m_listWidget->viewport()->update();
}

void BookmarksBar::populateItems() {
    m_listWidget->clear();

    auto addSeparatorItem = [this]() {
        auto sep = new QListWidgetItem(m_listWidget);
        sep->setFlags(Qt::NoItemFlags);
        sep->setData(Qt::UserRole + 2, true);
        sep->setSizeHint(QSize(0, 12));
    };

    auto addItem = [this](const Core::BookmarkItem& b, bool isUserBookmark) {
        auto item = new QListWidgetItem(m_listWidget);
        item->setText(QString("%1  %2").arg(b.iconGlyph, b.title));
        item->setData(Qt::UserRole, QString::fromUtf8(b.path.c_str()));
        item->setData(Qt::UserRole + 1, isUserBookmark);
        item->setData(Qt::UserRole + 2, false);
        item->setData(Qt::UserRole + 3, b.iconGlyph);
        QString tip = QString::fromUtf8(b.path.c_str());
        if (!b.extraInfo.isEmpty() && b.extraInfo != "~") {
            tip += QString(" (%1)").arg(b.extraInfo);
        }
        item->setToolTip(tip);
        item->setSizeHint(QSize(0, QFontMetrics(Theme::getMonospaceFont(11)).height() + 4));
    };

    // places, системные папки
    for (const auto& b : m_places) {
        addItem(b, false);
    }

    // drives, смонтированные разделы
    if (!m_drives.empty()) {
        if (m_listWidget->count() > 0) {
            addSeparatorItem();
        }
        for (const auto& b : m_drives) {
            addItem(b, false);
        }
    }

    // bookmarks, закладки пользователя
    if (!m_userBookmarks.empty()) {
        if (m_listWidget->count() > 0) {
            addSeparatorItem();
        }
        for (const auto& b : m_userBookmarks) {
            addItem(b, true);
        }
    }
}

void BookmarksBar::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    // фон как у списков файлов (Base), сайдбар это крайняя колонка Miller, а не отдельная панель
    painter.fillRect(rect(), Theme::Colors::Base());
}

void BookmarksBar::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    // шрифт уменьшается вместе с панелью, чтобы подписи влезли
    int w = width();
    int pt = (w < 130) ? 8 : ((w < 160) ? 9 : 11);
    QFont f = Theme::getMonospaceFont(pt);
    if (m_listWidget && m_listWidget->font() != f) {
        m_listWidget->setFont(f);
    }
}

void BookmarksBar::onItemClicked(QListWidgetItem* item) {
    if (!item || item->data(Qt::UserRole + 2).toBool()) return;
    QString pathStr = item->data(Qt::UserRole).toString();
    if (pathStr.isEmpty()) return;

    std::filesystem::path p(pathStr.toUtf8().toStdString());
    std::error_code ec;
    if (std::filesystem::is_directory(p, ec)) {
        emit navigateRequested(p);
    } else if (std::filesystem::exists(p, ec)) {
        Core::XdgUtils::openWithDefault(p);
    }
}

void BookmarksBar::onCustomContextMenu(const QPoint& /*pos*/) {
    // обрабатывается в BookmarksListWidget::mousePressEvent через itemRightClicked
}

void BookmarksBar::onAddCurrentClicked() {
    if (!m_activeDir.empty()) {
        addBookmark(m_activeDir);
    }
}

void BookmarksBar::addBookmark(const std::filesystem::path& path, const QString& title) {
    std::string t = title.isEmpty() ? path.filename().string() : title.toUtf8().toStdString();
    if (t.empty()) t = path.string();

    Core::BookmarksManager::addBookmark(path, QString::fromUtf8(t.c_str()));
    refresh();
}

void BookmarksBar::dragEnterEvent(QDragEnterEvent* event) {
    if (Dnd::DragDropHandler::canAcceptDrop(event->mimeData())) {
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

void BookmarksBar::dragMoveEvent(QDragMoveEvent* event) {
    if (Dnd::DragDropHandler::canAcceptDrop(event->mimeData())) {
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

void BookmarksBar::dragLeaveEvent(QDragLeaveEvent* event) {
    event->accept();
}

void BookmarksBar::dropEvent(QDropEvent* event) {
    if (!Dnd::DragDropHandler::canAcceptDrop(event->mimeData())) {
        event->ignore();
        return;
    }

    QPoint listPos = m_listWidget->mapFrom(this, event->position().toPoint());
    QListWidgetItem* item = m_listWidget->itemAt(listPos);

    if (item && !item->data(Qt::UserRole + 2).toBool()) {
        QString destStr = item->data(Qt::UserRole).toString();
        if (!destStr.isEmpty()) {
            std::filesystem::path dest(destStr.toUtf8().toStdString());
            std::error_code ec;
            if (std::filesystem::is_directory(dest, ec)) {
                auto paths = Dnd::DragDropHandler::extractPaths(event->mimeData());
                if (!paths.empty()) {
                    event->acceptProposedAction();
                    emit filesDropped(paths, dest, event->proposedAction(), event->modifiers());
                    return;
                }
                QPointer<BookmarksBar> guard(this);
                if (Dnd::DragDropHandler::handleMimeDrop(event->mimeData(), dest,
                                                             event->proposedAction(), event->modifiers(),
                                                             this, [guard]() { if (guard) guard->refresh(); })) {
                    event->acceptProposedAction();
                    return;
                }
            }
        }
    }

    event->ignore();
}

}

#include "BookmarksBar.moc"
