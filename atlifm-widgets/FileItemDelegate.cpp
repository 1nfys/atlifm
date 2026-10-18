#include "FileItemDelegate.hpp"
#include "../atlifm-theme/Theme.hpp"
#include "../atlifm-models/FileSystemModel.hpp"
#include <QPainter>
#include <QApplication>

namespace Widgets {

FileItemDelegate::FileItemDelegate(bool isDimmed, QObject* parent)
    : QStyledItemDelegate(parent), m_isDimmed(isDimmed) {
}

bool FileItemDelegate::isHighlighted(const QModelIndex& index) const {
    if (m_highlightedPath.empty() || !index.isValid()) return false;
    QString pathStr = index.data(Models::FileSystemModel::PathRole).toString();
    return std::filesystem::path(pathStr.toUtf8().toStdString()) == m_highlightedPath;
}

void FileItemDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const {
    if (!index.isValid()) return;

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, false); // чёткое выравнивание моноширинного текста

    using namespace Theme::Colors;

    // заливаем строку фоном Base, чтобы левый отступ и промежутки были чистыми
    painter->fillRect(option.rect, Base());

    // левый отступ для удобного выделения резиной и чистого выравнивания, плюс правый
    int leftGutter = m_isDimmed ? 16 : 24;
    int rightGutter = 12;
    QRect rect = option.rect.adjusted(leftGutter, 0, -rightGutter, 0);
    bool isSelected = option.state & QStyle::State_Selected;
    bool isHovered = option.state & QStyle::State_MouseOver;
    bool isCurrentInParent = m_isDimmed && isHighlighted(index);
    bool isContextMenuTarget = (index.row() == m_contextMenuRow);
    bool isDropTarget = (index.row() == m_dropTargetRow);
    bool isMultiSelected = index.data(Models::FileSystemModel::IsSelectedRole).toBool();
    QString itemPathStr = index.data(Models::FileSystemModel::PathRole).toString();
    bool isInProgress = Models::FileSystemModel::isInProgress(std::filesystem::path(itemPathStr.toUtf8().toStdString()));

    if (isDropTarget) {
        painter->fillRect(rect, DropBg());
        painter->setPen(QPen(DropBorder(), 1));
        painter->drawRect(rect.adjusted(0, 0, -1, -1));
    } else if (isContextMenuTarget) {
        // отдельная подсветка пункта, где вызвано меню, в родительской панели
        painter->fillRect(rect, DropBg());
        painter->setPen(QPen(DropBorder(), 1));
        painter->drawRect(rect.adjusted(0, 0, -1, -1));
    } else if (isInProgress) {
        painter->fillRect(rect, HoverBg());
    } else if (isSelected) {
        painter->fillRect(rect, SelectionBg());
    } else if (isCurrentInParent) {
        painter->fillRect(rect, HoverBg());
    } else if (isHovered) {
        painter->fillRect(rect, HoverBg());
    } else {
        painter->fillRect(rect, Base());
    }

    QFont font = Theme::getMonospaceFont(10);
    painter->setFont(font);
    QFontMetrics fm(font);

    int padding = 6;
    int x = rect.left() + padding;
    int yMid = rect.top() + (rect.height() + fm.ascent() - fm.descent()) / 2;

    // галочек нет по дизайну: множественное выделение показывается акцентной заливкой и счётчиком, без символов ✓
    (void)isMultiSelected;

    QString glyph = index.data(Models::FileSystemModel::IconGlyphRole).toString();
    QColor iconColor = index.data(Models::FileSystemModel::IconColorRole).value<QColor>();

    if (isInProgress) {
        iconColor = Dim();
    } else if (isSelected) {
        iconColor = SelectionFg();
    } else if (isContextMenuTarget) {
        iconColor = DropBorder();
    } else if (m_isDimmed) {
        iconColor = Dim();
    }

    painter->setPen(iconColor);
    painter->drawText(x, yMid, glyph);
    x += fm.horizontalAdvance(glyph) + 8;

    QString name = index.data(Models::FileSystemModel::NameRole).toString();
    bool isSymlink = index.data(Models::FileSystemModel::IsSymlinkRole).toBool();
    QString linkTarget = index.data(Models::FileSystemModel::SymlinkTargetRole).toString();

    QColor textColor;
    if (isInProgress) {
        textColor = Dim();
    } else if (isSelected) {
        textColor = SelectionFg();
    } else if (isContextMenuTarget) {
        textColor = DropBorder();
    } else if (m_isDimmed) {
        textColor = isCurrentInParent ? Text() : Dim();
    } else {
        textColor = Text();
    }

    // метаданные справа: размер, затем дата левее, прав доступа в строках нет (они в TuiStatusBar)
    QString sizeStr = index.data(Models::FileSystemModel::SizeRole).toString();
    QString dateStr = index.data(Models::FileSystemModel::DateRole).toString();

    int rightEdge = rect.right() - padding;

    if (!m_isDimmed && rect.width() > 220) {
        QColor metaColor = isSelected ? SelectionFg() : Dim();

        if (!sizeStr.isEmpty()) {
            painter->setPen(metaColor);
            int sizeWidth = fm.horizontalAdvance(sizeStr);
            painter->drawText(rightEdge - sizeWidth, yMid, sizeStr);
            rightEdge -= (sizeWidth + 10);
        }

        if (rect.width() > 300 && !dateStr.isEmpty()) {
            painter->setPen(metaColor);
            int dateWidth = fm.horizontalAdvance(dateStr);
            painter->drawText(rightEdge - dateWidth, yMid, dateStr);
            rightEdge -= (dateWidth + 10);
        }
    }

    // имя рисуем обрезанным, чтобы не пересекалось с метаданными; цели симлинков курсивом
    int maxNameWidth = rightEdge - x - 8;
    if (maxNameWidth > 20) {
        if (isSymlink && !linkTarget.isEmpty()) {
            QString arrow = " -> ";
            int nameW = fm.horizontalAdvance(name);
            int arrowW = fm.horizontalAdvance(arrow);
            painter->setPen(textColor);
            painter->drawText(x, yMid, name);

            if (nameW + arrowW < maxNameWidth) {
                painter->setPen(isSelected ? AccentFg() : Dim());
                painter->drawText(x + nameW, yMid, arrow);

                QFont italicFont = font;
                italicFont.setItalic(true);
                painter->setFont(italicFont);
                QFontMetrics ifm(italicFont);
                int targetMaxW = maxNameWidth - nameW - arrowW;
                QString elidedTarget = ifm.elidedText(linkTarget, Qt::ElideMiddle, targetMaxW);
                painter->drawText(x + nameW + arrowW, yMid, elidedTarget);
                painter->setFont(font);
            }
        } else {
            painter->setPen(textColor);
            QString elidedName = fm.elidedText(name, Qt::ElideMiddle, maxNameWidth);
            painter->drawText(x, yMid, elidedName);
        }
    }

    painter->restore();
}

QSize FileItemDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const {
    Q_UNUSED(option);
    Q_UNUSED(index);
    // компактный ритм: строка примерно высота терминальной линии
    QFont font = Theme::getMonospaceFont(10);
    QFontMetrics fm(font);
    return QSize(0, fm.height() + 1);
}

}
