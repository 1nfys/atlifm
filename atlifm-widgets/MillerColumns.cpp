#include "MillerColumns.hpp"
#include "ContextMenu.hpp"
#include "NativePlugins.hpp"
#include "TuiPanesOverlay.hpp"
#include "ArchiveTreeWidget.hpp"
#include "../atlifm-models/ArchiveModel.hpp"
#include <QProcess>
#include <QProcessEnvironment>
#include <QMimeData>
#include <QMenu>
#include <QWidgetAction>
#include <QLabel>
#include <QDrag>
#include "TuiInputPopup.hpp"
#include "BulkCreateDialog.hpp"
#include "../atlifm-theme/Theme.hpp"
#include "../atlifm-core/XdgUtils.hpp"
#include "../atlifm-core/Plugins.hpp"
#include "../atlifm-core/UndoManager.hpp"
#include "../atlifm-core/tasks/TaskManager.hpp"
#include "../atlifm-dnd/DragDropHandler.hpp"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QEvent>
#include <QApplication>
#include <QClipboard>
#include <QTimer>
#include <QPointer>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QImageReader>
#include <QPainter>
#include <QFontMetricsF>
#include <QSettings>
#include <QDebug>
#include <algorithm>
#include <fstream>
#include <unordered_set>
#include <unistd.h>

namespace Widgets {

namespace {

// конвертация пути файловой системы и строки qstring
inline QString toQ(const std::filesystem::path& p) {
    return QString::fromUtf8(p.c_str());
}
inline std::filesystem::path toFs(const QString& s) {
    return std::filesystem::path(s.toUtf8().toStdString());
}

// path лежит внутри dir (или совпадает с ним); без ложных срабатываний на /home/user2
inline bool isInsideDir(const std::filesystem::path& p, const std::filesystem::path& dir) {
    const std::filesystem::path pp(p);
    const std::filesystem::path dd(dir);
    auto pi = pp.begin();
    for (auto di = dd.begin(); di != dd.end(); ++di, ++pi) {
        if (pi == pp.end() || *pi != *di) return false;
    }
    return true;
}

// имя из встроенного приглашения (rename / new folder / new file): пустое, с '/',
// с NUL или "."/".." недопустимо; иначе ввод («../evil», «a/b») создал бы элемент
// вне текущего каталога или превратил rename в произвольный перенос
QString validateItemName(const QString& name) {
    if (name.isEmpty()) return QStringLiteral("Name cannot be empty");
    if (name.contains(QLatin1Char('/'))) return QStringLiteral("Name cannot contain '/'");
    if (name.contains(QChar(u'\0'))) return QStringLiteral("Name contains an invalid character");
    if (name == QLatin1String(".") || name == QLatin1String(".."))
        return QStringLiteral("Name is reserved");
    return {};
}

}

// MillerListView

MillerListView::MillerListView(bool isParentPane, QWidget* parent)
    : QListView(parent), m_isParentPane(isParentPane) {
    setAcceptDrops(true);
    // отключаем внутренний драг QAbstractItemView: конфликтует с нашим QDrag
    // (DragDropHandler::createDrag) и выделяет строки протяжкой
    setDragEnabled(false);
    setDropIndicatorShown(false);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // скроллбары убраны полностью: прокрутка колесом и клавиатурой, полосы не рисуются
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setUniformItemSizes(true);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
}

void MillerListView::setDropTargetRow(int row) {
    auto del = qobject_cast<FileItemDelegate*>(itemDelegate());
    if (del && del->dropTargetRow() != row) {
        del->setDropTargetRow(row);
        viewport()->update();
    }
}

void MillerListView::updateAnchor(int row, const std::filesystem::path& path) {
    m_selectionAnchorRow = row;
    m_selectionAnchorPath = path;
}

int MillerListView::resolveAnchorRow() const {
    if (!model()) return -1;
    auto fsModel = qobject_cast<Models::FileSystemModel*>(model());
    if (fsModel && !m_selectionAnchorPath.empty()) {
        int r = fsModel->findRowByPath(m_selectionAnchorPath);
        if (r >= 0) return r;
    }
    if (m_selectionAnchorRow >= 0 && m_selectionAnchorRow < model()->rowCount()) {
        return m_selectionAnchorRow;
    }
    if (currentIndex().isValid()) {
        return currentIndex().row();
    }
    return 0;
}

void MillerListView::clearAnchor() {
    m_selectionAnchorRow = -1;
    m_selectionAnchorPath.clear();
}

void MillerListView::currentChanged(const QModelIndex& current, const QModelIndex& previous) {
    QListView::currentChanged(current, previous);
    if (!m_preservingAnchor && !(QApplication::keyboardModifiers() & Qt::ShiftModifier) && current.isValid()) {
        QString p = current.data(Models::FileSystemModel::PathRole).toString();
        updateAnchor(current.row(), std::filesystem::path(p.toUtf8().toStdString()));
    }
}

void MillerListView::keyPressEvent(QKeyEvent* event) {
    emit selectionInteractionStarted();
    if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Backspace) {
        emit navigateToParentRequested();
        return;
    }

    if (event->key() == Qt::Key_Right || event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        QModelIndex currentIdx = currentIndex();
        if (currentIdx.isValid()) {
            bool isDir = currentIdx.data(Models::FileSystemModel::IsDirRole).toBool();
            QString pathStr = currentIdx.data(Models::FileSystemModel::PathRole).toString();
            std::filesystem::path p(pathStr.toUtf8().toStdString());
            if (isDir) {
                emit enterDirectoryRequested(p);
            } else {
                emit openFileRequested(p);
            }
        }
        return;
    }

    if (event->key() == Qt::Key_Space) {
        QModelIndex currentIdx = currentIndex();
        if (currentIdx.isValid()) {
            emit toggleSelectionRequested(currentIdx.row());
            if (model() && currentIdx.row() + 1 < model()->rowCount()) {
                QModelIndex nextIdx = model()->index(currentIdx.row() + 1, 0);
                if (selectionModel()) {
                    selectionModel()->setCurrentIndex(nextIdx, QItemSelectionModel::NoUpdate);
                }
                scrollTo(nextIdx, QAbstractItemView::EnsureVisible);
            }
        }
        return;
    }

    if (event->key() == Qt::Key_Slash) {
        emit filterTriggered();
        return;
    }

    if (event->key() == Qt::Key_Delete) {
        if (event->modifiers() & Qt::ShiftModifier) {
            emit deletePermanentlyRequested();
        } else {
            emit trashRequested();
        }
        event->accept();
        return;
    }

    // Shift+стрелки/Home/End/PageUp/PageDown: детерминированный диапазон от anchor;
    // NoUpdate потому что setCurrentIndex вызывает selectionCommand и сбрасывает выбор
    // и сбрасывает весь диапазон Shift
    {
        int k = event->key();
        bool isNav = (k == Qt::Key_Up || k == Qt::Key_Down ||
                      k == Qt::Key_Home || k == Qt::Key_End ||
                      k == Qt::Key_PageUp || k == Qt::Key_PageDown);
        if (isNav && (event->modifiers() & Qt::ShiftModifier) && model()) {
            int count = model()->rowCount();
            if (count > 0) {
                int curRow = currentIndex().isValid() ? currentIndex().row() : 0;
                int target = curRow;
                if (k == Qt::Key_Up) target = std::max(0, curRow - 1);
                else if (k == Qt::Key_Down) target = std::min(count - 1, curRow + 1);
                else if (k == Qt::Key_Home) target = 0;
                else if (k == Qt::Key_End) target = count - 1;
                else if (k == Qt::Key_PageUp) target = std::max(0, curRow - 10);
                else if (k == Qt::Key_PageDown) target = std::min(count - 1, curRow + 10);
                int anchor = resolveAnchorRow();
                if (anchor < 0 || anchor >= count) {
                    anchor = curRow;
                    QString ap = model()->index(anchor, 0).data(Models::FileSystemModel::PathRole).toString();
                    updateAnchor(anchor, std::filesystem::path(ap.toUtf8().toStdString()));
                }
                int lo = std::min(anchor, target);
                int hi = std::max(anchor, target);
                QModelIndex targetIdx = model()->index(target, 0);
                m_preservingAnchor = true;
                if (selectionModel()) {
                    selectionModel()->setCurrentIndex(targetIdx, QItemSelectionModel::NoUpdate);
                    QItemSelection range;
                    range.select(model()->index(lo, 0), model()->index(hi, 0));
                    bool add = (event->modifiers() & Qt::ControlModifier);
                    selectionModel()->select(range, (add ? QItemSelectionModel::Select
                                                         : QItemSelectionModel::ClearAndSelect)
                                                    | QItemSelectionModel::Rows);
                }
                m_preservingAnchor = false;
                scrollTo(targetIdx, QAbstractItemView::EnsureVisible);
            }
            event->accept();
            return;
        }
        if (isNav && !(event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier))) {
            QListView::keyPressEvent(event);
            if (currentIndex().isValid()) {
                QString p = currentIndex().data(Models::FileSystemModel::PathRole).toString();
                updateAnchor(currentIndex().row(), std::filesystem::path(p.toUtf8().toStdString()));
            }
            return;
        }
    }

    // поиск по вводу: любой печатный символ сразу запускает поиск
    if (!m_isParentPane && !event->text().isEmpty() && event->modifiers() == Qt::NoModifier) {
        QChar ch = event->text().at(0);
        if (ch.isPrint() && ch != ' ' && ch != '?') {
            emit typeAheadSearchTriggered(event->text());
            return;
        }
    }

    QListView::keyPressEvent(event);
}

void MillerListView::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::BackButton) {
        emit navigateHistoryBackRequested();
        event->accept();
        return;
    }
    if (event->button() == Qt::ForwardButton) {
        emit navigateHistoryForwardRequested();
        event->accept();
        return;
    }
    // нажатие на колесо мыши по папке: открыть её в новой вкладке
    if (event->button() == Qt::MiddleButton) {
        const QModelIndex idx = indexAt(event->pos());
        if (idx.isValid()) {
            const QString pathStr = idx.data(Models::FileSystemModel::PathRole).toString();
            if (!pathStr.isEmpty()) {
                const std::filesystem::path path(pathStr.toUtf8().toStdString());
                std::error_code ec;
                if (std::filesystem::is_directory(path, ec)) {
                    emit openInNewTabRequested(path);
                }
            }
        }
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton || event->button() == Qt::RightButton) {
        emit selectionInteractionStarted();
    }

    if (event->button() == Qt::LeftButton) {
        // зоны gutter запускают rubberband даже на элементах: в маленьких окнах список
        // занимает весь viewport и пустого места нет; левые 16px совпадают с leftGutter
        // в FileItemDelegate, правые 18px зона захвата
        bool inLeftGutter = (event->pos().x() < 16);
        bool inRightGutter = (event->pos().x() >= viewport()->width() - 18);
        QModelIndex idx = (inLeftGutter || inRightGutter) ? QModelIndex() : indexAt(event->pos());

        if (!idx.isValid()) {
            // клик по пустому месту или gutter запускает выделение рамкой
            m_isRubberBanding = true;
            m_dragMode = DragMode::BoxSelect;
            m_clearOthersOnRelease = false;
            m_skipBaseRelease = false;
            m_dragStartIndex = QPersistentModelIndex();
            m_rubberBandOrigin = event->pos();
            if (!m_rubberBand) {
                m_rubberBand = new QRubberBand(QRubberBand::Rectangle, viewport());
            }
            m_rubberBand->setGeometry(QRect(m_rubberBandOrigin, QSize()));
            m_rubberBand->show();

            if (!(event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier))) {
                clearSelection();
                setCurrentIndex(QModelIndex());
                clearAnchor();
            }
            event->accept();
            return;
        }

        // Shift+Click: детерминированный выбор от anchor; press и release обрабатываем сами
        // (m_skipBaseRelease), т.к. Qt не видит этот press и его internal anchor затёр бы диапазон;
        // NoUpdate, чтобы setCurrentIndex не вызвал ClearAndSelect и не стёр диапазон
        if (event->modifiers() & Qt::ShiftModifier) {
            int clickedRow = idx.row();
            int anchorRow = resolveAnchorRow();
            if (anchorRow < 0 || (model() && anchorRow >= model()->rowCount())) {
                anchorRow = currentIndex().isValid() ? currentIndex().row() : clickedRow;
                QString ap = model()->index(anchorRow, 0).data(Models::FileSystemModel::PathRole).toString();
                updateAnchor(anchorRow, std::filesystem::path(ap.toUtf8().toStdString()));
            }

            int startRow = std::min(anchorRow, clickedRow);
            int endRow = std::max(anchorRow, clickedRow);

            m_preservingAnchor = true;
            if (selectionModel() && model()) {
                selectionModel()->setCurrentIndex(idx, QItemSelectionModel::NoUpdate);
                QItemSelection rangeSelection;
                rangeSelection.select(model()->index(startRow, 0), model()->index(endRow, 0));

                if (event->modifiers() & Qt::ControlModifier) {
                    selectionModel()->select(rangeSelection, QItemSelectionModel::Select | QItemSelectionModel::Rows);
                } else {
                    selectionModel()->select(rangeSelection, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
                }
            }
            m_preservingAnchor = false;

            m_dragMode = DragMode::None;
            m_clearOthersOnRelease = false;
            m_skipBaseRelease = true;
            event->accept();
            return;
        }

        m_dragStartPos = event->pos();
        m_dragStartIndex = idx;
        m_clearOthersOnRelease = false;
        m_skipBaseRelease = false;
        m_dragMode = DragMode::None;

        const bool ctrlHeld = (event->modifiers() & Qt::ControlModifier);

        // Ctrl+Click переключает только эту строку; Ctrl+drag продолжает расширять от неё через Qt
        if (ctrlHeld) {
            QString p = idx.data(Models::FileSystemModel::PathRole).toString();
            updateAnchor(idx.row(), toFs(p));
            QListView::mousePressEvent(event);
            return;
        }

        // клик или нажатие на элемент готовит возможный DragOut (DnD), рамка только
        // для пустого места или gutter. Если элемент не выделен, выделяем и ставим
        // фокус; если в мультивыделении, сохраняем его для драга, а простое нажатие
        // без драга сворачивает выделение до одного элемента при отпускании
        if (!m_isParentPane) {
            m_dragMode = DragMode::DragOut;
            QString p = idx.data(Models::FileSystemModel::PathRole).toString();
            updateAnchor(idx.row(), toFs(p));

            bool alreadySelected = selectionModel() && selectionModel()->isSelected(idx);
            if (!alreadySelected) {
                m_preservingAnchor = true;
                if (selectionModel()) {
                    selectionModel()->setCurrentIndex(idx, QItemSelectionModel::NoUpdate);
                    selectionModel()->select(idx, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
                }
                m_preservingAnchor = false;
                m_clearOthersOnRelease = false;
            } else {
                selectionModel()->setCurrentIndex(idx, QItemSelectionModel::NoUpdate);
                m_clearOthersOnRelease = selectionModel()->selectedIndexes().size() > 1;
            }
            m_skipBaseRelease = true;
            event->accept();
            return;
        }

        // родительская панель сохраняет стандартный press Qt (вытаскивание одного соседа);
        // Ctrl там обычный переключатель, поэтому драг не начинается
        m_dragMode = ctrlHeld ? DragMode::None : DragMode::DragOut;
        m_selectionAnchorRow = idx.row();
        QListView::mousePressEvent(event);
        return;
    } else if (event->button() == Qt::RightButton) {
        m_dragMode = DragMode::None;
        m_isRubberBanding = false;
        if (m_rubberBand) {
            m_rubberBand->hide();
        }
        QModelIndex idx = indexAt(event->pos());
        if (idx.isValid()) {
            if (!selectionModel()->isSelected(idx)) {
                selectionModel()->clearSelection();
                setCurrentIndex(idx);
                selectionModel()->select(idx, QItemSelectionModel::Select);
                m_selectionAnchorRow = idx.row();
            }
        } else {
            selectionModel()->clearSelection();
            setCurrentIndex(QModelIndex());
            m_selectionAnchorRow = -1;
        }
        emit contextMenuRequested(event->globalPosition().toPoint(), idx);
        event->accept();
        return;
    }
    QListView::mousePressEvent(event);
}

void MillerListView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        QModelIndex idx = indexAt(event->pos());
        if (idx.isValid()) {
            bool isDir = idx.data(Models::FileSystemModel::IsDirRole).toBool();
            QString pathStr = idx.data(Models::FileSystemModel::PathRole).toString();
            std::filesystem::path p(pathStr.toUtf8().toStdString());
            if (isDir) {
                emit enterDirectoryRequested(p);
            } else {
                emit openFileRequested(p);
            }
            event->accept();
            return;
        }
    }
    QListView::mouseDoubleClickEvent(event);
}

