#include "MillerColumns.hpp"
#include "ContextMenu.hpp"
#include "NativePlugins.hpp"
#include "TuiOutputOverlay.hpp"
#include <QProcess>
#include <QProcessEnvironment>
#include <QMimeData>
#include <QMenu>
#include <QWidgetAction>
#include <QLabel>
#include <QDrag>
#include "TuiInputPopup.hpp"
#include "../atlifm-theme/Theme.hpp"
#include "../atlifm-core/XdgUtils.hpp"
#include "../atlifm-core/Plugins.hpp"
#include "../atlifm-core/UndoManager.hpp"
#include "../atlifm-dnd/DragDropHandler.hpp"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QEvent>
#include <QApplication>
#include <QClipboard>
#include <QTimer>
#include <QThreadPool>
#include <QPointer>
#include <QScrollBar>
#include <QImageReader>
#include <QPainter>
#include <QFontMetricsF>
#include <QSettings>
#include <algorithm>
#include <fstream>
#include <unordered_set>
#include <unistd.h>

namespace Widgets {

namespace {

// path <-> QString: кодировка файловой системы и обратно
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
    // NoUpdate, т.к. setCurrentIndex() вызывает selectionCommand -> ClearAndSelect
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
    // Shift+протяжка по строкам: живое расширение anchor->hovered (DnD при Shift запрещён)
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
                // Proposed action по умолчанию MOVE как в Nautilus/Thunar/Dolphin;
                // приёмники сами отвечают Copy, если не умеют Move, а Ctrl на старте
                // драга заранее задаёт Copy для внешних приложений
                Qt::DropAction defaultAction =
                    (event->modifiers() & Qt::ControlModifier) ? Qt::CopyAction
                                                              : Qt::MoveAction;
                startDragOut(paths, event->modifiers(), defaultAction);
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

void MillerListView::startDragOut(const std::vector<std::filesystem::path>& paths,
                                  Qt::KeyboardModifiers /*modifiers*/,
                                  Qt::DropAction defaultAction) {
    if (paths.empty()) return;
    QDrag* drag = Dnd::DragDropHandler::createDrag(paths, this);
    if (drag) {
        drag->exec(Qt::CopyAction | Qt::MoveAction | Qt::LinkAction, defaultAction);
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
        event->acceptProposedAction();
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
        event->acceptProposedAction();
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
        emit filesDropped(paths, targetDir, event->proposedAction(), event->modifiers());
        event->acceptProposedAction();
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

    m_splitter = new QSplitter(Qt::Horizontal, this);
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
    m_splitter->addWidget(m_centerView);
    m_splitter->addWidget(m_previewPane);
    // превью не коллапсируется до нуля: иначе рукоятку между центром и превью
    // перехватывает зона ресайза окна, и полоска уходит за интерфейс без возврата
    m_splitter->setCollapsible(2, false);

    // левая колонка компактная (~160px), центр забирает основное растяжение (3), превью уже (1)
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 3);
    m_splitter->setStretchFactor(2, 1);
    m_splitter->setSizes({160, 480, 240});

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

    mainLayout->addWidget(m_splitter);

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
        // режим выбора файла (диалог/портал): Enter по файлу выбор, а не запуск приложением
        if (m_fileChooseMode) {
            emit fileChosenRequested(p);
            return;
        }
        if (tryPluginOverrideWith("open", {p}, p.parent_path())) return;
        Core::XdgUtils::openWithDefault(p);
    });
    connect(m_centerView, &MillerListView::navigateToParentRequested, this, [this]() {
        if (m_currentDir.has_parent_path() && m_currentDir != m_currentDir.parent_path()) {
            auto exitedDir = m_currentDir;
            selectPathAfterRefresh(exitedDir);
            setDirectory(m_currentDir.parent_path());
        }
    });
    connect(m_centerView, &MillerListView::navigateHistoryBackRequested, this, &MillerViewContainer::navigateHistoryBackRequested);
    connect(m_centerView, &MillerListView::navigateHistoryForwardRequested, this, &MillerViewContainer::navigateHistoryForwardRequested);

    // подключения навигации родительской панели (двойной клик по файлу или папке)
    connect(m_parentView, &MillerListView::enterDirectoryRequested, this, [this](const std::filesystem::path& p) {
        setDirectory(p);
    });
    connect(m_parentView, &MillerListView::openFileRequested, this, [this](const std::filesystem::path& p) {
        if (tryPluginOverrideWith("open", {p}, p.parent_path())) return;
        Core::XdgUtils::openWithDefault(p);
    });
    connect(m_parentView, &MillerListView::navigateHistoryBackRequested, this, &MillerViewContainer::navigateHistoryBackRequested);
    connect(m_parentView, &MillerListView::navigateHistoryForwardRequested, this, &MillerViewContainer::navigateHistoryForwardRequested);