void MillerListView::mouseMoveEvent(QMouseEvent* event) {
    // протяжка по строкам с зажатым shift: живое расширение выделения от якоря к текущей строке
    if ((event->buttons() & Qt::LeftButton) && !m_isRubberBanding
        && (event->modifiers() & Qt::ShiftModifier) && model()) {
        QModelIndex hovered = indexAt(event->pos());
        if (hovered.isValid()) {
            int count = model()->rowCount();
            // якорь резолвим по пути: после фонового пересканирования строка могла сместиться
            int anchor = resolveAnchorRow();
            if (anchor < 0 || anchor >= count) {
                anchor = currentIndex().isValid() ? currentIndex().row() : hovered.row();
            }
            int lo = std::min(anchor, hovered.row());
            int hi = std::max(anchor, hovered.row());
            m_preservingAnchor = true;
            if (selectionModel()) {
                selectionModel()->setCurrentIndex(hovered, QItemSelectionModel::NoUpdate);
                QItemSelection range;
                range.select(model()->index(lo, 0), model()->index(hi, 0));
                bool add = (event->modifiers() & Qt::ControlModifier);
                selectionModel()->select(range, (add ? QItemSelectionModel::Select
                                                     : QItemSelectionModel::ClearAndSelect)
                                                | QItemSelectionModel::Rows);
            }
            m_preservingAnchor = false;
            scrollTo(hovered, QAbstractItemView::EnsureVisible);
            // release обрабатываем сами, иначе Qt применит свой устаревший диапазон от anchor
            m_skipBaseRelease = true;
        }
        event->accept();
        return;
    }
    if (m_isRubberBanding) {
        // ограничиваем viewport, чтобы рамка не улетала в маленьких окнах
        QPoint clamped = event->pos();
        clamped.setX(std::clamp(clamped.x(), 0, viewport()->width()));
        clamped.setY(std::clamp(clamped.y(), 0, viewport()->height()));
        QRect box = QRect(m_rubberBandOrigin, clamped).normalized();
        if (m_rubberBand) {
            m_rubberBand->setGeometry(box);
        }

        // автопрокрутка у верхнего/нижнего края: выделение работает, когда список выше окна
        if (event->pos().y() < 20) {
            verticalScrollBar()->triggerAction(QAbstractSlider::SliderSingleStepSub);
        } else if (event->pos().y() > viewport()->height() - 20) {
            verticalScrollBar()->triggerAction(QAbstractSlider::SliderSingleStepAdd);
        }

        // пересечение рамки с visualRect элементов в реальном времени
        // (работает и для архивного листинга с виртуальными путями)
        if (model()) {
            QItemSelection selection;
            for (int r = 0; r < model()->rowCount(); ++r) {
                QModelIndex itemIdx = model()->index(r, 0);
                QRect itemR = visualRect(itemIdx);
                if (!itemR.isValid() || itemR.isEmpty()) continue;
                if (box.intersects(itemR)) {
                    selection.select(itemIdx, itemIdx);
                }
            }
            // Ctrl или Shift расширяет, обычный драг заменяет
            if (event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier)) {
                selectionModel()->select(selection, QItemSelectionModel::Select);
            } else {
                selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect);
            }
        }
        event->accept();
        return;
    }

    // QDrag при прохождении порога от press с DragOut; press по невыделенному элементу
    // обрабатывает ветка рамки выше; драг с Shift расширяет выделение и никогда не стартует DnD
    if (m_dragMode == DragMode::DragOut && (event->buttons() & Qt::LeftButton)
        && !(event->modifiers() & Qt::ShiftModifier)) {
        int distance = (event->pos() - m_dragStartPos).manhattanLength();
        if (distance >= QApplication::startDragDistance()) {
            std::vector<std::filesystem::path> paths = dragOutPaths(m_dragStartIndex);
            m_dragMode = DragMode::None;
            m_clearOthersOnRelease = false;

            if (!paths.empty()) {
                startDragOut(paths);
                return;
            }
        }
        // не передаём драг элемента левой кнопкой в QListView::mouseMoveEvent (иначе случайное выделение протяжкой)
        event->accept();
        return;
    }

    QListView::mouseMoveEvent(event);
}

std::vector<std::filesystem::path> MillerListView::dragOutPaths(const QModelIndex& startIndex) const {
    std::vector<std::filesystem::path> paths;
    auto fsModel = qobject_cast<Models::FileSystemModel*>(model());
    if (!fsModel) return paths;

    if (!m_isParentPane) {
        paths = fsModel->selectedPaths();
    }
    if (startIndex.isValid()) {
        QString startPathStr = startIndex.data(Models::FileSystemModel::PathRole).toString();
        if (!startPathStr.isEmpty()) {
            std::filesystem::path startPath(startPathStr.toUtf8().toStdString());
            if (paths.empty() || std::find(paths.begin(), paths.end(), startPath) == paths.end()) {
                paths.clear();
                paths.push_back(std::move(startPath));
            }
        }
    }
    if (paths.empty()) {
        QModelIndex curr = currentIndex();
        if (curr.isValid()) {
            QString pathStr = curr.data(Models::FileSystemModel::PathRole).toString();
            if (!pathStr.isEmpty()) {
                paths.push_back(std::filesystem::path(pathStr.toUtf8().toStdString()));
            }
        }
    }
    return paths;
}

void MillerListView::startDragOut(const std::vector<std::filesystem::path>& paths) {
    if (paths.empty()) return;
    QDrag* drag = Dnd::DragDropHandler::createDrag(paths, this);
    if (drag) {
        drag->mimeData()->setData(Dnd::DragDropHandler::kMovePreferredMime, "1");
        drag->exec(Qt::CopyAction | Qt::MoveAction | Qt::LinkAction, Qt::CopyAction);
    }
}

void MillerListView::mouseReleaseEvent(QMouseEvent* event) {
    if (m_isRubberBanding) {
        m_isRubberBanding = false;
        m_dragMode = DragMode::None;
        if (m_rubberBand) {
            m_rubberBand->hide();
        }
        // курсор/anchor = ряд, с которого начался жест, а не последний выделенный;
        // NoUpdate, т.к. setCurrentIndex() вызывает ClearAndSelect и затирал рамку
        int dist = (event->pos() - m_rubberBandOrigin).manhattanLength();
        if (dist >= QApplication::startDragDistance()) {
            int anchorRow = -1;
            if (m_dragStartIndex.isValid()) {
                anchorRow = m_dragStartIndex.row();
            } else if (selectionModel() && !selectionModel()->selectedIndexes().isEmpty()) {
                anchorRow = selectionModel()->selectedIndexes().first().row();
            }
            if (anchorRow >= 0 && model() && anchorRow < model()->rowCount() && selectionModel()) {
                QModelIndex anchorIdx = model()->index(anchorRow, 0);
                m_preservingAnchor = true;
                selectionModel()->setCurrentIndex(anchorIdx, QItemSelectionModel::NoUpdate);
                m_preservingAnchor = false;
                QString p = anchorIdx.data(Models::FileSystemModel::PathRole).toString();
                updateAnchor(anchorRow, std::filesystem::path(p.toUtf8().toStdString()));
            }
        }
        m_dragStartIndex = QPersistentModelIndex();
        m_clearOthersOnRelease = false;
        m_skipBaseRelease = false;
        event->accept();
        return;
    }
    if (m_clearOthersOnRelease && event->button() == Qt::LeftButton && m_dragStartIndex.isValid()) {
        if (selectionModel()) {
            selectionModel()->select(m_dragStartIndex, QItemSelectionModel::ClearAndSelect);
        }
        setCurrentIndex(m_dragStartIndex);
    }
    m_clearOthersOnRelease = false;
    m_dragMode = DragMode::None;
    // Press поглощён нами (Shift+Click / сохранение мультивыбора): Qt его не видел,
    // поэтому base release пропускаем, иначе Qt затрёт выделение
    if (m_skipBaseRelease && event->button() == Qt::LeftButton) {
        m_skipBaseRelease = false;
        m_dragStartIndex = QPersistentModelIndex();
        event->accept();
        return;
    }
    m_skipBaseRelease = false;
    m_dragStartIndex = QPersistentModelIndex();
    QListView::mouseReleaseEvent(event);
}

void MillerListView::dragEnterEvent(QDragEnterEvent* event) {
    if (Dnd::DragDropHandler::canAcceptDrop(event->mimeData())) {
        Dnd::DragDropHandler::acceptDrag(event);
    } else {
        event->ignore();
    }
}

void MillerListView::dragMoveEvent(QDragMoveEvent* event) {
    if (Dnd::DragDropHandler::canAcceptDrop(event->mimeData())) {
        QModelIndex idx = indexAt(event->position().toPoint());
        if (idx.isValid() && idx.data(Models::FileSystemModel::IsDirRole).toBool()) {
            setDropTargetRow(idx.row());
        } else {
            setDropTargetRow(-1);
        }
        Dnd::DragDropHandler::acceptDrag(event);
    } else {
        event->ignore();
    }
}

void MillerListView::dragLeaveEvent(QDragLeaveEvent* /*event*/) {
    setDropTargetRow(-1);
}

void MillerListView::dropEvent(QDropEvent* event) {
    setDropTargetRow(-1);

    if (!Dnd::DragDropHandler::canAcceptDrop(event->mimeData())) {
        event->ignore();
        return;
    }

    QModelIndex idx = indexAt(event->position().toPoint());
    std::filesystem::path targetDir;

    auto fsModel = qobject_cast<Models::FileSystemModel*>(model());
    if (idx.isValid() && idx.data(Models::FileSystemModel::IsDirRole).toBool()) {
        QString pathStr = idx.data(Models::FileSystemModel::PathRole).toString();
        targetDir = std::filesystem::path(pathStr.toUtf8().toStdString());
    } else if (fsModel) {
        targetDir = fsModel->currentDirectory();
    }

    if (targetDir.empty()) {
        event->ignore();
        return;
    }

    auto paths = Dnd::DragDropHandler::extractPaths(event->mimeData());
    if (!paths.empty()) {
        const Qt::DropAction action = Dnd::DragDropHandler::effectiveAction(event);
        event->setDropAction(action);
        emit filesDropped(paths, targetDir, action, event->modifiers());
        event->accept();
        return;
    }

    // входящий drop сырого изображения, data: URI или удалённого URL из внешнего браузера / AI чата
    if (Dnd::DragDropHandler::handleMimeDrop(event->mimeData(), targetDir,
                                                 event->proposedAction(), event->modifiers(),
                                                 parentWidget())) {
        event->acceptProposedAction();
        return;
    }

    event->ignore();
}

// MillerViewContainer

// кнопка сортировки 20×20: глиф центрируется по чернилам (tightBoundingRect),
// т.к. у Nerd ink глифов смещение от центра ячейки и обычный QPushButton
// показывал иконку не по центру
class SortGlyphButton : public QPushButton {
public:
    explicit SortGlyphButton(QWidget* parent = nullptr)
        : QPushButton(parent) {
        setFixedSize(20, 20);
        setText(QString::fromUtf8("\xEF\x82\xB0"));
        setFont(Theme::getMonospaceFont(10));
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        if (underMouse()) {
            p.fillRect(rect(), Theme::Colors::Surface1());
        }
        const QString glyph = text();
        if (glyph.isEmpty()) return;
        const QFontMetricsF fm(font());
        const QRectF ink = fm.tightBoundingRect(glyph);
        p.setPen(underMouse() ? Theme::Colors::Accent() : Theme::Colors::Subtext());
        // точный центр через QRectF: QRect::center() на чётной стороне округляет
        // вниз и глиф уезжает влево/вверх
        const QPointF cellCenter = QRectF(rect()).center();
        p.drawText(QPointF(cellCenter.x() - ink.center().x(),
                           cellCenter.y() - ink.center().y()), glyph);
    }
};

MillerViewContainer::MillerViewContainer(QWidget* parent)
    : QWidget(parent) {
    setupUi();
}

MillerViewContainer::~MillerViewContainer() {
    // плагин может ещё работать (дочерний QProcess): его уничтожение убьёт процесс,
    // а finished синхронно вызвал бы refresh по разрушаемому дереву виджетов;
    // отключаем хендлеры заранее
    for (QProcess* proc : findChildren<QProcess*>(QString(), Qt::FindDirectChildrenOnly))
        proc->disconnect(this);
}

void MillerViewContainer::setupUi() {
    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    m_viewStack = new QStackedWidget(this);
    m_viewStack->setObjectName(QStringLiteral("millerViewStack"));
    if (auto* stackLayout = m_viewStack->layout()) {
        stackLayout->setContentsMargins(0, 0, 0, 0);
        stackLayout->setSpacing(0);
    }

    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setObjectName(QStringLiteral("millerSplitter"));
    m_splitter->setHandleWidth(2); // тонкий разделитель колонок, подсветка при наведении акцентом
    m_splitter->setChildrenCollapsible(false);
    m_splitter->setOpaqueResize(true); // плавный ресайз в реальном времени, без рваной фантомной линии

    // слева: родительская колонка
    m_parentView = new MillerListView(true, this);
    m_parentView->setMinimumWidth(120);
    m_parentView->setMaximumWidth(320);
    m_parentModel = new Models::FileSystemModel(this);
    m_parentDelegate = new FileItemDelegate(true, this);
    m_parentView->setModel(m_parentModel);
    m_parentView->setItemDelegate(m_parentDelegate);

    // в центре: активная колонка
    m_centerView = new MillerListView(false, this);
    m_centerView->setMinimumWidth(160);
    m_centerModel = new Models::FileSystemModel(this);
    m_centerDelegate = new FileItemDelegate(false, this);
    m_centerView->setModel(m_centerModel);
    m_centerView->setItemDelegate(m_centerDelegate);

    // центральная колонка: список файлов или содержимое архива, без растягивания
    // на всю область Miller и без затрагивания соседних колонок
    m_centerStack = new QStackedWidget(this);
    m_centerStack->setMinimumWidth(160);
    m_centerStack->addWidget(m_centerView);

    // пустой центр при permission denied: подсказка поверх списка; «try to sudo open»
    // кликабельна (sudo open этого каталога), остальное прозрачна для мыши
    m_centerDeniedOverlay = new QWidget(m_centerView->viewport());
    auto deniedLay = new QVBoxLayout(m_centerDeniedOverlay);
    deniedLay->setContentsMargins(0, 0, 0, 0);
    deniedLay->setSpacing(6);
    deniedLay->addStretch(1);

    m_centerDeniedHint = new QLabel(m_centerDeniedOverlay);
    m_centerDeniedHint->setAlignment(Qt::AlignCenter);
    m_centerDeniedHint->setWordWrap(true);
    m_centerDeniedHint->setTextFormat(Qt::RichText);
    m_centerDeniedHint->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_centerDeniedHint->setFont(Theme::getMonospaceFont(10));
    deniedLay->addWidget(m_centerDeniedHint);

    m_centerDeniedAction = new QLabel(m_centerDeniedOverlay);
    m_centerDeniedAction->setAlignment(Qt::AlignCenter);
    m_centerDeniedAction->setTextFormat(Qt::RichText);
    m_centerDeniedAction->setFont(Theme::getMonospaceFont(10));
    m_centerDeniedAction->setCursor(Qt::PointingHandCursor);
    m_centerDeniedAction->setToolTip(QStringLiteral("sudo open"));
    m_centerDeniedAction->installEventFilter(this);
    deniedLay->addWidget(m_centerDeniedAction);

    deniedLay->addStretch(1);
    m_centerDeniedOverlay->hide();
    m_centerView->viewport()->installEventFilter(this);

    // справа: колонка превью
    m_previewPane = new PreviewPane(this);
    m_previewPane->setMinimumWidth(120);

    m_splitter->addWidget(m_parentView);
    m_splitter->addWidget(m_centerStack);
    m_splitter->addWidget(m_previewPane);
    // превью не коллапсируется до нуля: иначе рукоятку между центром и превью
    // перехватывает зона ресайза окна, и полоска уходит за интерфейс без возврата
    m_splitter->setCollapsible(2, false);

    // левая колонка компактная (~160px), центр забирает основное растяжение (3), превью уже (1)
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 3);
    m_splitter->setStretchFactor(2, 1);
    m_splitter->setSizes({300, 480, 420});
    // После восстановления layout колонки держим ровно сохранённой ширины,
    // пока пользователь сам не потянет рукоятку. Иначе первый resize от WM
    // мог применить ширины к промежуточному размеру окна а следующий
    // пропорционально растянуть их и снова получить «не ту» раскладку.
    connect(m_splitter, &QSplitter::splitterMoved, this, [this]() {
        m_exactInitialWidths = false;
        m_initialSizesApplied = true;
    });

    // кнопка с иконкой сортировки в углу центральной колонки (глиф центрируется по чернилам в SortGlyphButton)
    m_sortButton = new SortGlyphButton(m_centerView);
    m_sortButton->move(0, 0);
    m_sortButton->setToolTip("Sort by (right-click: reset)");
    connect(m_sortButton, &QPushButton::clicked, this, &MillerViewContainer::showSortMenu);
    // ПКМ по иконке сортировки: сброс папки к дефолту (Name ascending)
    m_sortButton->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_sortButton, &QPushButton::customContextMenuRequested, this,
            [this](const QPoint&) { resetSortToDefault(); });
    m_sortButton->show();
    m_sortButton->raise();

    mainLayout->addWidget(m_viewStack, 1);

    m_viewStack->addWidget(m_splitter);
    m_pluginPanes = new TuiPanesOverlay(m_viewStack);
    connect(m_pluginPanes, &TuiPanesOverlay::closeRequested,
            this, [this]() { closePluginPanes(); });
    m_viewStack->addWidget(m_pluginPanes);
    m_viewStack->setCurrentWidget(m_splitter);

    // Страница архива живёт только в центральной колонке: левая и правая
    // колонки остаются на месте, путь вкладки при этом не меняется.
    m_archivePage = new QWidget(m_centerStack);
    auto* archiveLayout = new QVBoxLayout(m_archivePage);
    archiveLayout->setContentsMargins(0, 0, 0, 0);
    archiveLayout->setSpacing(0);

    auto* archiveHeader = new QWidget(m_archivePage);
    auto* archiveHeaderLayout = new QHBoxLayout(archiveHeader);
    archiveHeaderLayout->setContentsMargins(6, 4, 8, 4);
    archiveHeaderLayout->setSpacing(8);

    m_archiveBackBtn = new QPushButton(QStringLiteral("\u2190 Back"), archiveHeader);
    m_archiveBackBtn->setAutoDefault(false);
    m_archiveBackBtn->setFixedHeight(24);
    m_archiveBackBtn->setCursor(Qt::PointingHandCursor);
    connect(m_archiveBackBtn, &QPushButton::clicked, this, &MillerViewContainer::leaveArchive);
    archiveHeaderLayout->addWidget(m_archiveBackBtn, 0, Qt::AlignVCenter);

    archiveHeaderLayout->addStretch(1);
    archiveLayout->addWidget(archiveHeader);

    m_archiveModel = new Models::ArchiveModel(this);
    m_archiveTree = new ArchiveTreeWidget(m_archivePage);
    m_archiveTree->setArchiveModel(m_archiveModel);
    m_archiveTree->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_archiveTree->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_archiveTree->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_archiveTree->installEventFilter(this);
    archiveLayout->addWidget(m_archiveTree, 1);
    m_centerStack->addWidget(m_archivePage);
    m_centerStack->setCurrentWidget(m_centerView);

    connect(m_archiveModel, &Models::ArchiveModel::archiveReset, this, [this]() {
        if (m_archiveTree && !m_archivePath.empty()) m_archiveTree->clearArchive();
    });
    connect(m_archiveModel, &Models::ArchiveModel::loaded, this, [this]() {
        if (!m_archiveTree || m_archivePath.empty() || !m_archiveModel) return;
        if (m_archiveModel->archivePath() != m_archivePath) return;
        m_archiveTree->setArchivePath(m_archivePath);
        m_archiveTree->buildFromEntries(m_archiveModel->entries());
    });
    connect(m_archiveModel, &Models::ArchiveModel::loadFailed, this, [this](const QString& error) {
        if (m_archiveTree && !m_archivePath.empty()) m_archiveTree->showMessage(error);
    });
    connect(m_archiveTree, &ArchiveTreeWidget::messageRequested,
            this, &MillerViewContainer::messageEmitted);
    connect(m_archiveTree, &ArchiveTreeWidget::entrySelected,
            this, &MillerViewContainer::onArchiveEntrySelected);

    // подключаем изменения выделения и курсора
    connect(m_centerView->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &MillerViewContainer::onCenterSelectionChanged);
    connect(m_centerView->selectionModel(), &QItemSelectionModel::currentChanged,
            this, [this](const QModelIndex& /*current*/, const QModelIndex& /*previous*/) {
        updateCenterSelection();
    });

    // Qt синхронно очищает выделение вида внутри begin/endResetModel (фоновые
    // пересканирования, ввод фильтра); ложный selectionChanged затёр бы m_selectedPaths
    // и свернул фокус до одного файла, поэтому блокируем его и восстанавливаем
    connect(m_centerModel, &QAbstractItemModel::modelAboutToBeReset, this, [this]() {
        if (m_centerView->currentIndex().isValid()) {
            m_savedCurrentPath = m_centerView->currentIndex().data(Models::FileSystemModel::PathRole).toString();
        } else {
            m_savedCurrentPath.clear();
        }
        if (auto* sm = m_centerView->selectionModel()) sm->blockSignals(true);
    });
    connect(m_centerModel, &QAbstractItemModel::modelReset, this, [this]() {
        if (auto* sm = m_centerView->selectionModel()) sm->blockSignals(false);
        // IsSelected сохранились на стороне модели через m_selectedPaths, применяем
        // их повторно к selectionModel, который Qt только что очистил
        auto selPaths = m_centerModel->selectedPaths();
        auto* sm = m_centerView->selectionModel();
        if (sm) {
            if (!selPaths.empty()) {
                QItemSelection qs;
                for (const auto& sp : selPaths) {
                    int r = m_centerModel->findRowByPath(sp);
                    if (r >= 0) {
                        QModelIndex mi = m_centerModel->index(r, 0);
                        qs.select(mi, mi);
                    }
                }
                if (!qs.isEmpty()) sm->select(qs, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
                else sm->clearSelection();
            } else {
                sm->clearSelection();
            }
        }
        // восстанавливаем курсор: сначала сохранённый путь, иначе последняя известная строка
        int restoreRow = -1;
        if (!m_savedCurrentPath.isEmpty()) {
            restoreRow = m_centerModel->findRowByPath(
                std::filesystem::path(m_savedCurrentPath.toUtf8().toStdString()));
        }
        if (restoreRow < 0 && m_centerModel->rowCount() > 0) {
            restoreRow = std::clamp(m_lastKnownRow, 0, m_centerModel->rowCount() - 1);
        }
        if (restoreRow >= 0 && sm) {
            QModelIndex ri = m_centerModel->index(restoreRow, 0);
            sm->setCurrentIndex(ri, QItemSelectionModel::NoUpdate);
            if (selPaths.size() <= 1) {
                sm->select(ri, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            }
        }
        updateCenterSelection();
    });

    // подключения навигации центральной панели
    connect(m_centerView, &MillerListView::enterDirectoryRequested, this, [this](const std::filesystem::path& p) {
        setDirectory(p);
    });
    connect(m_centerView, &MillerListView::openFileRequested, this, [this](const std::filesystem::path& p) {
        openFile(p);
    });
    connect(m_centerView, &MillerListView::navigateToParentRequested, this, [this]() {
        if (m_currentDir.has_parent_path() && m_currentDir != m_currentDir.parent_path()) {
            const auto exitedDir = m_currentDir;
            const auto parentDir = exitedDir.parent_path();
            // Сначала переключаем каталог, потому что setDirectory() сбрасывает
            // отложенный выбор. Потом просим выделить папку, из которой поднялись,
            // чтобы Right вернул обратно.
            setDirectory(parentDir);
            selectPathAfterRefresh(exitedDir);
        }
    });
    connect(m_centerView, &MillerListView::navigateHistoryBackRequested, this, &MillerViewContainer::navigateHistoryBackRequested);
    connect(m_centerView, &MillerListView::navigateHistoryForwardRequested, this, &MillerViewContainer::navigateHistoryForwardRequested);

    // подключения навигации родительской панели (двойной клик по файлу или папке)
    connect(m_parentView, &MillerListView::enterDirectoryRequested, this, [this](const std::filesystem::path& p) {
        setDirectory(p);
    });
    connect(m_parentView, &MillerListView::openFileRequested, this, [this](const std::filesystem::path& p) {
        openFile(p);
    });
    connect(m_parentView, &MillerListView::navigateHistoryBackRequested, this, &MillerViewContainer::navigateHistoryBackRequested);
    connect(m_parentView, &MillerListView::navigateHistoryForwardRequested, this, &MillerViewContainer::navigateHistoryForwardRequested);

    connect(m_centerView, &MillerListView::typeAheadSearchTriggered, this, &MillerViewContainer::typeAheadSearchTriggered);
    connect(m_centerView, &MillerListView::openInNewTabRequested, this, &MillerViewContainer::openInNewTabRequested);
    connect(m_parentView, &MillerListView::openInNewTabRequested, this, &MillerViewContainer::openInNewTabRequested);
    connect(m_centerView, &MillerListView::filterTriggered, this, &MillerViewContainer::filterTriggered);
    connect(m_centerView, &MillerListView::trashRequested, this, &MillerViewContainer::trashSelection);
    connect(m_centerView, &MillerListView::deletePermanentlyRequested, this, &MillerViewContainer::deletePermanentlySelection);

    connect(m_centerView, &MillerListView::toggleSelectionRequested, this, [this](int row) {
        m_centerModel->toggleSelection(row);
        // держим selectionModel Qt в синхроне с явным мультивыбором по Space,
        // чтобы Shift+Click/Shift+стрелки расширяли его, а не стирали
        QModelIndex toggledIdx = m_centerModel->index(row, 0);
        if (toggledIdx.isValid() && m_centerView->selectionModel()) {
            bool nowSelected = m_centerModel->data(toggledIdx, Models::FileSystemModel::IsSelectedRole).toBool();
            m_centerView->selectionModel()->select(toggledIdx,
                nowSelected ? QItemSelectionModel::Select : QItemSelectionModel::Deselect);
        }
        updateCenterSelection();
    });

    // подключение навигации по папкам из панели превью
    connect(m_previewPane, &PreviewPane::enterDirectoryRequested, this, [this](const std::filesystem::path& p) {
        setDirectory(p);
    });
    // архив из превью открывается в центре, как папка, а не вторым окном
    connect(m_previewPane, &PreviewPane::archiveOpenRequested, this, [this](const std::filesystem::path& p) {
        enterArchive(p);
    });

    // свойства из превью тем же внутренним окном в центре (не в превью)
    connect(m_previewPane, &PreviewPane::propertiesRequested, this, &MillerViewContainer::showPropertiesFor);
    connect(m_previewPane, &PreviewPane::openWithDialogRequested, this, &MillerViewContainer::openWithDialogRequested);
    // Extract In… из меню превью: каталог выбирает MainWindow тем же оверлеем, что и для колонок
    connect(m_previewPane, &PreviewPane::extractInRequested, this, &MillerViewContainer::extractInPathRequested);
    // Plugins из меню превью: общий путь исполнения с колонками
    connect(m_previewPane, &PreviewPane::pluginRunRequested, this,
            [this](const QString& id, const std::filesystem::path& target, const std::filesystem::path& cwd) {
        runPluginByIdIn(id, {target}, cwd.empty() ? target.parent_path() : cwd,
                        QString(), false, true);
    });
    connect(m_previewPane, &PreviewPane::archiveSelectionInteractionStarted,
            this, &MillerViewContainer::activateArchiveSelection);
    connect(m_previewPane, &PreviewPane::messageRequested,
            this, &MillerViewContainer::messageEmitted);
    connect(m_centerView, &MillerListView::selectionInteractionStarted,
            this, &MillerViewContainer::activateFileSelection);
    connect(m_parentView, &MillerListView::selectionInteractionStarted,
            this, &MillerViewContainer::activateFileSelection);

    // двойной клик по центральной панели
    connect(m_centerView, &QListView::doubleClicked, this, &MillerViewContainer::onCenterDoubleClicked);

    // подключения DnD
    connect(m_centerView, &MillerListView::filesDropped, this, &MillerViewContainer::onFilesDropped);
    connect(m_parentView, &MillerListView::filesDropped, this, &MillerViewContainer::onFilesDropped);

    // триггеры контекстного меню (асинхронный попап для мгновенного отклика на первый клик)
    connect(m_centerView, &MillerListView::contextMenuRequested, this, [this](const QPoint& globalPos, const QModelIndex& idx) {
        if (m_activeContextMenu) {
            m_activeContextMenu->close();
            m_activeContextMenu->deleteLater();
            m_activeContextMenu.clear();
        }

        std::vector<std::filesystem::path> sel;
        if (idx.isValid()) {
            sel = selectedPaths();
        } else {
            m_centerModel->clearSelection();
            m_centerView->clearSelection();
            m_centerView->setCurrentIndex(QModelIndex());
        }
        auto menu = new TuiContextMenu(sel, m_currentDir, this);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        m_activeContextMenu = menu;

        connect(menu, &TuiContextMenu::openRequested, this, &MillerViewContainer::openCurrent);
        connect(menu, &TuiContextMenu::openInNewTabRequested, this, &MillerViewContainer::openSelectionInNewTab);
        connect(menu, &TuiContextMenu::openWithAppRequested, this, &MillerViewContainer::openWithApp);
        connect(menu, &TuiContextMenu::openWithRequested, this, [this, sel]() {
            if (sel.empty()) return;
            if (tryPluginOverrideWith("open_with", sel, m_currentDir)) return;
            emit openWithDialogRequested(sel.front());
        });
        connect(menu, &TuiContextMenu::clearTrashRequested, this, &MillerViewContainer::clearTrashBin);
        connect(menu, &TuiContextMenu::cutRequested, this, &MillerViewContainer::cutSelection);
        connect(menu, &TuiContextMenu::copyRequested, this, &MillerViewContainer::copySelection);
        connect(menu, &TuiContextMenu::pasteRequested, this, &MillerViewContainer::pasteClipboard);
        connect(menu, &TuiContextMenu::copyPathRequested, this, &MillerViewContainer::copyPaths);
        connect(menu, &TuiContextMenu::renameRequested, this, &MillerViewContainer::renameCurrent);
        connect(menu, &TuiContextMenu::trashRequested, this, &MillerViewContainer::trashSelection);
        connect(menu, &TuiContextMenu::deletePermanentlyRequested, this, &MillerViewContainer::deletePermanentlySelection);
        connect(menu, &TuiContextMenu::compressRequested, this, &MillerViewContainer::compressSelection);
        connect(menu, &TuiContextMenu::extractHereRequested, this, &MillerViewContainer::extractHere);
        connect(menu, &TuiContextMenu::extractInRequested, this, [this, sel]() {
            if (tryPluginOverrideWith("extract_in", sel, m_currentDir)) return;
            // меню в hideEvent возвращает фокус колонкам; открываем оверлей следующим
            // циклом, иначе он сразу теряет фокус и Esc не доходит до диалога
            QTimer::singleShot(0, this, [this]() { emit extractInRequested(); });
        });
        connect(menu, &TuiContextMenu::openTerminalRequested, this, &MillerViewContainer::openTerminal);
        connect(menu, &TuiContextMenu::newFolderRequested, this, &MillerViewContainer::createNewFolder);
        connect(menu, &TuiContextMenu::newFileRequested, this, &MillerViewContainer::createNewFile);
        connect(menu, &TuiContextMenu::bookmarkRequested, this, &MillerViewContainer::bookmarkSelection);
        connect(menu, &TuiContextMenu::propertiesRequested, this, &MillerViewContainer::showProperties);
        connect(menu, &TuiContextMenu::sortModeRequested, this, &MillerViewContainer::setSortMode);
        connect(menu, &TuiContextMenu::pluginRequested, this, &MillerViewContainer::runPluginById);
        connect(menu, &TuiContextMenu::nativeActionRequested, this,
                &MillerViewContainer::runNativePluginAction);

        menu->popup(globalPos);
    });

    // контекстное меню родительской панели
    connect(m_parentView, &MillerListView::contextMenuRequested, this, [this](const QPoint& globalPos, const QModelIndex& idx) {
        if (m_activeContextMenu) {
            m_activeContextMenu->close();
            m_activeContextMenu->deleteLater();
            m_activeContextMenu.clear();
        }

        std::vector<std::filesystem::path> sel;
        if (idx.isValid()) {
            QString pathStr = idx.data(Models::FileSystemModel::PathRole).toString();
            if (!pathStr.isEmpty()) {
                sel.push_back(std::filesystem::path(pathStr.toUtf8().toStdString()));
            }
            m_parentDelegate->setContextMenuRow(idx.row());
            m_parentView->viewport()->update();
        }
        auto parentDir = m_currentDir.has_parent_path() ? m_currentDir.parent_path() : m_currentDir;
        auto menu = new TuiContextMenu(sel, parentDir, this);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        m_activeContextMenu = menu;

        connect(menu, &QMenu::aboutToHide, this, [this]() {
            m_parentDelegate->setContextMenuRow(-1);
            m_parentView->viewport()->update();
        });

        connect(menu, &TuiContextMenu::openRequested, this, [this, sel]() {
            if (!sel.empty()) {
                std::error_code ec;
                if (std::filesystem::is_directory(sel.front(), ec)) {
                    setDirectory(sel.front());
                } else {
                    openFile(sel.front());
                }
            }
        });
        connect(menu, &TuiContextMenu::openInNewTabRequested, this, [this, sel, parentDir]() {
            if (!sel.empty()) {
                std::error_code ec;
                if (std::filesystem::is_directory(sel.front(), ec)) {
                    emit openInNewTabRequested(sel.front());
                    return;
                }
            }
            emit openInNewTabRequested(parentDir);
        });
        connect(menu, &TuiContextMenu::openWithAppRequested, this, [sel](const QString& execCmd) {
            if (!sel.empty()) {
                Core::DesktopApp app;
                app.execCommand = execCmd;
                Core::XdgUtils::openWithApp(app, sel.front());
            }
        });
        connect(menu, &TuiContextMenu::openWithRequested, this, [this, sel, parentDir]() {
            if (!sel.empty()) {
                if (tryPluginOverrideWith("open_with", sel, parentDir)) return;
                emit openWithDialogRequested(sel.front());
            }
        });
        connect(menu, &TuiContextMenu::copyPathRequested, this, [this, sel, parentDir](int mode) {
            const QString id = (mode == 0) ? QStringLiteral("copy_path")
                              : (mode == 1) ? QStringLiteral("copy_name")
                                            : QStringLiteral("copy_rel_path");
            if (tryPluginOverrideWith(id, sel, parentDir)) return;
            std::filesystem::path target = sel.empty() ? parentDir : sel.front();
            if (mode == 0) {
                QApplication::clipboard()->setText(QString::fromUtf8(target.c_str()));
            } else if (mode == 1) {
                QApplication::clipboard()->setText(QString::fromUtf8(target.filename().c_str()));
            } else {
                std::error_code ec;
                auto rel = std::filesystem::relative(target, parentDir, ec);
                QApplication::clipboard()->setText(QString::fromUtf8(rel.c_str()));
            }
        });
        connect(menu, &TuiContextMenu::bookmarkRequested, this, [this, sel, parentDir]() {
            if (tryPluginOverrideWith("bookmark", sel, parentDir)) return;
            emit bookmarkAdded(sel.empty() ? parentDir : sel.front());
            emit messageEmitted("Added to bookmarks");
        });
        connect(menu, &TuiContextMenu::openTerminalRequested, this, [this, sel, parentDir]() {
            if (tryPluginOverrideWith("open_terminal", sel, parentDir)) return;
            Core::XdgUtils::openTerminalAt(parentDir);
        });
        connect(menu, &TuiContextMenu::propertiesRequested, this, [this, sel, parentDir]() {
            if (tryPluginOverrideWith("properties", sel, parentDir)) return;
            showPropertiesFor(sel.empty() ? parentDir : sel.front());
        });
        connect(menu, &TuiContextMenu::pluginRequested, this, [this, sel, parentDir](const QString& id) {
            runPluginByIdIn(id, sel, parentDir, QString(), false, true);
        });
        connect(menu, &TuiContextMenu::nativeActionRequested, this,
                [this, sel, parentDir](const QString& actionId) {
            NativePluginHost::instance().invokeAction(actionId, sel, parentDir, QString(), true);
        });
        menu->popup(globalPos);
    });

    connect(m_centerModel, &Models::FileSystemModel::globalSearchFinished, this, [this](int count) {
        if (count > 0 && m_centerView->model()) {
            m_centerView->setCurrentIndex(m_centerModel->index(0, 0));
        }
    });

    // уведомления о загрузке модели
    connect(m_centerModel, &Models::FileSystemModel::loadingFinished, this, [this](int count) {
        updateCenterDeniedOverlay(count);
        if (!m_pathToSelectAfterRefresh.empty()) {
            if (selectAndFocusPath(m_pathToSelectAfterRefresh)) {
                m_pathToSelectAfterRefresh.clear();
                m_pendingSelectRetries = 0;
            } else if (m_pendingSelectRetries > 0) {
                // путь ещё не появился (переименование, фильтр): повторяем ограниченное
                // число раз, refresh снова придёт в loadingFinished
                --m_pendingSelectRetries;
                m_centerModel->refresh();
            } else {
                m_pathToSelectAfterRefresh.clear();
            }
            return;
        }

        if (count > 0) {
            // сохраняем фокус при фоновых обновлениях: восстанавливаем выделение Qt
            // для явно выбранных строк, иначе держим текущую строку вместо прыжка на строку 0
            auto selPaths = m_centerModel->selectedPaths();
            if (selPaths.size() > 1 && m_centerView->selectionModel()) {
                // повторно применяем визуальное выделение Qt для мультивыбора, стёртого resetModel
                QItemSelection qs;
                int firstValidRow = -1;
                for (const auto& sp : selPaths) {
                    int r = m_centerModel->findRowByPath(sp);
                    if (r >= 0) {
                        if (firstValidRow < 0) firstValidRow = r;
                        qs.select(m_centerModel->index(r, 0), m_centerModel->index(r, 0));
                    }
                }
                if (!qs.isEmpty()) {
                    if (!m_centerView->currentIndex().isValid() && firstValidRow >= 0) {
                        m_centerView->selectionModel()->setCurrentIndex(
                            m_centerModel->index(firstValidRow, 0), QItemSelectionModel::NoUpdate);
                    }
                    m_centerView->selectionModel()->select(qs, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
                }
            } else if (!m_centerView->currentIndex().isValid()) {
                // Current сброшен при reset модели: восстанавливаем прежнюю строку
                // с ограничением по новому количеству
                int restoreRow = std::clamp(m_lastKnownRow, 0, count - 1);
                QModelIndex restoreIdx = m_centerModel->index(restoreRow, 0);
                m_centerView->setCurrentIndex(restoreIdx);
                if (m_centerView->selectionModel()) {
                    m_centerView->selectionModel()->select(restoreIdx, QItemSelectionModel::ClearAndSelect);
                }
            }
        }
        updateCenterSelection();
    });

    // родительская модель загружена, обновляем подсветку
    connect(m_parentModel, &Models::FileSystemModel::directoryLoaded, this, [this](const std::filesystem::path& /*p*/) {
        syncParentViewToCurrentDir();
    });
}

void MillerViewContainer::syncParentViewToCurrentDir() {
    if (!m_parentModel || !m_parentView || !m_parentDelegate) return;
    m_parentDelegate->setHighlightedPath(m_currentDir);
    if (auto* sm = m_parentView->selectionModel()) {
        // setCurrentIndex() делает ClearAndSelect|Rows и зажигает яркое выделение; при смене
        // папки с тем же родителем модель не перезагружается, и выделение застревало бы,
        // поэтому двигаем current index точечно
        sm->clearSelection();
        const int row = m_parentModel->findRowByPath(m_currentDir);
        sm->setCurrentIndex(row >= 0 ? m_parentModel->index(row, 0) : QModelIndex(),
                            QItemSelectionModel::NoUpdate);
    }
    // каталог грузится асинхронно: модель уже отрисовалась со старым highlight; без update()
    // подсветка «открытой папки» появлялась бы лишь при наведении мыши
    m_parentView->viewport()->update();
}

void MillerViewContainer::setCurrentSudo(bool sudo) {
    if (m_isCurrentSudo != sudo) {
        m_isCurrentSudo = sudo;
        emit sudoModeChanged(m_isCurrentSudo);
    }
}

void MillerViewContainer::restoreMultiSelection(const std::unordered_set<std::string>& paths, const std::filesystem::path& focusItem) {
    if (!m_centerModel) return;
    m_centerModel->setSelectedPaths(paths);
    if (!paths.empty()) {
        QItemSelection qs;
        int focusRow = -1;
        for (const auto& spStr : paths) {
            std::filesystem::path sp(spStr);
            int r = m_centerModel->findRowByPath(sp);
            if (r >= 0) {
                if (focusRow < 0 && sp == focusItem) focusRow = r;
                qs.select(m_centerModel->index(r, 0), m_centerModel->index(r, 0));
            }
        }
        if (m_centerView->selectionModel()) {
            if (focusRow >= 0) {
                m_centerView->setCurrentIndex(m_centerModel->index(focusRow, 0));
            } else if (!paths.empty()) {
                int r = m_centerModel->findRowByPath(std::filesystem::path(*paths.begin()));
                if (r >= 0) m_centerView->setCurrentIndex(m_centerModel->index(r, 0));
            }
            m_centerView->selectionModel()->select(qs, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        }
        updateCenterSelection();
    } else if (!focusItem.empty()) {
        int r = m_centerModel->findRowByPath(focusItem);
        if (r >= 0) {
            QModelIndex idx = m_centerModel->index(r, 0);
            m_centerView->setCurrentIndex(idx);
            if (m_centerView->selectionModel()) {
                m_centerView->selectionModel()->select(idx, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            }
            updateCenterSelection();
        }
    }
}

namespace {
// сортировка для каждой папки: каталог → «режим|порядок» (0=Name, 1=Size, 2=Date,
// 3=Type; 1=по возрастанию, 0=по убыванию). Плоский ~/.config/atlifm/sorting.ini
// (QSettings, ключ = канонический путь в процентном кодировании: слэши в ключах
// QSettings разделители групп, а путь начинается со слэша). При ATLIFM_CONFIG_DIR
// файл <каталог>/sorting.ini рядом с keybinds/plugins/theme.conf (изоляция тестов)
QSettings makeSortSettings() {
    const QString override = qEnvironmentVariable("ATLIFM_CONFIG_DIR");
    if (!override.isEmpty()) {
        return QSettings(override + QStringLiteral("/sorting.ini"), QSettings::IniFormat);
    }
    return QSettings(QSettings::IniFormat, QSettings::UserScope,
                     QStringLiteral("atlifm"), QStringLiteral("sorting"));
}
QString sortSettingsKey(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::path norm = std::filesystem::weakly_canonical(dir, ec);
    if (ec) norm = dir;
    return QString::fromLatin1(QUrl::toPercentEncoding(QString::fromUtf8(norm.c_str())));
}

// текст индикатора сортировки для строки состояния; иконка инверта, если порядок
// инвертирован относительно дефолтного для режима
QString sortedByMessage(const Models::FileSystemModel* model) {
    if (!model) return QStringLiteral("Sorted by");
    const int mode = model->sortMode();
    const QString name = (mode == 1) ? QStringLiteral("Size")
                                     : (mode == 2) ? QStringLiteral("Date")
                                     : (mode == 3) ? QStringLiteral("Type")
                                                   : QStringLiteral("Name");
    const bool inverted =
        model->isSortAscending() != Models::FileSystemModel::defaultSortAscending(mode);
    return inverted ? QStringLiteral("Sorted by %1 \u21C5").arg(name)
                    : QStringLiteral("Sorted by %1").arg(name);
}

QString readSavedSort(const std::filesystem::path& dir) {
    QSettings s = makeSortSettings();
    return s.value(sortSettingsKey(dir)).toString();
}

void writeSavedSort(const std::filesystem::path& dir, int mode, bool asc) {
    QSettings s = makeSortSettings();
    s.setValue(sortSettingsKey(dir), QStringLiteral("%1|%2").arg(mode).arg(asc ? 1 : 0));
}
}

void MillerViewContainer::applySortToModel(const std::filesystem::path& dir,
                                           Models::FileSystemModel* model) {
    if (dir.empty() || !model) return;
    const QString v = readSavedSort(dir);
    if (v.isEmpty()) return; // нет записи оставляем текущий режим
    const QStringList parts = v.split('|');
    bool okMode = false;
    bool okAsc = false;
    const int mode = parts.value(0).toInt(&okMode);
    const int asc = parts.value(1).toInt(&okAsc);
    if (!okMode || !okAsc || mode < 0 || mode > 3) return;
    model->setSortMode(mode, asc != 0);
}

void MillerViewContainer::applySortForDirectory(const std::filesystem::path& dir) {
    applySortToModel(dir, m_centerModel);
}

void MillerViewContainer::saveSortForCurrentDir() {
    if (m_currentDir.empty() || !m_centerModel) return;
    writeSavedSort(m_currentDir, m_centerModel->sortMode(), m_centerModel->isSortAscending());
}

void MillerViewContainer::setDirectory(const std::filesystem::path& path) {
    if (path.empty()) return;
    if (isArchiveViewActive()) leaveArchive();
    m_archiveSelectionActive = false;
    if (m_previewPane) m_previewPane->clearArchiveSelection();

    std::error_code ec;
    m_currentDir = std::filesystem::canonical(std::filesystem::absolute(path), ec);
    if (ec) {
        m_currentDir = path;
    }

    // ожидаемый выбор от прошлой операции больше не актуален в новом каталоге
    m_pathToSelectAfterRefresh.clear();
    m_pendingSelectRetries = 0;

    // индикатор sudo только внутри каталога, открытого через "Sudo Open" (m_sudoRoot),
    // либо там, где чтение реально требует root (restricted)
    const bool insideSudoRoot = !m_sudoRoot.empty() && isInsideDir(m_currentDir, m_sudoRoot);
    const bool isRestricted = (::access(m_currentDir.c_str(), R_OK | X_OK) != 0);
    setCurrentSudo(insideSudoRoot || isRestricted);

    NativePluginHost::fireHook(QStringLiteral("on_navigate"), {}, QString::fromUtf8(m_currentDir.c_str()));
    // левая колонка своя папка (родительская) со своей сохранённой сортировкой;
    // сортировка текущей папки её не перестраивает
    const bool hasParent =
        m_currentDir.has_parent_path() && m_currentDir != m_currentDir.parent_path();
    if (hasParent) {
        m_parentModel->setDirectory(m_currentDir.parent_path());
        applySortToModel(m_currentDir.parent_path(), m_parentModel);
    } else {
        // корень: обе колонки показывают один каталог, режим общий (ниже)
        m_parentModel->setDirectory(m_currentDir);
    }
    syncParentViewToCurrentDir();

    // сбрасываем якоря выделения при переходе в новый каталог
    m_centerView->clearAnchor();
    m_parentView->clearAnchor();

    // обновляем активную панель
    m_centerModel->setDirectory(m_currentDir);
    // сортировка для каждой папки: у папки свой сохранённый режим/порядок
    applySortForDirectory(m_currentDir);
    if (!hasParent && m_parentModel) {
        m_parentModel->setSortMode(m_centerModel->sortMode(), m_centerModel->isSortAscending());
    }
    m_centerView->setFocus();

    // пока грузится новый каталог, старая подсказка «permission denied» не должна
    // висеть; loadingFinished решит заново (updateCenterDeniedOverlay)
    if (m_centerDeniedOverlay) m_centerDeniedOverlay->hide();

    emit directoryChanged(m_currentDir);
}

void MillerViewContainer::setFilter(const QString& filter) {
    // фильтр транзиентный: живёт, пока открыт редактор; Enter/Esc снимают его; хранения по папкам нет
    m_centerModel->setFilter(filter);
    if (m_centerModel->rowCount() > 0) {
        m_centerView->setCurrentIndex(m_centerModel->index(0, 0));
    }
}

void MillerViewContainer::searchEverywhere(const QString& query) {
    m_centerModel->searchEverywhere(query);
}

void MillerViewContainer::searchContents(const QString& query) {
    m_centerModel->searchContents(query);
}

void MillerViewContainer::clearGlobalSearch() {
    m_centerModel->clearGlobalSearch();
}

bool MillerViewContainer::hasGlobalSearch() const {
    return m_centerModel && m_centerModel->globalSearchActive();
}

void MillerViewContainer::focusCenter() {
    m_centerView->setFocus();
}

std::vector<std::filesystem::path> MillerViewContainer::selectedPaths() const {
    // страница архива перекрывает колонки: скрытый выбор файлов не должен
    // участвовать в delete/rename/cut и других файловых операциях
    if (isArchiveViewActive()) return {};

    auto multiSel = m_centerModel->selectedPaths();
    if (!multiSel.empty()) return multiSel;

    if (m_centerView->selectionModel()) {
        auto selIndexes = m_centerView->selectionModel()->selectedIndexes();
        if (!selIndexes.isEmpty()) {
            std::vector<std::filesystem::path> res;
            for (const auto& idx : selIndexes) {
                QString pathStr = idx.data(Models::FileSystemModel::PathRole).toString();
                res.push_back(std::filesystem::path(pathStr.toUtf8().toStdString()));
            }
            return res;
        }
    }

    std::vector<std::filesystem::path> res;
    QModelIndex curr = m_centerView->currentIndex();
    if (curr.isValid()) {
        QString pathStr = curr.data(Models::FileSystemModel::PathRole).toString();
        res.push_back(std::filesystem::path(pathStr.toUtf8().toStdString()));
    }
    return res;
}

std::vector<std::filesystem::path> MillerViewContainer::currentEntries() const {
    std::vector<std::filesystem::path> entries;
    if (!m_centerModel) return entries;
    const int rows = m_centerModel->rowCount();
    entries.reserve(static_cast<size_t>(rows));
    for (int row = 0; row < rows; ++row) {
        const QString path = m_centerModel->index(row, 0)
                                 .data(Models::FileSystemModel::PathRole).toString();
        if (!path.isEmpty()) entries.emplace_back(path.toUtf8().toStdString());
    }
    if (!entries.empty()) return entries;

    // Палитра может открыться до асинхронной загрузки модели или в момент,
    // когда m_currentDir ещё не выставлен. Читаем каталог напрямую, иначе
    // поиск показывает "No entries" в непустой папке.
    std::filesystem::path scanDir = m_currentDir;
    if (scanDir.empty()) scanDir = m_centerModel->currentDirectory();
    if (scanDir.empty()) return entries;

    const bool showHidden = m_centerModel->showHidden();
    std::error_code ec;
    std::filesystem::directory_iterator it(scanDir, ec);
    const std::filesystem::directory_iterator end;
    if (ec) {
        qWarning("[palette] cannot read current folder %s: %s",
                 scanDir.c_str(), ec.message().c_str());
        return entries;
    }
    for (; it != end && entries.size() < 10000; it.increment(ec)) {
        if (ec) break;
        const auto& path = it->path();
        if (!showHidden && !path.filename().empty() &&
            path.filename().string().front() == '.') {
            continue;
        }
        entries.push_back(path);
    }
    return entries;
}

void MillerViewContainer::toggleHidden() {
    bool current = m_centerModel->showHidden();
    m_centerModel->setShowHidden(!current);
    m_parentModel->setShowHidden(!current);
    emit messageEmitted(!current ? "Show hidden files" : "Hide hidden files");
}

void MillerViewContainer::setSortMode(int mode) {
    // перехват плагином: sort_name/sort_size/sort_date/sort_type (Alt+1..4, ПКМ Sort By)
    static const char* const sortIds[] = {"sort_name", "sort_size", "sort_date", "sort_type"};
    if (mode >= 0 && mode <= 3 &&
        tryPluginOverride(QLatin1String(sortIds[mode])))
        return;
    // только центральная колонка: родительская показывает другую папку со своим режимом
    if (m_centerModel) m_centerModel->setSortMode(mode);
    saveSortForCurrentDir();
    emit messageEmitted(sortedByMessage(m_centerModel));
}

void MillerViewContainer::toggleSortDirection() {
    if (m_centerModel) m_centerModel->toggleSortDirection();
    saveSortForCurrentDir();
    emit messageEmitted(sortedByMessage(m_centerModel));
}

// ПКМ по иконке сортировки: дефолт = Name ascending; запись папки в sorting.ini
// перезаписывается, т.е. папка забывает свой режим
void MillerViewContainer::resetSortToDefault() {
    if (m_centerModel) m_centerModel->setSortMode(0, true);
    saveSortForCurrentDir();
    emit messageEmitted(sortedByMessage(m_centerModel));
}

namespace {
// меню сортировки в стиле TUI: строка «· Name» и отдельная кнопка инверсии «⇅» у каждого
// пункта; прямой порядок подсвечен зелёным (Accent/AccentFg), инверсный синим (Sapphire/Base)
class SortMenuPart : public QLabel {
public:
    SortMenuPart(const QString& text, QWidget* parent) : QLabel(text, parent) {
        setFont(Theme::getMonospaceFont(9));
        setCursor(Qt::PointingHandCursor);
        applyStyle();
    }
    std::function<void()> onClicked;

    void setActive(bool on) { m_active = on; applyStyle(); }
    void setActiveColors(const QColor& bg, const QColor& fg) {
        m_activeBg = bg;
        m_activeFg = fg;
        applyStyle();
    }
    void setIdleColor(const QColor& c) { m_idleFg = c; applyStyle(); }
    // размер шрифта в персональной таблице стилей приложения
    // перебивает QWidget::setFont
    void setFontPx(int px) { m_fontPx = px; applyStyle(); }

protected:
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton) { m_pressed = true; applyStyle(); }
        QLabel::mousePressEvent(e);
        e->accept(); // клик обработан здесь и не должен уходить в QMenu
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        m_pressed = false;
        applyStyle();
        const bool inside = rect().contains(e->position().toPoint());
        if (e->button() == Qt::LeftButton && inside && onClicked) onClicked();
        QLabel::mouseReleaseEvent(e);
        e->accept(); // иначе QMenu считает отпускание своей командой и закрывается
    }
    void enterEvent(QEnterEvent* e) override { m_hover = true; applyStyle(); QLabel::enterEvent(e); }
    void leaveEvent(QEvent* e) override { m_hover = false; applyStyle(); QLabel::leaveEvent(e); }

private:
    void applyStyle() {
        using namespace Theme::Colors;
        QString bg = QStringLiteral("transparent");
        QString fg = m_idleFg.name();
        if (m_active) { bg = m_activeBg.name(); fg = m_activeFg.name(); }
        else if (m_hover || m_pressed) { bg = Surface1().name(); fg = m_activeBg.name(); }
        setStyleSheet(QString("QLabel { background: %1; color: %2; padding: 4px 12px; font-size: %3px; }")
                          .arg(bg, fg).arg(m_fontPx));
    }
    bool m_active{false};
    bool m_hover{false};
    bool m_pressed{false};
    int m_fontPx{13};
    QColor m_activeBg{Theme::Colors::SelectionBg()};
    QColor m_activeFg{Theme::Colors::SelectionFg()};
    QColor m_idleFg{Theme::Colors::Text()};
};

// меню сортировки не закрывается сразу после клика: после первого взаимодействия оно
// закрывается только когда курсор ушёл за пределы меню, либо по Esc или клику вовне
class SortMenu : public QMenu {
public:
    explicit SortMenu(QWidget* parent = nullptr) : QMenu(parent) {}
    void markInteracted() { m_interacted = true; }

protected:
    void leaveEvent(QEvent* event) override {
        if (m_interacted && !rect().contains(mapFromGlobal(QCursor::pos()))) close();
        QMenu::leaveEvent(event);
    }

private:
    bool m_interacted{false};
};
}

void MillerViewContainer::showSortMenu() {
    if (!m_sortButton || !m_centerModel) return;

    SortMenu menu(this);
    menu.setFont(Theme::getMonospaceFont(9));
    // ноль вертикальных полей QMenu: строки прижаты к рамке; QSS виджета перекрывает
    // общий QMenu { padding: 3px 0px } из application stylesheet
    menu.setStyleSheet(Theme::getApplicationStyleSheet() + QStringLiteral("QMenu { padding: 0px; }"));

    struct Parts { SortMenuPart* label; SortMenuPart* inv; int mode; };
    std::vector<Parts> parts;
    auto refreshStates = [this, &parts]() {
        const int mode = m_centerModel->sortMode();
        const bool asc = m_centerModel->isSortAscending();
        for (const auto& p : parts) {
        // активна строка режима в его дефолтном направлении (Name ↑, Size ↓, Date ↓), кнопка ⇅ в инвертированном
            const bool isDefaultDir = (asc == Models::FileSystemModel::defaultSortAscending(p.mode));
            p.label->setActive(p.mode == mode && isDefaultDir);
            p.inv->setActive(p.mode == mode && !isDefaultDir);
        }
    };

    auto addRow = [this, &menu, &parts, &refreshStates](const QString& text, int mode) {
        auto* act = new QWidgetAction(&menu);
        auto* row = new QWidget(&menu);
        auto* lay = new QHBoxLayout(row);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(0);

        auto* labelPart = new SortMenuPart(text, row);
        labelPart->setObjectName(QStringLiteral("sortPartLabel"));
        labelPart->setFontPx(Theme::uiConfig().fontSize);
        labelPart->setActiveColors(Theme::Colors::SelectionBg(), Theme::Colors::SelectionFg());
        auto* invPart = new SortMenuPart(QStringLiteral("\u21C5"), row);
        invPart->setObjectName(QStringLiteral("sortPartInv"));
        // кнопка инверсии крупнее текста: размер в персональном QSS, иначе общий QSS приложения его съедает
        invPart->setFontPx(Theme::uiConfig().fontSize + 4);
        invPart->setActiveColors(Theme::Colors::Sapphire(), Theme::Colors::Base());
        invPart->setIdleColor(Theme::Colors::Subtext());
        invPart->setToolTip(QStringLiteral("Invert direction"));

        parts.push_back({labelPart, invPart, mode});

        // режим в его дефолтном направлении (Name ↑, Size ↓, Date ↓), кнопка ⇅ инвертированное
        // направление; повторный клик по активному состоянию ничего не делает; меню не закрываем,
        // SortMenu закроется при клике и уводе курсора или по Esc
        labelPart->onClicked = [this, &menu, mode, &refreshStates]() {
            const bool defaultAsc = Models::FileSystemModel::defaultSortAscending(mode);
            if (m_centerModel->sortMode() != mode) {
                setSortMode(mode); // переключение режима = его дефолт
            } else if (m_centerModel->isSortAscending() != defaultAsc) {
                toggleSortDirection();
            }
            refreshStates();
            static_cast<SortMenu*>(&menu)->markInteracted();
        };
        invPart->onClicked = [this, &menu, mode, &refreshStates]() {
            const bool defaultAsc = Models::FileSystemModel::defaultSortAscending(mode);
            if (m_centerModel->sortMode() != mode) {
                setSortMode(mode); // → дефолт, ниже инвертируем
            }
            if (m_centerModel->isSortAscending() == defaultAsc) {
                toggleSortDirection();
            }
            refreshStates();
            static_cast<SortMenu*>(&menu)->markInteracted();
        };

        lay->addWidget(labelPart, 1);
        lay->addWidget(invPart, 0);
        act->setDefaultWidget(row);
        menu.addAction(act);
    };

    addRow(QStringLiteral("Name"), 0);
    addRow(QStringLiteral("Size"), 1);
    addRow(QStringLiteral("Date"), 2);
    addRow(QStringLiteral("Type"), 3);
    refreshStates();

    menu.exec(m_sortButton->mapToGlobal(QPoint(0, m_sortButton->height())));
}

void MillerViewContainer::sudoOpenCurrent(const std::filesystem::path& targetDir) {
    std::filesystem::path target = targetDir;
    if (target.empty()) {
        auto sel = selectedPaths();
        target = sel.empty() ? m_currentDir : sel.front();
    }
    // классификация target без привилегированной пробы: оверлей передаёт текущий
    // каталог (по определению каталог), меню элемент с типом, известным модели (IsDirRole)
    std::filesystem::path dir = target;
    std::error_code dirEc;
    if (!std::filesystem::is_directory(target, dirEc)
        && !(dirEc && dirEc == std::errc::permission_denied)) {
        dir = target.parent_path(); // обычный файл открываем его каталог
    }
    if (dir.empty()) dir = std::filesystem::path("/");

    Core::XdgUtils::sudoDebugLog(QString("sudoOpen: target=[%1] dir=[%2]")
                                     .arg(QString::fromUtf8(target.c_str()),
                                          QString::fromUtf8(dir.c_str())));

    const bool readable = (::access(dir.c_str(), R_OK | X_OK) == 0);

    // одна авторизация на действие где живой тикет молча запускает sudo find
    // иначе ровно один вызов pkexec и его вывод наполняет колонки
    // через provideSudoListingTree а вторых привилегированных вызовов нет
    // без polkit резерв: попап пароля и запуск sudo find
    bool folderEmpty = false;
    bool sudoDenied = false;
    bool aborted = false;     // авторизация отклонена: каталог не открываем
    QString rootOut;          // вывод авторизованного find (пустой тоже ответ)
    bool haveRootOut = false;
    int probeExit = -1;
    int probeOutLen = -1;
    QString probeErr;

    if (readable) {
        std::error_code probeEc;
        auto probeIt = std::filesystem::directory_iterator(dir, probeEc);
        folderEmpty = (!probeEc && probeIt == std::filesystem::directory_iterator());
    } else {
        const bool ticketAlive = (::geteuid() == 0) || Core::XdgUtils::sudoTicketAlive();
        emit messageEmitted(ticketAlive ? QString("Sudo open…") : QString("Sudo auth…"));

        // рекурсивный листинг (maxdepth 5) одним вызовом: после одной авторизации
        // в кэше листинги всех подкаталогов, навигация на любую глубину без новых
        // диалогов pkexec. Разделитель NUL записей + полный путь %p
        // (см. provideSudoListingTree): имя с '\n' не инжектит записи, хвост,
        // обрезанный по лимиту head вывод отбрасывается
        QString treeOut;
        QString treeErr;
        const Core::XdgUtils::SudoListStatus status =
            Core::XdgUtils::sudoListTree(dir, 5, treeOut, treeErr, this);
        probeExit = status == Core::XdgUtils::SudoListStatus::Ok ? 0
            : (status == Core::XdgUtils::SudoListStatus::Dismissed ? 126 : -1);
        probeOutLen = treeOut.size();
        probeErr = treeErr.left(120);
        rootOut = treeOut;

        switch (status) {
        case Core::XdgUtils::SudoListStatus::Ok:
            folderEmpty = rootOut.isEmpty();
            haveRootOut = true;
            break;
        case Core::XdgUtils::SudoListStatus::Dismissed:
            // 126 диалог отклонён, 127 не авторизован или нет агента; отказ есть
            // отказ: второй запрос (попап пароля) не показываем
            aborted = true;
            break;
        case Core::XdgUtils::SudoListStatus::NotAuthorized:
            Core::XdgUtils::sudoDebugLog(QString("sudoOpen: auth failed err=[%1]").arg(treeErr));
            emit messageEmitted(QString("Sudo denied"));
            return;
        case Core::XdgUtils::SudoListStatus::Failed:
            sudoDenied = true; // система отказала листингом даже root
            break;
        }
    }

    if (aborted) {
        Core::XdgUtils::sudoDebugLog(QString("sudoOpen: pkexec dismissed exit=%1 err=[%2]")
                                         .arg(probeExit).arg(probeErr));
        emit messageEmitted(QString("Sudo denied"));
        return;
    }

    // листинг в модель ДО setDirectory/refresh: скан не должен сделать свой привилегированный
    // вызов (двойной диалог); рекурсивный вывод раскладывается по каталогам дерева,
    // вся глубина обслуживается кэшем
    if (haveRootOut) Core::XdgUtils::provideSudoListingTree(dir, rootOut);

    // запоминаем корень sudo: индикатор горит только внутри него (см. setDirectory)
    m_sudoRoot = dir;
    setDirectory(dir);
    refresh();

    // система отказала даже sudo (пустой листинг + ненулевой код): запоминаем каталог,
    // чтобы оверлей не подначивал повторные бесполезные попытки
    if (sudoDenied) {
        m_sudoDeniedDir = dir;
    } else if (m_sudoDeniedDir == dir) {
        m_sudoDeniedDir.clear();
    }

    Core::XdgUtils::sudoDebugLog(QString("sudoOpen: done dir=[%1] readable=%2 empty=%3 denied=%4 badge=%5 exit=%6 out=%7 err=[%8]")
                                     .arg(QString::fromUtf8(dir.c_str()))
                                     .arg(readable ? 1 : 0)
                                     .arg(folderEmpty ? 1 : 0)
                                     .arg(sudoDenied ? 1 : 0)
                                     .arg(m_isCurrentSudo ? 1 : 0)
                                     .arg(probeExit)
                                     .arg(probeOutLen)
                                     .arg(probeErr));

    // короткие плашки и подробности в логе отладки
    emit messageEmitted(sudoDenied ? QString("Sudo denied") : QString("Sudo open"));
}

void MillerViewContainer::clearTrashBin() {
    if (tryPluginOverride("clear_trash")) return;
    if (Core::XdgUtils::clearTrash()) {
        refresh();
        emit messageEmitted("Trash cleared");
    }
}

void MillerViewContainer::activateArchiveSelection() {
    m_archiveSelectionActive = true;
    if (auto* sm = m_centerView->selectionModel()) {
        QSignalBlocker blocker(sm);
        sm->clearSelection();
    }
    m_centerModel->clearSelection();
    m_centerDelegate->setCheckmarksEnabled(false);
    if (m_centerView->viewport()) m_centerView->viewport()->update();
    updateCenterSelection();
}

void MillerViewContainer::activateFileSelection() {
    m_archiveSelectionActive = false;
    if (m_previewPane) m_previewPane->clearArchiveSelection();
}

void MillerViewContainer::selectAll() {
    if (isArchiveViewActive()) {
        m_archiveTree->selectAllEntries();
        return;
    }
    if (m_archiveSelectionActive) {
        m_previewPane->selectAllArchiveEntries();
        return;
    }
    // selectAll триггерит selectionChanged и синхронизирует модель
    m_centerView->selectAll();
}

void MillerViewContainer::clearSelection() {
    if (isArchiveViewActive()) {
        m_archiveTree->clearEntrySelection();
        return;
    }
    if (m_archiveSelectionActive) {
        m_archiveSelectionActive = false;
        m_previewPane->clearArchiveSelection();
        m_centerView->clearSelection();
        m_centerModel->clearSelection();
        m_centerView->setCurrentIndex(QModelIndex());
        updateCenterSelection();
        return;
    }
    m_centerView->clearSelection();
    m_centerModel->clearSelection();
    m_centerView->setCurrentIndex(QModelIndex());
    updateCenterSelection();
}

void MillerViewContainer::selectFirst() {
    if (isArchiveViewActive()) {
        m_archiveTree->selectFirstEntry();
        return;
    }
    if (m_archiveSelectionActive) {
        m_previewPane->selectFirstArchiveEntry();
        return;
    }
    if (!m_centerModel || m_centerModel->rowCount() == 0) return;
    const QModelIndex index = m_centerModel->index(0, 0);
    m_centerView->setCurrentIndex(index);
    if (auto* sm = m_centerView->selectionModel()) {
        sm->select(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    }
    m_centerView->scrollTo(index, QAbstractItemView::PositionAtTop);
}

void MillerViewContainer::selectLast() {
    if (isArchiveViewActive()) {
        m_archiveTree->selectLastEntry();
        return;
    }
    if (m_archiveSelectionActive) {
        m_previewPane->selectLastArchiveEntry();
        return;
    }
    if (!m_centerModel || m_centerModel->rowCount() == 0) return;
    const QModelIndex index = m_centerModel->index(m_centerModel->rowCount() - 1, 0);
    m_centerView->setCurrentIndex(index);
    if (auto* sm = m_centerView->selectionModel()) {
        sm->select(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    }
    m_centerView->scrollTo(index, QAbstractItemView::PositionAtBottom);
}

std::filesystem::path MillerViewContainer::computeNeighbourPath(
        const std::vector<std::filesystem::path>& excluding) const {
    // ближайший к курсору элемент, не попавший под операцию: сначала вниз,
    // потом вверх, чтобы фокус не прыгал на первую строку после удаления
    int curRow = m_centerView->currentIndex().isValid() ? m_centerView->currentIndex().row() : -1;
    const int total = m_centerModel->rowCount();
    auto isExcluded = [&excluding](const std::filesystem::path& p) {
        return std::find(excluding.begin(), excluding.end(), p) != excluding.end();
    };
    for (int r = curRow + 1; r < total; ++r) {
        std::filesystem::path cand = toFs(m_centerModel->index(r, 0).data(Models::FileSystemModel::PathRole).toString());
        if (!isExcluded(cand)) return cand;
    }
    for (int r = curRow - 1; r >= 0; --r) {
        std::filesystem::path cand = toFs(m_centerModel->index(r, 0).data(Models::FileSystemModel::PathRole).toString());
        if (!isExcluded(cand)) return cand;
    }
    return {};
}

bool MillerViewContainer::selectAndFocusPath(const std::filesystem::path& p) {
    int row = m_centerModel->findRowByPath(p);
    if (row < 0) return false;
    m_centerModel->clearSelection();
    const QModelIndex targetIdx = m_centerModel->index(row, 0);
    m_centerView->setCurrentIndex(targetIdx);
    if (m_centerView->selectionModel()) {
        m_centerView->selectionModel()->select(targetIdx, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    }
    m_centerView->scrollTo(targetIdx, QAbstractItemView::PositionAtCenter);
    updateCenterSelection();
    return true;
}

void MillerViewContainer::selectPathAfterRefresh(const std::filesystem::path& p) {
    if (selectAndFocusPath(p)) {
        m_pathToSelectAfterRefresh.clear();
        m_pendingSelectRetries = 0;
        return;
    }
    // пути ещё нет в списке, выберем его после ближайших перезагрузок
    m_pathToSelectAfterRefresh = p;
    m_pendingSelectRetries = 8;
}

void MillerViewContainer::refresh() {
    // запоминаем текущую строку, чтобы фоновое обновление не прыгало на строку 0
    if (m_centerView->currentIndex().isValid()) {
        m_lastKnownRow = m_centerView->currentIndex().row();
    }
    m_parentModel->refresh();
    m_centerModel->refresh();
}

void MillerViewContainer::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    applyResponsiveSizes();
}

// пустое состояние центра: каталог не читается (permission denied), список пуст и
// содержимое не получено через sudo (count > 0 или ответ sudo в кэше), подсказка скрыта
void MillerViewContainer::updateCenterDeniedOverlay(int count) {
    if (!m_centerDeniedOverlay || !m_centerView) return;
    const bool readable = (::access(m_currentDir.c_str(), R_OK | X_OK) == 0);
    if (count > 0 || readable) {
        m_centerDeniedOverlay->hide();
        return;
    }
    // каталог уже открывался через sudo (в кэше есть действующий ответ, пусть даже
    // пустой): просто пустая колонка, никаких плашек и лишних пометок
    QString sudoOut;
    if (Core::XdgUtils::sudoListing(m_currentDir, &sudoOut)) {
        m_centerDeniedOverlay->hide();
        return;
    }
    using namespace Theme::Colors;
    m_centerDeniedHint->setText(QString(
        "<span style='color:%1;'>can't view what folder contains: permission denied</span>")
        .arg(Subtext().name()));
    // если sudo уже пробовали и система отказала: вторая строка честная и клика не предлагает
    const bool sudoHopeless = (!m_sudoDeniedDir.empty() && m_sudoDeniedDir == m_currentDir);
    m_centerDeniedActionClickable = !sudoHopeless;
    if (sudoHopeless) {
        m_centerDeniedAction->setText(QString(
            "<span style='color:%1;'>sudo can't open this folder</span>")
            .arg(Subtext().name()));
        m_centerDeniedAction->setCursor(Qt::ArrowCursor);
        m_centerDeniedAction->setToolTip(QStringLiteral("denied by system even with sudo"));
    } else {
        m_centerDeniedAction->setText(QString(
            "<span style='color:%1;'><u>try to sudo open</u></span>")
            .arg(Accent().name()));
        m_centerDeniedAction->setCursor(Qt::PointingHandCursor);
        m_centerDeniedAction->setToolTip(QStringLiteral("sudo open"));
    }
    positionCenterDeniedOverlay();
    m_centerDeniedOverlay->show();
    m_centerDeniedOverlay->raise();
}

void MillerViewContainer::positionCenterDeniedOverlay() {
    if (!m_centerDeniedOverlay || !m_centerView) return;
    m_centerDeniedOverlay->setGeometry(m_centerView->viewport()->rect());
}

bool MillerViewContainer::eventFilter(QObject* watched, QEvent* event) {
    if (m_archiveTree && watched == m_archiveTree && event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Escape || ke->key() == Qt::Key_Backspace) {
            leaveArchive();
            return true;
        }
    }
    if (m_centerView && watched == m_centerView->viewport() && event->type() == QEvent::Resize) {
        positionCenterDeniedOverlay();
    }
    // клик по «try to sudo open» поверх пустого списка = sudo open каталога;
    // если система уже отказала sudo для этого каталога, клик проглатываем
    if (watched == m_centerDeniedAction && event->type() == QEvent::MouseButtonPress) {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton) {
            if (m_centerDeniedActionClickable) {
                sudoOpenCurrent(m_currentDir);
            }
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void MillerViewContainer::setColumnsConfig(bool showParent, bool showPreview,
                                           int parentWidth, int previewWidth,
                                           bool exactWidths) {
    m_parentColumnEnabled = showParent;
    m_previewColumnEnabled = showPreview;
    // сохранённая раскладка может быть любой ширины: жёсткие 800/1000
    // обрезали реальный preview и после рестарта колонки выглядели иначе.
    // Реальный предел проверяет applyResponsiveSizes() по ширине окна.
    m_parentColumnWidth = std::clamp(parentWidth, 80, 10000);
    m_previewColumnWidth = std::clamp(previewWidth, 80, 10000);
    m_initialSizesApplied = false;
    m_exactInitialWidths = exactWidths;
    m_parentAutoHidden = false;
    m_previewAutoHidden = false;

    if (m_parentView) {
        m_parentView->setMaximumWidth(std::max(320, m_parentColumnWidth));
        if (!m_parentColumnEnabled) m_parentView->hide();
    }
    if (m_previewPane && !m_previewColumnEnabled) m_previewPane->hide();
    if (isVisible()) {
        QTimer::singleShot(0, this, [this]() {
            applyResponsiveSizes();
            update();
        });
    }
}

void MillerViewContainer::relayoutColumns() {
    m_initialSizesApplied = false;
    if (!isVisible()) return;
    QTimer::singleShot(0, this, [this]() {
        applyResponsiveSizes();
        update();
    });
}

int MillerViewContainer::parentColumnWidth() const {
    if (m_splitter) {
        const QList<int> sizes = m_splitter->sizes();
        if (!sizes.isEmpty() && sizes.front() > 0) return sizes.front();
    }
    return m_parentColumnWidth;
}

int MillerViewContainer::previewColumnWidth() const {
    if (m_splitter) {
        const QList<int> sizes = m_splitter->sizes();
        if (sizes.size() >= 3 && sizes.back() > 0) return sizes.back();
    }
    return m_previewColumnWidth;
}

void MillerViewContainer::applyResponsiveSizes() {
    if (!m_splitter || !m_previewPane || !m_parentView || !m_centerView) return;
    if (m_viewStack && m_viewStack->currentWidget() != m_splitter) return;
    int total = width();
    if (total <= 0) return;

    if (m_sortButton) m_sortButton->raise();

    // крайне узкое окно: прячем крайние панели; три панели помещаются
    // от 120+160+120+8=408px (минимумы + 2 рукоятки)
    constexpr int kHideParentBelow = 380;
    constexpr int kHidePreviewBelow = 408;

    // в очень узком контейнере центр тоже ужимается: жёсткие 160px оставляли
    // рукоятке bookmarks без хода
    const int centerMin = (total < 250) ? 56 : ((total < 300) ? 120 : 160);
    if (m_centerView->minimumWidth() != centerMin) m_centerView->setMinimumWidth(centerMin);
    if (m_centerStack && m_centerStack->minimumWidth() != centerMin) m_centerStack->setMinimumWidth(centerMin);

    const bool wantParent = m_parentColumnEnabled && total >= kHideParentBelow;
    const bool wantPreview = m_previewColumnEnabled && total >= kHidePreviewBelow;

    bool visibilityChanged = false;
    if (!wantParent) {
        if (m_parentView->isVisible()) {
            m_parentView->hide();
            m_parentAutoHidden = true;
            visibilityChanged = true;
        }
    } else if (!m_parentView->isVisible()) {
        m_parentView->show();
        m_parentAutoHidden = false;
        visibilityChanged = true;
    }

    if (!wantPreview) {
        if (m_previewPane->isVisible()) {
            m_previewPane->hide();
            m_previewAutoHidden = true;
            visibilityChanged = true;
        }
    } else if (!m_previewPane->isVisible()) {
        m_previewPane->show();
        m_previewAutoHidden = false;
        visibilityChanged = true;
    }

    // Комфортная раскладка при первом показе. Сохранённую раскладку
    // применяем на каждом изменении размера, пока пользователь не потянул
    // рукоятку: так промежуточный размер окна от WM не «замораживает»
    // неправильные ширины после рестарта.
    if ((m_exactInitialWidths || !m_initialSizesApplied) && total >= 460) {
        m_initialSizesApplied = true;
        const int handles = 2 * m_splitter->handleWidth();
        const int avail = std::max(0, total - handles);
        // Боковые колонки не должны выглядеть полосками: по умолчанию даём
        // родителю до четверти и превью до трети ширины а значения конфига работают
        // как желаемая ширина в этих пределах.
        // Для сохранённой раскладки ширины должны восстанавливаться ровно.
        // Ограничение avail/2 ломало раскладки, где preview шире половины окна:
        // например, 120 + 786 + 1000 при окне 1910 сохранялось как 1000,
        // а после рестарта превращалось в 953. Оставляем запас только на
        // вторую боковую колонку (120) и минимальный центр (200).
        const int exactSideCap = std::max(120, avail - 320);
        const int parentCap = m_exactInitialWidths
            ? exactSideCap
            : std::max(120, avail / 4);
        const int previewCap = m_exactInitialWidths
            ? exactSideCap
            : std::max(120, avail * 35 / 100);
        // wantParent/wantPreview, а не isVisible(): при первом resizeEvent
        // главное окно еще не показано и видимость у детей false
        int parentW = wantParent
            ? std::clamp(m_parentColumnWidth, 120, parentCap)
            : 0;
        int previewW = wantPreview
            ? std::clamp(m_previewColumnWidth, 120, previewCap)
            : 0;
        int centerW = avail - parentW - previewW;
        if (centerW < 200) {
            centerW = std::min(avail, 200);
            if (previewW > 0) {
                previewW = std::clamp(avail - parentW - centerW, 120,
                                      std::max(120, avail - parentW));
            }
            if (parentW > 0) {
                parentW = std::clamp(avail - previewW - centerW, 120,
                                     std::max(120, avail - previewW));
            }
            centerW = std::max(0, avail - parentW - previewW);
        }
        m_splitter->setSizes({parentW, centerW, previewW});
        return;
    }

    // обычный ресайз окна размеры не перезаписываем: QSplitter сам плавно
    // масштабирует панели, иначе рукоятка переносилась в другую сторону
    // вместо следования за мышью
    if (!visibilityChanged) return;

    // видимость панелей изменилась: пересобираем размеры из текущих пропорций
    // (жёсткие формулы дают скачок в другую сторону)
    const int handles = 2 * m_splitter->handleWidth();
    const int avail = std::max(0, total - handles);
    QList<int> cur = m_splitter->sizes();
    int curSum = 0;
    for (int v : cur) curSum += v;
    auto scaled = [&](int idx, int minV, int maxV) {
        int v = (curSum > 0 && idx < cur.size()) ? int(qint64(cur[idx]) * avail / curSum) : avail / 3;
        return std::clamp(v, minV, maxV);
    };

    constexpr int parentMin = 120, visCenterMin = 160, previewMin = 120;
    if (!m_parentView->isVisible() && !m_previewPane->isVisible()) {
        m_splitter->setSizes({0, avail, 0});
    } else if (!m_parentView->isVisible()) {
        const int previewW = scaled(2, previewMin, std::max(previewMin, avail - visCenterMin));
        m_splitter->setSizes({0, avail - previewW, previewW});
    } else if (!m_previewPane->isVisible()) {
        const int parentMax = std::min(m_parentView->maximumWidth(), std::max(parentMin, avail - visCenterMin));
        const int parentW = scaled(0, parentMin, parentMax);
        m_splitter->setSizes({parentW, avail - parentW, 0});
    } else {
        const int parentW = scaled(0, parentMin, m_parentView->maximumWidth());
        const int centerMax = std::max(visCenterMin, avail - parentW - previewMin);
        const int centerW = scaled(1, visCenterMin, centerMax);
        const int previewW = std::max(previewMin, avail - parentW - centerW);
        m_splitter->setSizes({parentW, centerW, previewW});
    }
}

void MillerViewContainer::onCenterSelectionChanged(const QItemSelection& /*selected*/, const QItemSelection& /*deselected*/) {
    // синхронизируем выделение Qt с элементами модели для выделения рамкой мышью
    if (m_centerView->selectionModel()) {
        auto selIndexes = m_centerView->selectionModel()->selectedIndexes();
        std::unordered_set<int> selectedRows;
        for (const auto& idx : selIndexes) {
            selectedRows.insert(idx.row());
        }
        for (int r = 0; r < m_centerModel->rowCount(); ++r) {
            m_centerModel->setItemSelected(r, selectedRows.contains(r));
        }
    }
    updateCenterSelection();
}

void MillerViewContainer::updateCenterSelection() {
    QModelIndex curr = m_centerView->currentIndex();
    if (curr.isValid()) {
        m_lastKnownRow = curr.row();
        QString pathStr = curr.data(Models::FileSystemModel::PathRole).toString();
        std::filesystem::path p(pathStr.toUtf8().toStdString());
        m_previewPane->previewPath(p);
        // соседние видео/PDF греются заранее; prefetch сам ограничивает очередь
        const int row = curr.row();
        if (row > 0) {
            const QString prev = m_centerModel->index(row - 1, 0)
                                     .data(Models::FileSystemModel::PathRole).toString();
            if (!prev.isEmpty()) m_previewPane->prefetchPath(
                std::filesystem::path(prev.toUtf8().toStdString()));
        }
        if (row + 1 < m_centerModel->rowCount()) {
            const QString next = m_centerModel->index(row + 1, 0)
                                     .data(Models::FileSystemModel::PathRole).toString();
            if (!next.isEmpty()) m_previewPane->prefetchPath(
                std::filesystem::path(next.toUtf8().toStdString()));
        }
        emit itemSelected(p);
    } else {
        m_previewPane->clearPreview();
    }
    if (m_archiveSelectionActive && !m_previewPane->archivePageActive()) {
        m_archiveSelectionActive = false;
        m_previewPane->clearArchiveSelection();
    }

    int totalCount = m_centerModel->rowCount();
    int selCount = static_cast<int>(m_centerModel->selectedPaths().size());
    // правило 2A: галочка только при 2+ явно выбранных
    m_centerDelegate->setCheckmarksEnabled(selCount > 1);
    if (m_centerView->viewport()) m_centerView->viewport()->update();
    int curRow = m_centerView->currentIndex().isValid() ? (m_centerView->currentIndex().row() + 1) : (totalCount > 0 ? 1 : 0);
    QString perms = curr.isValid()
        ? curr.data(Models::FileSystemModel::PermsRole).toString()
        : QString();
    emit statusUpdated(curRow, totalCount, selCount, perms);
    QStringList hookSel;
    for (const auto& sp : m_centerModel->selectedPaths()) {
        hookSel.append(QString::fromUtf8(sp.c_str()));
    }
    if (hookSel.isEmpty() && curr.isValid()) {
        hookSel.append(curr.data(Models::FileSystemModel::PathRole).toString());
    }
    NativePluginHost::fireHook(QStringLiteral("on_select"), hookSel, QString::fromUtf8(m_currentDir.c_str()));
}

void MillerViewContainer::onCenterDoubleClicked(const QModelIndex& index) {
    if (!index.isValid()) return;
    bool isDir = index.data(Models::FileSystemModel::IsDirRole).toBool();
    QString pathStr = index.data(Models::FileSystemModel::PathRole).toString();
    std::filesystem::path p(pathStr.toUtf8().toStdString());
    if (isDir) {
        setDirectory(p);
    } else {
        openFile(p);
    }
}

void MillerViewContainer::openFile(const std::filesystem::path& path) {
    if (path.empty()) return;
    if (m_fileChooseMode) {
        emit fileChosenRequested(path);
        return;
    }
    if (tryPluginOverrideWith("open", {path}, path.parent_path())) return;
    if (Core::XdgUtils::isArchive(path)) {
        enterArchive(path);
        return;
    }
    Core::XdgUtils::openWithDefault(path);
}

void MillerViewContainer::onArchiveEntrySelected(const QString& normalizedPath,
                                                 const QString& memberPath, bool isDir) {
    if (!isArchiveViewActive() || !m_previewPane) return;
    ++m_archivePreviewRequest;
    m_archivePreviewQueued = false;
    if (m_archivePreviewTimer) m_archivePreviewTimer->stop();
    if (isDir || normalizedPath.isEmpty() || memberPath.isEmpty()) {
        m_archivePreviewPendingPath.clear();
        m_archivePreviewPendingMember.clear();
        m_previewPane->clearPreview();
        return;
    }
    m_archivePreviewPendingPath = normalizedPath;
    m_archivePreviewPendingMember = memberPath;
    if (!m_archivePreviewTimer) {
        m_archivePreviewTimer = new QTimer(this);
        m_archivePreviewTimer->setSingleShot(true);
        m_archivePreviewTimer->setInterval(200);
        connect(m_archivePreviewTimer, &QTimer::timeout, this, [this]() {
            if (!isArchiveViewActive() || m_archivePreviewPendingPath.isEmpty()) return;
            if (m_archivePreviewBusy) {
                m_archivePreviewQueued = true;
                return;
            }
            if (m_archiveTree->isExtracting()) {
                m_archivePreviewTimer->start();
                return;
            }
            const quint64 request = m_archivePreviewRequest;
            const QString normalized = m_archivePreviewPendingPath;
            const QString member = m_archivePreviewPendingMember;
            m_archivePreviewBusy = true;
            std::filesystem::path real;
            QString error;
            const bool ok = m_archiveTree->extractEntryForPreview(normalized, member, real, error);
            m_archivePreviewBusy = false;
            if (request == m_archivePreviewRequest && isArchiveViewActive()) {
                if (ok) {
                    m_previewPane->previewPath(real);
                } else {
                    m_previewPane->clearPreview();
                    if (!error.isEmpty()) emit messageEmitted(error);
                }
            }
            if (m_archivePreviewQueued) {
                m_archivePreviewQueued = false;
                if (isArchiveViewActive() && !m_archivePreviewPendingPath.isEmpty()) {
                    m_archivePreviewTimer->start();
                }
            }
        });
    }
    m_archivePreviewTimer->start();
}

void MillerViewContainer::enterArchive(const std::filesystem::path& archivePath) {
    if (archivePath.empty() || !m_centerStack || !m_archiveTree || !m_archiveModel) return;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(archivePath, ec) || ec) return;

    m_archivePath = archivePath;
    if (m_archivePreviewTimer) m_archivePreviewTimer->stop();
    ++m_archivePreviewRequest;
    m_archivePreviewQueued = false;
    m_archivePreviewPendingPath.clear();
    m_archivePreviewPendingMember.clear();
    // файловые операции/Escape не должны действовать на скрытый архив под страницей
    if (m_centerModel) m_centerModel->clearSelection();
    if (m_centerView) {
        m_centerView->clearSelection();
        m_centerView->setCurrentIndex(QModelIndex());
    }

    m_archiveTree->setArchivePath(archivePath);
    m_centerStack->setCurrentWidget(m_archivePage);

    // как при открытии папки: слева каталог архива, сам архив подсвечен
    if (m_parentModel && !archivePath.parent_path().empty()) {
        const auto parent = archivePath.parent_path();
        m_parentModel->setDirectory(parent);
        applySortToModel(parent, m_parentModel);
        if (m_parentDelegate) m_parentDelegate->setHighlightedPath(archivePath);
        if (auto* sm = m_parentView->selectionModel()) {
            sm->clearSelection();
            const int row = m_parentModel->findRowByPath(archivePath);
            sm->setCurrentIndex(row >= 0 ? m_parentModel->index(row, 0) : QModelIndex(),
                                QItemSelectionModel::NoUpdate);
        }
        if (m_parentView && m_parentView->viewport()) m_parentView->viewport()->update();
    }

    m_archiveModel->setArchive(archivePath);
    if (m_archiveModel->isLoading()) {
        m_archiveTree->showMessage(QStringLiteral("Reading archive\u2026"));
    } else if (m_archiveModel->isLoaded()) {
        m_archiveTree->buildFromEntries(m_archiveModel->entries());
    } else if (!m_archiveModel->error().isEmpty()) {
        m_archiveTree->showMessage(m_archiveModel->error());
    }

    m_archiveTree->setFocus();
    emit archiveEntered(archivePath);
}

void MillerViewContainer::leaveArchive() {
    if (m_archivePath.empty()) return;
    m_archivePath.clear();
    if (m_archivePreviewTimer) m_archivePreviewTimer->stop();
    ++m_archivePreviewRequest;
    m_archivePreviewQueued = false;
    m_archivePreviewPendingPath.clear();
    m_archivePreviewPendingMember.clear();
    if (m_previewPane) m_previewPane->clearPreview();
    if (m_archiveTree) m_archiveTree->clearArchive();
    if (m_centerStack) m_centerStack->setCurrentWidget(m_centerView);
    // возвращаем левую колонку к родителю текущего каталога
    if (m_parentModel) {
        const bool hasParent =
            m_currentDir.has_parent_path() && m_currentDir != m_currentDir.parent_path();
        const auto parent = hasParent ? m_currentDir.parent_path() : m_currentDir;
        m_parentModel->setDirectory(parent);
        applySortToModel(parent, m_parentModel);
    }
    syncParentViewToCurrentDir();
    if (m_centerView) m_centerView->setFocus();
    emit archiveLeft(m_currentDir);
}

void MillerViewContainer::setMultiSelectEnabled(bool enabled) {
    if (!m_centerView) return;
    m_centerView->setSelectionMode(enabled ? QAbstractItemView::ExtendedSelection
                                           : QAbstractItemView::SingleSelection);
}

void MillerViewContainer::onFilesDropped(const std::vector<std::filesystem::path>& sources,
                                         const std::filesystem::path& targetDir,
                                         Qt::DropAction action,
                                         Qt::KeyboardModifiers modifiers) {
    // handleDrop уже делает selectPathAfterRefresh(firstDest) + refresh(); лишний
    // отложенный refresh создал бы гонку и сбросил фокус строку 0
    if (auto* view = qobject_cast<MillerListView*>(sender())) view->setFocus();
    Dnd::DragDropHandler::handleDrop(sources, targetDir, action, modifiers, this);
}

void MillerViewContainer::openCurrent() {
    auto sel = selectedPaths();
    if (!sel.empty()) {
        const auto& target = sel.front();
        std::error_code ec;
        if (std::filesystem::is_directory(target, ec)) {
            setDirectory(target);
        } else {
            openFile(target);
        }
    }
}

void MillerViewContainer::openSelectionInNewTab() {
    bool opened = false;
    for (const auto& path : selectedPaths()) {
        std::error_code ec;
        if (std::filesystem::is_directory(path, ec)) {
            emit openInNewTabRequested(path);
            opened = true;
        }
    }
    if (!opened) emit openInNewTabRequested(m_currentDir);
}

void MillerViewContainer::openWithApp(const QString& execCmd) {
    if (tryPluginOverride("open_with")) return;
    auto sel = selectedPaths();
    if (sel.empty()) return;

    Core::DesktopApp app;
    app.execCommand = execCmd;
    Core::XdgUtils::openWithApp(app, sel.front());
}

namespace {

// общая сборка mime для буфера обмена: список uri обычный текст и gnome copied files
QMimeData* buildClipboardMimeData(const std::vector<std::filesystem::path>& paths, bool isCut) {
    auto mimeData = new QMimeData();
    QList<QUrl> urls;
    QString plainText;
    QByteArray gnomeCopiedData = isCut ? "cut\n" : "copy\n";
    for (const auto& p : paths) {
        QUrl u = QUrl::fromLocalFile(toQ(p));
        urls.append(u);
        plainText += toQ(p) + "\n";
        gnomeCopiedData += u.toString().toUtf8() + "\n";
    }
    mimeData->setUrls(urls);
    mimeData->setText(plainText.trimmed());
    mimeData->setData("x-special/gnome-copied-files", gnomeCopiedData);
    return mimeData;
}

}

void MillerViewContainer::cutSelection() {
    if (m_archiveSelectionActive) return;
    if (tryPluginOverride("cut")) return;
    auto sel = selectedPaths();
    if (sel.empty()) return;

    QApplication::clipboard()->setMimeData(buildClipboardMimeData(sel, true));
    emit messageEmitted(QString("Cut %1").arg(sel.size()));
}

void MillerViewContainer::copySelection() {
    if (isArchiveViewActive()) {
        m_archiveTree->copySelectedEntries();
        return;
    }
    if (m_archiveSelectionActive) {
        m_previewPane->copyArchiveSelection();
        return;
    }
    if (tryPluginOverride("copy")) return;
    auto sel = selectedPaths();
    if (sel.empty()) return;

    QMimeData* mimeData = buildClipboardMimeData(sel, false);

    // если копируется один файл изображения, кладём в буфер и QImage, чтобы Ctrl+V работал напрямую в браузерных AI чатах
    if (sel.size() == 1) {
        std::string ext = sel.front().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp" || ext == ".bmp" || ext == ".gif") {
            QImageReader reader(QString::fromUtf8(sel.front().c_str()));
            reader.setAutoTransform(true);
            QImage img = reader.read();
            if (!img.isNull()) {
                mimeData->setImageData(img);
            }
        }
    }

    QApplication::clipboard()->setMimeData(mimeData);
    emit messageEmitted(QString("Copy %1").arg(sel.size()));
}

void MillerViewContainer::clipboardPaths(const std::vector<std::filesystem::path>& paths, bool cut) {
    if (paths.empty()) return;
    if (tryPluginOverrideWith(cut ? "cut" : "copy", paths, m_currentDir)) return;

    QApplication::clipboard()->setMimeData(buildClipboardMimeData(paths, cut));
    emit messageEmitted(cut ? QString("Cut %1").arg(paths.size())
                            : QString("Copy %1").arg(paths.size()));
}

void MillerViewContainer::pasteClipboard() {
    if (m_archiveSelectionActive) return;
    if (tryPluginOverride("paste")) return;
    const QMimeData* mimeData = QApplication::clipboard()->mimeData();
    if (!mimeData) return;

    auto paths = Dnd::DragDropHandler::extractPaths(mimeData);
    if (!paths.empty()) {
        bool isCut = false;
        if (mimeData->hasFormat("x-special/gnome-copied-files")) {
            QByteArray data = mimeData->data("x-special/gnome-copied-files");
            if (data.startsWith("cut")) {
                isCut = true;
            }
        }

        Qt::DropAction act = isCut ? Qt::MoveAction : Qt::CopyAction;
        Dnd::DragDropHandler::handleDrop(paths, m_currentDir, act, Qt::NoModifier, this);

        emit messageEmitted(isCut ? "Move" : "Copy");
        return;
    }

    // вставка изображения / скриншота / data: URI или удалённого URL из буфера в текущий каталог
    Dnd::DragDropHandler::handleMimeDrop(mimeData, m_currentDir, Qt::CopyAction, Qt::NoModifier, this);
}

void MillerViewContainer::copyPaths(int mode) {
    if (m_archiveSelectionActive) return;
    // перехват плагином: 0 = copy_path (abs), 1 = copy_name, 2 = copy_rel_path
    static const char* const copyIds[] = {"copy_path", "copy_name", "copy_rel_path"};
    if (mode >= 0 && mode <= 2 && tryPluginOverride(QLatin1String(copyIds[mode])))
        return;
    auto sel = selectedPaths();
    if (sel.empty()) {
        sel.push_back(m_currentDir);
    }

    QString text;
    for (const auto& p : sel) {
        if (mode == 0) {
            text += QString::fromUtf8(p.c_str()) + "\n";
        } else if (mode == 1) {
            text += QString::fromUtf8(p.filename().c_str()) + "\n";
        } else {
            std::error_code ec;
            auto rel = std::filesystem::relative(p, m_currentDir, ec);
            text += QString::fromUtf8(rel.c_str()) + "\n";
        }
    }

    QApplication::clipboard()->setText(text.trimmed());
    emit messageEmitted("Path copied");
}

void MillerViewContainer::renameCurrent() {
    if (m_archiveSelectionActive) return;
    if (tryPluginOverride("rename")) return;
    auto sel = selectedPaths();
    if (sel.empty()) return;

    if (sel.size() > 1) {
        emit bulkRenameRequested(sel);
        return;
    }

    renamePath(sel.front());
}

void MillerViewContainer::applyBulkRenames(
    const std::vector<std::pair<std::filesystem::path, std::filesystem::path>>& renames) {
    if (renames.empty()) return;

    QPointer<MillerViewContainer> self(this);
    const std::filesystem::path firstTarget = renames.front().second;
    auto cancelToken = std::make_shared<std::atomic_bool>(false);
    emit messageEmitted(QString("Renaming %1 items...").arg(renames.size()));
    Core::Tasks::TaskManager::instance().submit(
        QString("Bulk rename %1 items").arg(renames.size()),
        QStringLiteral("io"),
        [self, renames, cancelToken, firstTarget](Core::Tasks::TaskContext& ctx) {
            std::vector<Core::UndoItem> moved;
            QStringList errors;
            for (size_t i = 0; i < renames.size(); ++i) {
                if (ctx.cancelled()) break;
                const auto& [source, target] = renames[i];
                std::error_code ec;
                if (std::filesystem::exists(target, ec)) {
                    const QString message = QStringLiteral("Already exists: %1")
                                                .arg(QString::fromUtf8(target.c_str()));
                    errors << message;
                    ctx.log(message);
                } else {
                    QString err;
                    if (Core::XdgUtils::moveItem(source, target, err, {}, ctx.cancelToken())) {
                        moved.push_back({source, target, {}});
                    } else {
                        const QString message = err.isEmpty()
                            ? QStringLiteral("Failed to rename %1").arg(QString::fromUtf8(source.c_str()))
                            : err;
                        errors << message;
                        ctx.log(message);
                    }
                }
                ctx.progress(static_cast<qint64>(i + 1), static_cast<qint64>(renames.size()));
            }
            if (!moved.empty()) {
                Core::UndoManager::instance().recordMultiMove(moved, "Bulk rename");
            }
            const bool cancelled = ctx.cancelled();
            const QString error = errors.join(QChar('\n'));
            QMetaObject::invokeMethod(qApp, [self, cancelled, error, moved, firstTarget]() {
                if (!self) return;
                if (!moved.empty()) self->selectPathAfterRefresh(firstTarget);
                self->refresh();
                if (!moved.empty()) {
                    self->messageEmitted(QString("Renamed %1 items").arg(moved.size()));
                }
                if (!error.isEmpty()) {
                    TuiConfirmPopup::alert(self.data(), "Bulk Rename", error.left(2000));
                } else if (cancelled) {
                    self->messageEmitted(QString("Bulk rename cancelled (%1 applied)")
                                             .arg(moved.size()));
                }
            }, Qt::QueuedConnection);
        },
        0, cancelToken);
}

void MillerViewContainer::renamePath(const std::filesystem::path& oldPath) {
    if (oldPath.empty()) return;
    QString oldName = QString::fromUtf8(oldPath.filename().c_str());
    std::error_code ec;
    bool isFile = !std::filesystem::is_directory(oldPath, ec);

    bool ok = false;
    QString newName = TuiInputPopup::getText(this, "Rename Item", "Rename:",
                                             oldName, &ok, QPoint(), isFile);
    if (ok) {
        newName = newName.trimmed();
        if (newName.isEmpty() || newName == oldName) return;

        const QString nameErr = validateItemName(newName);
        if (!nameErr.isEmpty()) {
            TuiConfirmPopup::alert(this, "Rename Error", nameErr);
            return;
        }

        std::filesystem::path newPath = oldPath.parent_path() / toFs(newName);
        // rename() на POSIX молча перезаписывает существующий dest, не теряем
        // чужой файл без явного согласия
        std::error_code existsEc;
        if (std::filesystem::exists(newPath, existsEc)) {
            TuiConfirmPopup::alert(this, "Rename Error",
                                   "An item with this name already exists");
            return;
        }

        QString err;
        if (Core::XdgUtils::moveItem(oldPath, newPath, err)) {
            Core::UndoManager::instance().recordMove(oldPath, newPath, {}, "Rename");
            // правило 3A: фокусируем переименованную цель после асинхронного пересканирования
            selectPathAfterRefresh(newPath);
            refresh();
            emit messageEmitted("Renamed");
        } else {
            TuiConfirmPopup::alert(this, "Rename Error", err);
        }
    }
}

void MillerViewContainer::trashSelection() {
    if (m_archiveSelectionActive) return;
    auto sel = selectedPaths();
    if (sel.empty()) return;
    trashPaths(sel, false);
}

void MillerViewContainer::trashPaths(const std::vector<std::filesystem::path>& sel,
                                     bool checkPluginOverride) {
    if (checkPluginOverride && tryPluginOverrideWith("trash", sel, m_currentDir)) return;
    if (sel.empty()) return;

    // если элементы уже в корзине, перемещение в корзину бессмысленно, удаляем навсегда
    const char* homeEnv = std::getenv("HOME");
    const std::filesystem::path trashRoot = homeEnv
        ? std::filesystem::path(homeEnv) / ".local/share/Trash" : std::filesystem::path();
    bool inTrash = (!trashRoot.empty() && isInsideDir(m_currentDir, trashRoot));
    if (!inTrash) {
        for (const auto& p : sel) {
            if (!trashRoot.empty() && isInsideDir(p, trashRoot)) {
                inTrash = true;
                break;
            }
        }
    }
    if (inTrash) {
        deletePathsPermanently(sel, false);
        return;
    }

    // запоминаем соседа, чтобы после удаления фокус не попал на строку 0 (правило 3B)
    std::filesystem::path neighbour = computeNeighbourPath(sel);

    emit messageEmitted(QString("Moving %1 to trash...").arg(sel.size()));
    QPointer<MillerViewContainer> self(this);
    // каждый элемент это отдельный шаг задачи: прогресс и отмена между файлами,
    // а не одна непрозрачная пачка. Ошибка одного файла не отменяет остальные.
    Core::Tasks::TaskManager::instance().submit(
        QString("Trash %1 item(s)").arg(sel.size()),
        QStringLiteral("io"),
        [self, sel, neighbour](Core::Tasks::TaskContext& ctx) mutable {
            std::vector<std::pair<std::filesystem::path, std::filesystem::path>> moved;
            QStringList errors;
            for (size_t i = 0; i < sel.size(); ++i) {
                if (ctx.cancelled()) break;
                std::vector<std::filesystem::path> one{sel[i]};
                std::vector<std::pair<std::filesystem::path, std::filesystem::path>> movedOne;
                QString err;
                if (Core::XdgUtils::moveToTrash(one, err, &movedOne)) {
                    moved.insert(moved.end(), movedOne.begin(), movedOne.end());
                } else {
                    const QString message = err.isEmpty()
                        ? QStringLiteral("Failed to trash %1").arg(QString::fromUtf8(sel[i].c_str()))
                        : err;
                    errors << message;
                    ctx.log(message);
                }
                ctx.progress(static_cast<qint64>(i + 1), static_cast<qint64>(sel.size()));
            }

            // Не пишем в лог каждый успешный файл, потому что на тысячах
            // файлов это забивает кольцевой буфер и UI. Ошибки логируем выше.
            if (!moved.empty()) {
                ctx.log(QStringLiteral("Moved %1 item(s) to trash").arg(moved.size()));
            }

            const bool cancelled = ctx.cancelled();
            const QString error = errors.join(QChar('\n'));
            const bool ok = errors.isEmpty();
            QMetaObject::invokeMethod(qApp, [self, ok, cancelled, error, moved, neighbour, sel]() mutable {
                if (!self) return;
                if (!moved.empty()) {
                    std::vector<Core::UndoItem> undoItems;
                    undoItems.reserve(moved.size());
                    for (const auto& [orig, inTrash] : moved) {
                        undoItems.push_back({orig, inTrash, {}});
                    }
                    Core::UndoManager::instance().recordMultiTrash(undoItems, "Trash");
                }
                if (!neighbour.empty()) self->selectPathAfterRefresh(neighbour);
                self->refresh();
                if (cancelled) {
                    self->messageEmitted(QString("Trash cancelled (%1 moved)").arg(moved.size()));
                } else if (ok) {
                    self->messageEmitted(QString("Trash %1").arg(sel.size()));
                } else {
                    self->messageEmitted(QString("Trash %1: %2 failed").arg(moved.size()).arg(sel.size() - moved.size()));
                    TuiConfirmPopup::alert(self.data(), "Trash Error", error);
                }
            }, Qt::QueuedConnection);
        });
}

void MillerViewContainer::deletePermanentlySelection() {
    if (m_archiveSelectionActive) return;
    auto sel = selectedPaths();
    if (sel.empty()) return;
    deletePathsPermanently(sel, false);
}

void MillerViewContainer::deletePathsPermanently(const std::vector<std::filesystem::path>& sel,
                                                 bool checkPluginOverride) {
    if (checkPluginOverride && tryPluginOverrideWith("delete_perm", sel, m_currentDir)) return;
    if (sel.empty()) return;

    bool confirmed = TuiConfirmPopup::ask(this, "",
        "Are you sure?",
        "Yes", "No", true);
    if (confirmed) {
        std::filesystem::path neighbour = computeNeighbourPath(sel);
        emit messageEmitted(QString("Deleting %1...").arg(sel.size()));
        QPointer<MillerViewContainer> self(this);
        Core::Tasks::TaskManager::instance().submit(
            QString("Delete %1 item(s)").arg(sel.size()),
            QStringLiteral("io"),
            [self, sel, neighbour](Core::Tasks::TaskContext& ctx) mutable {
                QString err;
                const bool ok = Core::XdgUtils::deletePermanently(sel, err, ctx.cancelToken());
                const bool cancelled = ctx.cancelled();

                // При отмене часть файлов уже могла быть удалена. Считаем
                // фактический результат и не пишем «Delete cancelled», если
                // удаление все таки прошло
                size_t removed = 0;
                for (const auto& p : sel) {
                    std::error_code ec;
                    const auto status = std::filesystem::symlink_status(p, ec);
                    if (!std::filesystem::exists(status)) ++removed;
                }
                if (!ok && !cancelled && !err.isEmpty()) ctx.log(err);
                ctx.progress(static_cast<qint64>(removed), static_cast<qint64>(sel.size()));
                QMetaObject::invokeMethod(qApp, [self, ok, cancelled, err, sel, neighbour, removed]() mutable {
                    if (!self) return;
                    if (ok && removed == sel.size()) {
                        if (!neighbour.empty()) self->selectPathAfterRefresh(neighbour);
                        self->refresh();
                        self->messageEmitted(QString("Deleted %1").arg(sel.size()));
                    } else if (cancelled) {
                        self->refresh();
                        if (removed > 0) {
                            self->messageEmitted(
                                QString("Delete cancelled (%1 of %2 removed)")
                                    .arg(removed).arg(sel.size()));
                        } else {
                            self->messageEmitted(QStringLiteral("Delete cancelled"));
                        }
                    } else {
                        self->refresh();
                        QString detail = err;
                        if (removed > 0 || detail.isEmpty()) {
                            if (!detail.isEmpty()) detail += QStringLiteral("\n\n");
                            detail += QString("Removed %1 of %2 item(s).")
                                          .arg(removed).arg(sel.size());
                        }
                        TuiConfirmPopup::alert(self.data(), "Delete Error", detail);
                    }
                }, Qt::QueuedConnection);
            });
    }
}

void MillerViewContainer::compressSelection(const QString& format) {
    if (m_archiveSelectionActive) return;
    if (tryPluginOverride("compress")) return;
    auto sel = selectedPaths();
    if (sel.empty()) return;

    QString ext = format.isEmpty() ? QStringLiteral(".tar.gz") : format;
    if (!ext.startsWith('.')) ext = "." + ext;
    QString defaultName = QString::fromUtf8(sel.front().filename().c_str()) + ext;
    bool ok = false;
    QString archiveName = TuiInputPopup::getText(this, "Create Archive",
                                                 "Archive filename:",
                                                 defaultName, &ok, QPoint(), true);
    if (ok && !archiveName.trimmed().isEmpty()) {
        std::filesystem::path dest = m_currentDir / archiveName.trimmed().toUtf8().toStdString();
        emit messageEmitted("Archiving...");
        QPointer<MillerViewContainer> self(this);
        Core::Tasks::TaskManager::instance().submit(
            QString("Create archive %1").arg(archiveName.trimmed()),
            QStringLiteral("io"),
            [self, sel, dest](Core::Tasks::TaskContext& ctx) mutable {
                if (ctx.cancelled()) return;
                QString err;
                const bool arcOk = Core::XdgUtils::createArchive(sel, dest, err, ctx.cancelToken());
                if (arcOk) {
                    ctx.log(QStringLiteral("created %1").arg(QString::fromUtf8(dest.c_str())));
                } else if (!ctx.cancelled() && !err.isEmpty()) {
                    ctx.log(err);
                    ctx.fail(err);
                }
                ctx.progress(ctx.cancelled() ? 0 : 1, 1);
                QMetaObject::invokeMethod(qApp, [self, arcOk, err, dest]() mutable {
                    if (!self) return;
                    if (arcOk) {
                        Core::UndoManager::instance().recordCreate(dest, "Archive");
                        self->selectPathAfterRefresh(dest);
                        self->refresh();
                        self->messageEmitted("Archived");
                    } else {
                        TuiConfirmPopup::alert(self.data(), "Archive Error", err);
                    }
                }, Qt::QueuedConnection);
            });
    }
}

void MillerViewContainer::extractHere() {
    if (tryPluginOverride("extract_here")) return;
    // Extract Here: файлы архива прямо в текущую папку (как в Nautilus/Dolphin),
    // т.к. пункт называется «Here», подпапка с именем архива не создаётся
    extractSelectionInto(m_currentDir);
}

void MillerViewContainer::extractSelectionInto(const std::filesystem::path& destDir) {
    if (m_archiveSelectionActive) return;
    auto sel = selectedPaths();
    if (sel.empty()) return;
    extractArchiveInto(sel.front(), destDir);
}

void MillerViewContainer::extractArchiveInto(const std::filesystem::path& archive,
                                             const std::filesystem::path& destDir) {
    if (archive.empty()) return;

    // распаковка в destDir, который при необходимости создаёт сам extractArchive;
    // фон: bsdtar/7z ждут бесконечно, не фризим GUI; по завершении выбираем цель,
    // если она в текущем каталоге
    std::filesystem::path arc = archive;
    emit messageEmitted("Extracting...");
    QPointer<MillerViewContainer> self(this);
    Core::Tasks::TaskManager::instance().submit(
        QString("Extract %1").arg(QString::fromUtf8(arc.filename().c_str())),
        QStringLiteral("io"),
        [self, arc, destDir](Core::Tasks::TaskContext& ctx) mutable {
            if (ctx.cancelled()) return;
            QString err;
            std::filesystem::path actualDest;
            const bool exOk = Core::XdgUtils::extractArchive(arc, destDir, err, &actualDest,
                                                             ctx.cancelToken());
            if (exOk) {
                ctx.log(QStringLiteral("extracted to %1").arg(QString::fromUtf8(actualDest.c_str())));
            } else if (!ctx.cancelled() && !err.isEmpty()) {
                ctx.log(err);
                ctx.fail(err);
            }
            ctx.progress(ctx.cancelled() ? 0 : 1, 1);
            QMetaObject::invokeMethod(qApp, [self, exOk, err, actualDest]() {
                if (!self) return;
                if (exOk) {
                    self->selectPathAfterRefresh(actualDest);
                    self->refresh();
                    self->messageEmitted("Extracted");
                } else {
                    TuiConfirmPopup::alert(self.data(), "Extraction Error", err);
                }
            }, Qt::QueuedConnection);
        });
}

void MillerViewContainer::openTerminal() {
    if (tryPluginOverride("open_terminal")) return;
    Core::XdgUtils::openTerminalAt(m_currentDir);
}

bool MillerViewContainer::tryPluginOverride(const QString& actionId) {
    return tryPluginOverrideWith(actionId, selectedPaths(), m_currentDir);
}

bool MillerViewContainer::tryPluginOverrideWith(
        const QString& actionId,
        const std::vector<std::filesystem::path>& selection,
        const std::filesystem::path& cwd) {
    // плагины в приоритете: цепочка callback'ов в процессе
    if (NativePluginHost::instance().tryOverride(actionId, selection, cwd)) return true;
    const Core::Plugins::Plugin p = Core::Plugins::overrideFor(actionId, selection);
    if (p.id.isEmpty()) return false;
    runPluginByIdIn(p.id, selection, cwd);
    return true;
}

void MillerViewContainer::runPluginById(const QString& pluginId) {
    runPluginByIdIn(pluginId, selectedPaths(), m_currentDir, QString(), false, true);
}

void MillerViewContainer::runPluginCommand(const QString& pluginId, const QString& query) {
    runPluginByIdIn(pluginId, selectedPaths(), m_currentDir, query, true);
}

void MillerViewContainer::beginPluginPanes(const QString& title, bool preview) {
    if (!m_pluginPanes || !m_viewStack) return;
    m_pluginPanesTitle = title;
    m_pluginPreview = preview;
    m_pluginPanes->begin();
    m_viewStack->setCurrentWidget(m_pluginPanes);
    if (!preview) {
        m_pluginPanes->focusCenterPane();
        emit messageEmitted(QStringLiteral("Plugin: %1").arg(title));
    }
    QTimer::singleShot(0, this, [this]() {
        if (!isPluginPanesVisible()) return;
        applyPluginPaneSizes();
    });
}

void MillerViewContainer::schedulePluginPreview(const QString& pluginId, const QString& query) {
    const Core::Plugins::Plugin plugin = Core::Plugins::findById(pluginId);
    if (plugin.id.isEmpty() || plugin.native || plugin.detach ||
        plugin.commandMode != QLatin1String("query")) {
        cancelPluginPreview(true);
        return;
    }
    m_pluginPreviewPluginId = pluginId;
    m_pluginPreviewQuery = query;
    if (!m_pluginPreviewTimer) {
        m_pluginPreviewTimer = new QTimer(this);
        m_pluginPreviewTimer->setSingleShot(true);
        m_pluginPreviewTimer->setInterval(200);
        connect(m_pluginPreviewTimer, &QTimer::timeout, this, [this]() {
            const QString id = m_pluginPreviewPluginId;
            const QString pending = m_pluginPreviewQuery;
            if (id.isEmpty()) return;
            cancelPluginPreview(false);
            runPluginByIdIn(id, selectedPaths(), m_currentDir, pending, true, false, true);
        });
    }
    m_pluginPreviewTimer->start();
}

void MillerViewContainer::cancelPluginPreview(bool closePage, bool focusPage) {
    const bool active = m_pluginPreview || !m_pluginPreviewPluginId.isEmpty() ||
                        (m_pluginPreviewTimer && m_pluginPreviewTimer->isActive());
    if (!active) return;
    if (m_pluginPreviewTimer) m_pluginPreviewTimer->stop();
    ++m_pluginRunGeneration;
    if (m_pluginPreviewProcess) {
        if (auto* proc = qobject_cast<QProcess*>(m_pluginPreviewProcess.data())) {
            proc->kill();
            proc->deleteLater();
        } else {
            m_pluginPreviewProcess->deleteLater();
        }
        m_pluginPreviewProcess.clear();
    }
    m_pluginPreviewPluginId.clear();
    m_pluginPreviewQuery.clear();
    const bool wasPreview = m_pluginPreview;
    m_pluginPreview = false;
    if (closePage && wasPreview) closePluginPanes(focusPage);
}

bool MillerViewContainer::isPluginPreviewActive() const {
    return m_pluginPreview;
}

void MillerViewContainer::applyPluginPaneSizes() {
    if (!m_pluginPanes || !m_splitter) return;
    QList<int> current = m_splitter->sizes();
    while (current.size() < 3) current.append(0);
    int base[3] = {current[0], current[1], current[2]};
    if (base[0] <= 0) base[0] = m_parentColumnWidth > 0 ? m_parentColumnWidth : 300;
    if (base[1] <= 0) base[1] = 480;
    if (base[2] <= 0) base[2] = m_previewColumnWidth > 0 ? m_previewColumnWidth : 420;

    const int available = std::max(0, m_pluginPanes->width() - 2 * 2);
    const int total = base[0] + base[1] + base[2];
    QList<int> sizes;
    for (int i = 0; i < 3; ++i) {
        const int value = total > 0 ? int(qint64(base[i]) * available / total) : available / 3;
        sizes.append(std::max(40, value));
    }
    m_pluginPanes->setPaneSizes(sizes);
}

void MillerViewContainer::appendPluginPanesOutput(const QByteArray& data) {
    if (!isPluginPanesVisible()) return;
    m_pluginPanes->appendOutput(data);
}

void MillerViewContainer::appendPluginPaneOutput(int pane, const QByteArray& data) {
    if (!isPluginPanesVisible()) return;
    m_pluginPanes->appendPane(pane, data);
}

void MillerViewContainer::finishPluginPanes(int exitCode, bool preview) {
    if (!m_pluginPanes) return;
    m_pluginPanes->finish(exitCode);
    if (preview) return;
    emit messageEmitted(exitCode == 0
        ? QStringLiteral("Plugin %1 done").arg(m_pluginPanesTitle)
        : QStringLiteral("Plugin %1 failed: exit %2").arg(m_pluginPanesTitle).arg(exitCode));
}

void MillerViewContainer::closePluginPanes(bool focusPage) {
    cancelPluginPreview(false);
    if (!m_viewStack || !m_splitter) return;
    m_viewStack->setCurrentWidget(m_splitter);
    if (m_pluginPanes) m_pluginPanes->reset();
    if (focusPage) focusCenter();
    QTimer::singleShot(0, this, [this]() { relayoutColumns(); });
}

bool MillerViewContainer::isPluginPanesVisible() const {
    return m_viewStack && m_pluginPanes && m_viewStack->currentWidget() == m_pluginPanes;
}

void MillerViewContainer::runNativePluginAction(const QString& actionId) {
    NativePluginHost::instance().invokeAction(actionId, selectedPaths(), m_currentDir, QString(), true);
}

void MillerViewContainer::runNativePluginCommand(const QString& actionId, const QString& query) {
    NativePluginHost::instance().invokeAction(actionId, selectedPaths(), m_currentDir, query, false);
}

void MillerViewContainer::runPluginByIdIn(const QString& pluginId,
                                          const std::vector<std::filesystem::path>& selection,
                                          const std::filesystem::path& cwd,
                                          const QString& query,
                                          bool commandOutput,
                                          bool allowQueryPrompt,
                                          bool preview) {
    const Core::Plugins::Plugin plugin = Core::Plugins::findById(pluginId);
    if (plugin.id.isEmpty()) return;
    if (preview && (plugin.native || plugin.detach ||
                    plugin.commandMode != QLatin1String("query"))) return;

    if (!preview) cancelPluginPreview(true);

    if (plugin.native) {
        NativePluginHost::instance().invokeAction(plugin.action, selection, cwd, query, allowQueryPrompt);
        return;
    }

    std::vector<std::filesystem::path> targets = selection;
    if (targets.empty()) targets.push_back(cwd);

    QStringList paths;
    for (const auto& p : targets) paths.append(QString::fromUtf8(p.c_str()));
    const QString cwdQ = QString::fromUtf8(cwd.c_str());

    if (plugin.detach) {
        QString err;
        if (Core::Plugins::spawnDetached(plugin, paths, cwdQ, &err, query)) {
            emit messageEmitted("Plugin: " + plugin.name);
            refresh();
        } else {
            TuiConfirmPopup::alert(this, "Plugin Error", err);
        }
        return;
    }

    QString queryText = query;
    if (!preview && allowQueryPrompt && queryText.isEmpty() &&
        plugin.commandMode == QLatin1String("query") && !plugin.command.isEmpty()) {
        bool ok = false;
        const QString promptTitle = plugin.commandTitle.isEmpty() ? plugin.name : plugin.commandTitle;
        const QString promptLabel = !plugin.commandDescription.isEmpty()
            ? plugin.commandDescription
            : (!plugin.commandPlaceholder.isEmpty() ? plugin.commandPlaceholder : QStringLiteral("Query:"));
        schedulePluginPreview(plugin.id, QString());
        queryText = TuiInputPopup::getText(this, promptTitle, promptLabel, QString(), &ok,
                                           QPoint(), false, false,
                                           [this, previewId = plugin.id](const QString& text) {
                                               schedulePluginPreview(previewId, text);
                                           });
        if (!ok) {
            cancelPluginPreview(true);
            return;
        }
        cancelPluginPreview(true);
    }

    QString err;
    QProcess* proc = Core::Plugins::spawn(plugin, paths, cwdQ, this, &err, queryText);
    if (!proc) {
        if (!preview) TuiConfirmPopup::alert(this, "Plugin Error", err);
        return;
    }

    const QString title = plugin.commandTitle.isEmpty() ? plugin.name : plugin.commandTitle;
    ++m_pluginRunGeneration;
    const quint64 generation = m_pluginRunGeneration;
    auto begun = std::make_shared<bool>(false);
    if (commandOutput) {
        beginPluginPanes(title, preview);
        *begun = true;
    }
    if (preview) m_pluginPreviewProcess = proc;

    connect(proc, &QProcess::readyRead, this, [this, proc, generation, begun, title, preview]() {
        const QByteArray chunk = proc->readAll();
        if (chunk.isEmpty() || generation != m_pluginRunGeneration) return;
        if (!*begun) {
            beginPluginPanes(title, preview);
            *begun = true;
        }
        appendPluginPanesOutput(chunk);
    });
    connect(proc, &QProcess::finished, this,
            [this, proc, generation, begun, title, preview](int exitCode, QProcess::ExitStatus) {
        const QByteArray tail = proc->readAll();
        if (generation == m_pluginRunGeneration) {
            if (!tail.isEmpty()) {
                if (!*begun) {
                    beginPluginPanes(title, preview);
                    *begun = true;
                }
                appendPluginPanesOutput(tail);
            }
            if (*begun) {
                finishPluginPanes(exitCode, preview);
            } else if (!preview) {
                emit messageEmitted(exitCode == 0
                    ? QStringLiteral("Plugin %1 done").arg(title)
                    : QStringLiteral("Plugin %1 failed: exit %2").arg(title).arg(exitCode));
            }
        }
        if (preview) {
            if (m_pluginPreviewProcess == proc) m_pluginPreviewProcess.clear();
            proc->deleteLater();
            return;
        }
        proc->deleteLater();
        refresh();
    });
}

void MillerViewContainer::createNewFolder() {
    if (tryPluginOverride("new_folder")) return;
    bool ok = false;
    QString name = TuiInputPopup::getText(this, "New Folder", "Folder name:",
                                          "new_folder", &ok);
    name = name.trimmed();
    if (ok && !name.isEmpty()) {
        const QString nameErr = validateItemName(name);
        if (!nameErr.isEmpty()) {
            TuiConfirmPopup::alert(this, "Create Folder Error", nameErr);
            return;
        }
        std::filesystem::path newDir = m_currentDir / toFs(name);
        std::error_code ec;
        if (std::filesystem::create_directories(newDir, ec)) {
            Core::UndoManager::instance().recordCreate(newDir, "New Folder");
            selectPathAfterRefresh(newDir);
            refresh();
            emit messageEmitted("Created folder");
        } else {
            // каталог, защищённый root (Sudo Open): один привилегированный mkdir
            // вместе со свежим листингом find; иначе refresh показал бы старый кэш,
            // а второй привилегированный вызов второй диалог
            const bool denied = (ec == std::errc::permission_denied ||
                                 ec == std::errc::operation_not_permitted);
            QString sudoErr;
            if (denied && Core::XdgUtils::sudoFileOp(Core::XdgUtils::PrivOp::MakeDir,
                                                     {newDir}, m_currentDir, sudoErr, this)) {
                selectPathAfterRefresh(newDir);
                refresh();
                emit messageEmitted("Created folder");
            } else {
                TuiConfirmPopup::alert(this, "Create Folder Error",
                                       denied && !sudoErr.isEmpty()
                                           ? sudoErr : QString::fromUtf8(ec.message().c_str()));
            }
        }
    }
}

void MillerViewContainer::createNewFile() {
    if (tryPluginOverride("new_file")) return;
    bool ok = false;
    QString name = TuiInputPopup::getText(this, "New File", "File name:",
                                          "new_file.txt", &ok, QPoint(), true);
    name = name.trimmed();
    if (ok && !name.isEmpty()) {
        const QString nameErr = validateItemName(name);
        if (!nameErr.isEmpty()) {
            TuiConfirmPopup::alert(this, "Create File Error", nameErr);
            return;
        }
        std::filesystem::path newFile = m_currentDir / toFs(name);
        std::error_code ec;
        if (std::filesystem::exists(newFile, ec)) {
            // ofstream усёк бы существующий файл, не теряем данные пользователя
            TuiConfirmPopup::alert(this, "Create File Error", "A file with this name already exists");
            return;
        }
        errno = 0;
        std::ofstream ofs(newFile);
        if (ofs.is_open()) {
            ofs.close();
            Core::UndoManager::instance().recordCreate(newFile, "New File");
            selectPathAfterRefresh(newFile);
            refresh();
            emit messageEmitted("Created file");
        } else {
            // каталог, защищённый root (Sudo Open): привилегированный touch +
            // свежий листинг тем же вызовом (см. createNewFolder)
            const int openErrno = errno;
            const bool denied = (openErrno == EACCES || openErrno == EPERM);
            QString sudoErr;
            if (denied && Core::XdgUtils::sudoFileOp(Core::XdgUtils::PrivOp::MakeFile,
                                                     {newFile}, m_currentDir, sudoErr, this)) {
                selectPathAfterRefresh(newFile);
                refresh();
                emit messageEmitted("Created file");
            } else {
                TuiConfirmPopup::alert(this, "Create File Error",
                                       denied && !sudoErr.isEmpty()
                                           ? sudoErr : QStringLiteral("Failed to create file"));
            }
        }
    }
}

void MillerViewContainer::createBulkItems() {
    BulkCreateDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted) return;

    const QStringList names = dialog.names();
    if (names.isEmpty()) return;
    const bool folders = dialog.folders();

    std::vector<std::filesystem::path> targets;
    QStringList conflicts;
    targets.reserve(static_cast<size_t>(names.size()));
    for (const QString& name : names) {
        std::filesystem::path target = m_currentDir / toFs(name);
        std::error_code ec;
        if (std::filesystem::exists(target, ec)) {
            conflicts << name;
        } else {
            targets.push_back(std::move(target));
        }
    }

    if (targets.empty()) {
        TuiConfirmPopup::alert(this, "Create Items",
                               "All entered names already exist in this directory");
        return;
    }
    if (!conflicts.isEmpty()) {
        const QString detail = QString("%1 name(s) already exist and will be skipped:\n\n%2\n\nCreate the remaining %3 item(s)?")
                                   .arg(conflicts.size())
                                   .arg(conflicts.join(QStringLiteral(", ")).left(1200))
                                   .arg(targets.size());
        if (!TuiConfirmPopup::ask(this, "Create Items", detail, "Create", "Cancel", false)) {
            return;
        }
    }

    const std::filesystem::path firstTarget = targets.front();
    QPointer<MillerViewContainer> self(this);
    emit messageEmitted(QString("Creating %1 item(s)...").arg(targets.size()));
    Core::Tasks::TaskManager::instance().submit(
        QString("Create %1 item(s)").arg(targets.size()),
        QStringLiteral("io"),
        [self, folders, targets, firstTarget](Core::Tasks::TaskContext& ctx) {
            std::vector<Core::UndoItem> created;
            QStringList errors;
            for (size_t i = 0; i < targets.size(); ++i) {
                if (ctx.cancelled()) break;
                const auto& target = targets[i];
                bool ok = false;
                std::error_code ec;
                if (folders) {
                    ok = std::filesystem::create_directories(target, ec);
                } else {
                    if (!std::filesystem::exists(target, ec)) {
                        std::ofstream ofs(target);
                        if (ofs.is_open()) {
                            ofs.close();
                            ok = true;
                        }
                    }
                }
                if (ok) {
                    created.push_back({{}, target, {}});
                } else {
                    const QString message = QStringLiteral("Failed to create %1%2")
                                                .arg(QString::fromUtf8(target.c_str()),
                                                     ec ? QStringLiteral(": ") +
                                                              QString::fromUtf8(ec.message().c_str())
                                                        : QString());
                    errors << message;
                    ctx.log(message);
                }
                ctx.progress(static_cast<qint64>(i + 1), static_cast<qint64>(targets.size()));
            }

            if (!created.empty()) {
                Core::UndoManager::instance().recordMultiCreate(created, "Bulk create");
            }
            const bool cancelled = ctx.cancelled();
            const QString error = errors.join(QChar('\n'));
            QMetaObject::invokeMethod(qApp, [self, created, cancelled, error, firstTarget]() {
                if (!self) return;
                if (!created.empty()) self->selectPathAfterRefresh(firstTarget);
                self->refresh();
                if (!error.isEmpty()) {
                    TuiConfirmPopup::alert(self.data(), "Create Items", error.left(2000));
                } else if (cancelled) {
                    self->messageEmitted(
                        QString("Create cancelled (%1 created)").arg(created.size()));
                } else {
                    self->messageEmitted(QString("Created %1 item(s)").arg(created.size()));
                }
            }, Qt::QueuedConnection);
        });
}

void MillerViewContainer::bookmarkSelection() {
    if (tryPluginOverride("bookmark")) return;
    auto sel = selectedPaths();
    if (!sel.empty()) {
        for (const auto& p : sel) {
            emit bookmarkAdded(p);
        }
    } else {
        emit bookmarkAdded(m_currentDir);
    }
    emit messageEmitted("Bookmarked");
}

void MillerViewContainer::showProperties() {
    if (m_archiveSelectionActive) return;
    if (tryPluginOverride("properties")) return;
    auto sel = selectedPaths();
    std::filesystem::path target = sel.empty() ? m_currentDir : sel.front();
    showPropertiesFor(target);
}

void MillerViewContainer::showPropertiesFor(const std::filesystem::path& path) {
    if (path.empty()) return;
    emit propertiesRequested(path);
}

}