    connect(m_centerView, &MillerListView::typeAheadSearchTriggered, this, &MillerViewContainer::typeAheadSearchTriggered);
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

    // свойства из превью тем же внутренним окном в центре (не в превью)
    connect(m_previewPane, &PreviewPane::propertiesRequested, this, &MillerViewContainer::showPropertiesFor);
    connect(m_previewPane, &PreviewPane::openWithDialogRequested, this, &MillerViewContainer::openWithDialogRequested);
    // Extract In… из меню превью: каталог выбирает MainWindow тем же оверлеем, что и для колонок
    connect(m_previewPane, &PreviewPane::extractInRequested, this, &MillerViewContainer::extractInPathRequested);
    // Plugins из меню превью: общий путь исполнения с колонками
    connect(m_previewPane, &PreviewPane::pluginRunRequested, this,
            [this](const QString& id, const std::filesystem::path& target, const std::filesystem::path& cwd) {
        runPluginByIdIn(id, {target}, cwd.empty() ? target.parent_path() : cwd);
    });

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
        bool selIsDir = false;
        if (idx.isValid()) {
            sel = selectedPaths();
            // тип элемента знает модель: файл под root-каталогом недоступен stat'у пользователя, но листинг от sudo
            selIsDir = idx.data(Models::FileSystemModel::IsDirRole).toBool();
        } else {
            m_centerModel->clearSelection();
            m_centerView->clearSelection();
            m_centerView->setCurrentIndex(QModelIndex());
        }
        auto menu = new TuiContextMenu(sel, m_currentDir, this);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        m_activeContextMenu = menu;

        connect(menu, &TuiContextMenu::openRequested, this, &MillerViewContainer::openCurrent);
        connect(menu, &TuiContextMenu::openWithAppRequested, this, &MillerViewContainer::openWithApp);
        connect(menu, &TuiContextMenu::openWithRequested, this, [this, sel]() {
            if (sel.empty()) return;
            if (tryPluginOverrideWith("open_with", sel, m_currentDir)) return;
            emit openWithDialogRequested(sel.front());
        });
        connect(menu, &TuiContextMenu::sudoOpenRequested, this, [this, sel, selIsDir]() {
            std::filesystem::path target = sel.empty() ? m_currentDir : sel.front();
            if (sel.empty() || selIsDir) {
                sudoOpenCurrent(target);
            } else {
                QString sudoErr;
                Core::XdgUtils::openFileWithSudo(target, sudoErr);
                if (!sudoErr.isEmpty()) {
                    emit messageEmitted(QString("Sudo denied"));
                }
            }
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
            emit extractInRequested();
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
        bool selIsDir = false;
        if (idx.isValid()) {
            QString pathStr = idx.data(Models::FileSystemModel::PathRole).toString();
            if (!pathStr.isEmpty()) {
                sel.push_back(std::filesystem::path(pathStr.toUtf8().toStdString()));
            }
            selIsDir = idx.data(Models::FileSystemModel::IsDirRole).toBool();
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
                    if (tryPluginOverrideWith("open", sel, sel.front().parent_path())) return;
                    Core::XdgUtils::openWithDefault(sel.front());
                }
            }
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
        connect(menu, &TuiContextMenu::sudoOpenRequested, this, [this, sel, parentDir, selIsDir]() {
            std::filesystem::path target = sel.empty() ? parentDir : sel.front();
            if (sel.empty() || selIsDir) {
                sudoOpenCurrent(target);
            } else {
                QString sudoErr;
                Core::XdgUtils::openFileWithSudo(target, sudoErr);
                if (!sudoErr.isEmpty()) {
                    emit messageEmitted(QString("Sudo denied"));
                }
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
            runPluginByIdIn(id, sel, parentDir);
        });
        connect(menu, &TuiContextMenu::nativeActionRequested, this,
                [this, sel, parentDir](const QString& actionId) {
            NativePluginHost::instance().invokeAction(actionId, sel, parentDir);
        });
        menu->popup(globalPos);
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

std::vector<std::filesystem::path> MillerViewContainer::selectedPaths() const {
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
    // размер шрифта в персональном stylesheet: application QSS (`* { font-size: ...px }`)
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

    // одна авторизация на действие (папка = один пароль): живой тикет молча `sudo -n find`;
    // иначе ровно один вызов pkexec (диалог пароля десктопа), его вывод наполняет колонки
    // через provideSudoListingTree, вторых привилегированных вызовов нет (второй pkexec = второй диалог);
    // без polkit резерв: попап пароля (ensureSudoAuthenticated) → `sudo -n find`
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
        // обрезанный head -c, отбрасывается
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

    // короткие плашки (обе ≤ 15 знаков); подробности в sudo-debug.log
    emit messageEmitted(sudoDenied ? QString("Sudo denied") : QString("Sudo open"));
}

void MillerViewContainer::clearTrashBin() {
    if (tryPluginOverride("clear_trash")) return;
    if (Core::XdgUtils::clearTrash()) {
        refresh();
        emit messageEmitted("Trash cleared");
    }
}

void MillerViewContainer::selectAll() {
    // selectAll() триггерит selectionChanged -> onCenterSelectionChanged сам синхронизирует модель
    m_centerView->selectAll();
}

void MillerViewContainer::clearSelection() {
    m_centerView->clearSelection();
    m_centerModel->clearSelection();
    m_centerView->setCurrentIndex(QModelIndex());
    updateCenterSelection();
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

void MillerViewContainer::applyResponsiveSizes() {
    if (!m_splitter || !m_previewPane || !m_parentView || !m_centerView) return;
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

    bool visibilityChanged = false;
    if (total < kHideParentBelow) {
        if (m_parentView->isVisible()) {
            m_parentView->hide();
            m_parentAutoHidden = true;
            visibilityChanged = true;
        }
    } else if (m_parentAutoHidden && !m_parentView->isVisible()) {
        m_parentView->show();
        m_parentAutoHidden = false;
        visibilityChanged = true;
    }

    if (total < kHidePreviewBelow) {
        if (m_previewPane->isVisible()) {
            m_previewPane->hide();
            m_previewAutoHidden = true;
            visibilityChanged = true;
        }
    } else if (m_previewAutoHidden && !m_previewPane->isVisible()) {
        m_previewPane->show();
        m_previewAutoHidden = false;
        visibilityChanged = true;
    }

    // комфортная раскладка при первом показе, один раз
    if (!m_initialSizesApplied && m_parentView->isVisible() && m_previewPane->isVisible() && total >= 460) {
        m_initialSizesApplied = true;
        int parentW = std::clamp(total * 16 / 100, 140, 180);
        int previewW = std::max(120, total * 35 / 100);
        int centerW = total - parentW - previewW;
        if (centerW < 200) {
            centerW = 200;
            previewW = std::max(120, total - parentW - centerW);
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
        emit itemSelected(p);
    } else {
        m_previewPane->clearPreview();
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
    } else if (m_fileChooseMode) {
        emit fileChosenRequested(p);
    } else {
        Core::XdgUtils::openWithDefault(p);
    }
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
            if (tryPluginOverride("open")) return;
            Core::XdgUtils::openWithDefault(target);
        }
    }
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

// общая сборка mime для буфера обмена при cut/copy: список URI, обычный текст и gnome-copied-files
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
    if (tryPluginOverride("cut")) return;
    auto sel = selectedPaths();
    if (sel.empty()) return;

    QApplication::clipboard()->setMimeData(buildClipboardMimeData(sel, true));
    emit messageEmitted(QString("Cut %1").arg(sel.size()));
}

void MillerViewContainer::copySelection() {
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

void MillerViewContainer::pasteClipboard() {
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
    if (tryPluginOverride("rename")) return;
    auto sel = selectedPaths();
    if (sel.empty()) return;

    const auto& oldPath = sel.front();
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
    if (tryPluginOverride("trash")) return;
    auto sel = selectedPaths();
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
        deletePermanentlySelection();
        return;
    }

    // запоминаем соседа, чтобы после удаления фокус не попал на строку 0 (правило 3B)
    std::filesystem::path neighbour = computeNeighbourPath(sel);

    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> moved;
    // фон: gio trash / QFile::moveToTrash могут висеть до 3с, не фризим GUI
    emit messageEmitted(QString("Moving %1 to trash...").arg(sel.size()));
    QPointer<MillerViewContainer> self(this);
    QThreadPool::globalInstance()->start([self, sel, neighbour]() mutable {
        QString err;
        std::vector<std::pair<std::filesystem::path, std::filesystem::path>> moved;
        bool ok = Core::XdgUtils::moveToTrash(sel, err, &moved);
        QMetaObject::invokeMethod(qApp, [self, ok, err, moved, neighbour, sel]() mutable {
            if (!self) return;
            if (ok) {
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
                self->messageEmitted(QString("Trash %1").arg(sel.size()));
            } else {
                TuiConfirmPopup::alert(self.data(), "Trash Error", err);
            }
        }, Qt::QueuedConnection);
    });
}

void MillerViewContainer::deletePermanentlySelection() {
    if (tryPluginOverride("delete_perm")) return;
    auto sel = selectedPaths();
    if (sel.empty()) return;

    bool confirmed = TuiConfirmPopup::ask(this, "",
        "Are you sure?",
        "Yes", "No", true);
    if (confirmed) {
        std::filesystem::path neighbour = computeNeighbourPath(sel);
        // фон: sudo rm может висеть до 3с на файл, не фризим GUI
        emit messageEmitted(QString("Deleting %1...").arg(sel.size()));
        QPointer<MillerViewContainer> self(this);
        QThreadPool::globalInstance()->start([self, sel, neighbour]() mutable {
            QString err;
            bool ok = Core::XdgUtils::deletePermanently(sel, err);
            QMetaObject::invokeMethod(qApp, [self, ok, err, sel, neighbour]() mutable {
                if (!self) return;
                if (ok) {
                    if (!neighbour.empty()) self->selectPathAfterRefresh(neighbour);
                    self->refresh();
                    self->messageEmitted(QString("Deleted %1").arg(sel.size()));
                } else {
                    TuiConfirmPopup::alert(self.data(), "Delete Error", err);
                }
            }, Qt::QueuedConnection);
        });
    }
}

void MillerViewContainer::compressSelection(const QString& format) {
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
        // фон: tar/zip ждут бесконечно (waitForFinished(-1)), не фризим GUI
        emit messageEmitted("Archiving...");
        QPointer<MillerViewContainer> self(this);
        QThreadPool::globalInstance()->start([self, sel, dest]() mutable {
            QString err;
            bool arcOk = Core::XdgUtils::createArchive(sel, dest, err);
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
    QThreadPool::globalInstance()->start([self, arc, destDir]() {
        QString err;
        std::filesystem::path actualDest;
        bool exOk = Core::XdgUtils::extractArchive(arc, destDir, err, &actualDest);
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
    runPluginByIdIn(pluginId, selectedPaths(), m_currentDir);
}

void MillerViewContainer::runNativePluginAction(const QString& actionId) {
    NativePluginHost::instance().invokeAction(actionId, selectedPaths(), m_currentDir);
}

void MillerViewContainer::runPluginByIdIn(const QString& pluginId,
                                          const std::vector<std::filesystem::path>& selection,
                                          const std::filesystem::path& cwd) {
    const Core::Plugins::Plugin plugin = Core::Plugins::findById(pluginId);
    if (plugin.id.isEmpty()) return;

    // плагины (манифест из *.so): in-process вызов вместо QProcess
    if (plugin.native) {
        NativePluginHost::instance().invokeAction(plugin.action, selection, cwd);
        return;
    }

    // пустое выделение (фон): плагин работает с самим каталогом
    std::vector<std::filesystem::path> targets = selection;
    if (targets.empty()) targets.push_back(cwd);

    QStringList paths;
    for (const auto& p : targets) paths.append(QString::fromUtf8(p.c_str()));
    const QString cwdQ = QString::fromUtf8(cwd.c_str());

    if (plugin.detach) {
        QString err;
        if (Core::Plugins::spawnDetached(plugin, paths, cwdQ, &err)) {
            emit messageEmitted("Plugin: " + plugin.name);
            refresh();
        } else {
            TuiConfirmPopup::alert(this, "Plugin Error", err);
        }
        return;
    }

    // оверлей вывода: один на окно (правый нижний угол), не забирает фокус
    QWidget* host = window() ? window() : this;
    QPointer<TuiOutputOverlay> overlay =
        host->findChild<TuiOutputOverlay*>(QStringLiteral("tuiOutputOverlay"));
    if (!overlay) overlay = new TuiOutputOverlay(host);

    QString err;
    QProcess* proc = Core::Plugins::spawn(plugin, paths, cwdQ, this, &err);
    if (!proc) {
        TuiConfirmPopup::alert(this, "Plugin Error", err);
        return;
    }

    overlay->begin(plugin.name);
    emit messageEmitted("Plugin: " + plugin.name);

    connect(proc, &QProcess::readyRead, overlay, [proc, overlay]() {
        if (overlay) overlay->appendOutput(proc->readAll());
    });
    QPointer<MillerViewContainer> self(this);
    const QString name = plugin.name;
    connect(proc, &QProcess::finished, this,
            [self, overlay, name, proc](int exitCode, QProcess::ExitStatus) {
        overlay->appendOutput(proc->readAll());
        overlay->finish(exitCode);
        proc->deleteLater();
        if (!self) return;
        self->refresh();
        self->messageEmitted(exitCode == 0 ? "Plugin " + name + " done"
                                           : "Plugin " + name + " failed");
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
