#include "TasksOverlay.hpp"
#include "../atlifm-core/Keybinds.hpp"
#include "../atlifm-theme/Theme.hpp"

#include <QDateTime>
#include <QEvent>
#include <QFontMetrics>
#include <QHideEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QShowEvent>
#include <QWheelEvent>
#include <algorithm>
#include <utility>

namespace Widgets {

namespace {

using Core::Tasks::State;
using Core::Tasks::TaskId;
using Core::Tasks::TaskInfo;

QString stateGlyph(State state) {
    switch (state) {
    case State::Queued: return QStringLiteral("·");
    case State::Running: return QStringLiteral("▸");
    case State::Done: return QStringLiteral("✓");
    case State::Failed: return QStringLiteral("✗");
    case State::Cancelled: return QStringLiteral("–");
    }
    return QStringLiteral("·");
}

QColor stateColor(State state) {
    using namespace Theme::Colors;
    switch (state) {
    case State::Queued: return Subtext();
    case State::Running: return Accent();
    case State::Done: return Green();
    case State::Failed: return Red();
    case State::Cancelled: return Dim();
    }
    return Subtext();
}

bool isActive(State state) {
    return state == State::Queued || state == State::Running;
}

QString humanBytes(quint64 bytes) {
    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    return unit == 0 ? QStringLiteral("%1 B").arg(bytes)
                     : QStringLiteral("%1 %2").arg(value, 0, 'f', value >= 100 ? 0 : 1)
                                            .arg(QString::fromLatin1(units[unit]));
}

QString formatAmount(qint64 value, const QString& unit) {
    if (value < 0) return QStringLiteral("?");
    if (unit == QLatin1String("bytes")) return humanBytes(static_cast<quint64>(value));
    return QString::number(value);
}

QString formatDuration(qint64 seconds) {
    if (seconds < 0 || seconds > 359999) return QStringLiteral("--:--");
    const qint64 h = seconds / 3600;
    const qint64 m = (seconds % 3600) / 60;
    const qint64 s = seconds % 60;
    if (h > 0) return QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'));
    return QStringLiteral("%1:%2").arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'));
}

QString elide(const QString& text, const QFontMetrics& fm, int width) {
    return fm.elidedText(text, Qt::ElideMiddle, std::max(20, width));
}

} // namespace

TasksOverlay::TasksOverlay(QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("tasksOverlay"));
    setAttribute(Qt::WA_StyledBackground, true);
    setFocusPolicy(Qt::StrongFocus);
    setStyleSheet(QString(
        "QWidget#tasksOverlay { background-color: %1; border: 2px solid %2; border-radius: 0px; }"
    ).arg(Theme::Colors::OverlayBg().name(), Theme::Colors::OverlayBorder().name()));

    m_logView = new QPlainTextEdit(this);
    m_logView->setObjectName(QStringLiteral("tasksLogView"));
    m_logView->setReadOnly(true);
    m_logView->setFont(Theme::getMonospaceFont(9));
    m_logView->setFocusPolicy(Qt::NoFocus);
    m_logView->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_logView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_logView->setMaximumBlockCount(5000);
    m_logView->setStyleSheet(QString(R"(
        QPlainTextEdit {
            background-color: %1;
            color: %2;
            border: 1px solid %3;
            padding: 6px;
        }
    )").arg(Theme::Colors::Mantle().name(), Theme::Colors::Text().name(),
            Theme::Colors::Surface1().name()));
    m_logView->hide();

    connect(&Core::Tasks::TaskManager::instance(), &Core::Tasks::TaskManager::added,
            this, [this](const Core::Tasks::TaskInfo&) { refresh(); });
    connect(&Core::Tasks::TaskManager::instance(), &Core::Tasks::TaskManager::updated,
            this, [this](const Core::Tasks::TaskInfo&) { refresh(); });
    connect(&Core::Tasks::TaskManager::instance(), &Core::Tasks::TaskManager::finished,
            this, [this](const Core::Tasks::TaskInfo&) { refresh(); });
    connect(&Core::Tasks::TaskManager::instance(), &Core::Tasks::TaskManager::logAppended,
            this, [this](Core::Tasks::TaskId id, const QString& line) {
                if (!m_logMode || id != m_logTask || !m_logView) return;
                // добавляем по одной строке чтобы не пересобирать весь текст
                if (m_logView->toPlainText() == QStringLiteral("(no log output)")) {
                    m_logView->clear();
                }
                auto* bar = m_logView->verticalScrollBar();
                const bool follow = !bar || bar->value() >= bar->maximum() - 2;
                m_logView->appendPlainText(line);
                if (follow && bar) bar->setValue(bar->maximum());
            });

    hide();
}

void TasksOverlay::refresh() {
    const TaskInfo previous = Core::Tasks::TaskManager::instance().task(m_selectedId);
    QVector<TaskInfo> all = Core::Tasks::TaskManager::instance().tasks();
    std::stable_sort(all.begin(), all.end(), [](const TaskInfo& a, const TaskInfo& b) {
        const bool aa = isActive(a.state);
        const bool ba = isActive(b.state);
        if (aa != ba) return aa;
        return a.id > b.id;
    });
    m_tasks = std::move(all);

    bool found = false;
    for (const TaskInfo& task : std::as_const(m_tasks)) {
        if (task.id == m_selectedId) {
            found = true;
            break;
        }
    }
    if (!found) {
        m_selectedId = m_tasks.isEmpty() ? 0 : m_tasks.front().id;
    } else if (previous.id == 0) {
        m_selectedId = m_tasks.isEmpty() ? 0 : m_tasks.front().id;
    }
    ensureSelectionVisible();

    // лог обновляется по строке через logappended чтобы полный пересбор текста не сбрасывал скролл при каждом тике прогресса
    update();
}

void TasksOverlay::repositionWithinParent() {
    QWidget* p = parentWidget();
    if (!p) return;
    const int pw = p->width();
    const int ph = p->height();
    constexpr int kMargin = 24;
    const int w = std::min(760, std::max(420, pw * 58 / 100));
    const int h = std::min(460, std::max(260, ph * 48 / 100));
    setGeometry(std::max(8, pw - w - kMargin),
                std::max(8, ph - h - kMargin),
                w, h);
}

QRect TasksOverlay::headerRect() const {
    return rect().adjusted(12, 8, -12, 0).intersected(QRect(0, 0, width(), 34));
}

QRect TasksOverlay::bodyRect() const {
    return rect().adjusted(10, 36, -10, -30);
}

QRect TasksOverlay::footerRect() const {
    return QRect(12, height() - 26, width() - 24, 20);
}

int TasksOverlay::visibleRows() const {
    return std::max(1, bodyRect().height() / 28);
}

int TasksOverlay::rowAt(int y) const {
    const QRect body = bodyRect();
    if (y < body.top() || y > body.bottom()) return -1;
    const int row = (y - body.top()) / 28 + m_scroll;
    if (row < 0 || row >= m_tasks.size()) return -1;
    return row;
}

void TasksOverlay::ensureSelectionVisible() {
    if (m_tasks.isEmpty()) {
        m_scroll = 0;
        return;
    }
    int index = 0;
    for (int i = 0; i < m_tasks.size(); ++i) {
        if (m_tasks[i].id == m_selectedId) {
            index = i;
            break;
        }
    }
    if (index < m_scroll) m_scroll = index;
    if (index >= m_scroll + visibleRows()) m_scroll = index - visibleRows() + 1;
    m_scroll = std::clamp(m_scroll, 0, std::max(0, static_cast<int>(m_tasks.size()) - visibleRows()));
}

void TasksOverlay::paintEvent(QPaintEvent* event) {
    QWidget::paintEvent(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setFont(Theme::getMonospaceFont(9));

    const QRect header = headerRect();
    painter.setPen(Theme::Colors::Accent());
    QFont headerFont = Theme::getMonospaceFont(9, QFont::Bold);
    painter.setFont(headerFont);
    const int active = Core::Tasks::TaskManager::instance().activeCount();
    const QString title = m_logMode
        ? QStringLiteral("▸ TASKS / LOG #%1").arg(m_logTask)
        : QStringLiteral("▸ TASKS");
    painter.drawText(header, Qt::AlignLeft | Qt::AlignVCenter, title);

    painter.setFont(Theme::getMonospaceFont(8));
    painter.setPen(active > 0 ? Theme::Colors::Green() : Theme::Colors::Dim());
    const QString summary = active > 0
        ? QStringLiteral("%1 active · %2 total").arg(active).arg(m_tasks.size())
        : QStringLiteral("idle");
    painter.drawText(header, Qt::AlignRight | Qt::AlignVCenter, summary);

    if (!m_logMode) {
        const QRect body = bodyRect();
        painter.save();
        painter.setClipRect(body);

        QFont rowFont = Theme::getMonospaceFont(9);
        painter.setFont(rowFont);
        const QFontMetrics fm(rowFont);

        if (m_tasks.isEmpty()) {
            painter.setPen(Theme::Colors::Dim());
            painter.drawText(body.adjusted(10, 10, -10, -10),
                             Qt::AlignLeft | Qt::AlignTop,
                             QStringLiteral("No background tasks."));
        }

        for (int i = m_scroll; i < m_tasks.size(); ++i) {
            const TaskInfo& task = m_tasks[i];
            const int visibleIndex = i - m_scroll;
            const QRect row(body.left() + 2, body.top() + visibleIndex * 28, body.width() - 4, 26);
            if (row.top() > body.bottom()) break;

            if (task.id == m_selectedId) {
                painter.fillRect(row, Theme::Colors::Surface0());
            }

            const QColor color = stateColor(task.state);
            painter.setPen(task.id == m_selectedId ? Theme::Colors::Text() : color);
            painter.drawText(QRect(row.left() + 2, row.top(), 14, row.height()),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             task.id == m_selectedId ? QStringLiteral("▸") : QStringLiteral(" "));
            painter.setPen(color);
            painter.drawText(QRect(row.left() + 16, row.top(), 14, row.height()),
                             Qt::AlignLeft | Qt::AlignVCenter, stateGlyph(task.state));

            const int textLeft = row.left() + 34;
            const int titleWidth = std::min(300, std::max(100, row.width() * 38 / 100));
            painter.setPen(task.state == State::Failed ? Theme::Colors::Red()
                                                       : Theme::Colors::Text());
            painter.drawText(QRect(textLeft, row.top(), titleWidth, row.height()),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             elide(task.title, fm, titleWidth));

            int x = textLeft + titleWidth + 8;
            painter.setPen(Theme::Colors::Dim());
            const QString kind = task.kind.isEmpty() ? QStringLiteral("io") : task.kind;
            painter.drawText(QRect(x, row.top(), 50, row.height()),
                             Qt::AlignLeft | Qt::AlignVCenter, kind);
            x += 54;

            const int percentWidth = 42;
            const int detailWidth = std::min(210, std::max(90, row.width() / 4));
            const int barWidth = std::min(120, std::max(60, row.right() - x - percentWidth - detailWidth - 16));

            if (barWidth >= 60 && task.total > 0) {
                const int cells = std::max(4, barWidth / fm.horizontalAdvance(QChar(0x2588)));
                const int filled = std::clamp<int>(
                    static_cast<int>(task.done * cells / std::max<qint64>(1, task.total)), 0, cells);
                painter.setPen(color);
                painter.drawText(QRect(x, row.top(), barWidth, row.height()),
                                 Qt::AlignLeft | Qt::AlignVCenter,
                                 QString(filled, QChar(0x2588)) + QString(cells - filled, QChar(0x2591)));
            } else if (barWidth >= 60) {
                painter.setPen(Theme::Colors::Dim());
                painter.drawText(QRect(x, row.top(), barWidth, row.height()),
                                 Qt::AlignLeft | Qt::AlignVCenter,
                                 task.state == State::Queued ? QStringLiteral("queued")
                                                             : QStringLiteral("working…"));
            }
            x += barWidth + 8;

            const int pct = task.total > 0
                ? std::clamp<int>(static_cast<int>(task.done * 100 / task.total), 0, 100)
                : -1;
            painter.setPen(color);
            painter.drawText(QRect(x, row.top(), percentWidth, row.height()),
                             Qt::AlignRight | Qt::AlignVCenter,
                             pct >= 0 ? QStringLiteral("%1%").arg(pct) : QStringLiteral("—"));
            x += percentWidth + 8;

            QString detail;
            if (task.state == State::Failed) {
                detail = task.error;
            } else if (task.state == State::Running || task.state == State::Queued) {
                if (task.cancelRequested) {
                    detail = QStringLiteral("cancelling…");
                } else if (task.startedAt.isValid() && isActive(task.state)) {
                    const qint64 elapsedMs = task.startedAt.msecsTo(QDateTime::currentDateTimeUtc());
                    if (elapsedMs >= 500 && task.done > 0) {
                        const double rate = task.done * 1000.0 / elapsedMs;
                        detail = task.unit == QLatin1String("bytes")
                            ? QStringLiteral("%1/s").arg(humanBytes(static_cast<quint64>(rate)))
                            : QStringLiteral("%1/s").arg(rate, 0, 'f', rate >= 10 ? 0 : 1);
                        if (task.total > task.done && rate > 0) {
                            detail += QStringLiteral(" · ETA %1").arg(
                                formatDuration(static_cast<qint64>((task.total - task.done) / rate)));
                        }
                    }
                }
                if (detail.isEmpty()) {
                    detail = task.total > 0
                        ? QStringLiteral("%1 / %2").arg(formatAmount(task.done, task.unit),
                                                       formatAmount(task.total, task.unit))
                        : QStringLiteral("started…");
                }
            } else if (task.state == State::Done) {
                detail = QStringLiteral("%1 · done").arg(formatAmount(task.done, task.unit));
            } else if (task.state == State::Cancelled) {
                detail = QStringLiteral("cancelled");
            }

            if (detailWidth >= 80 && x < row.right()) {
                painter.setPen(task.state == State::Failed ? Theme::Colors::Red()
                                                           : Theme::Colors::Dim());
                painter.drawText(QRect(x, row.top(), row.right() - x, row.height()),
                                 Qt::AlignRight | Qt::AlignVCenter,
                                 elide(detail, fm, row.right() - x));
            }
        }
        painter.restore();
    }

    painter.setFont(Theme::getMonospaceFont(8));
    painter.setPen(Theme::Colors::Dim());
    // только настроенный бинд задачи и отдельного локального w нет
    const QStringList configured = Core::Keybinds::keysFor(QStringLiteral("tasks"));
    const QString closeHint = (configured.isEmpty() ? QStringLiteral("Alt+W") : configured.front())
                              + QStringLiteral(" close");
    const QString keys = m_logMode
        ? QStringLiteral("↑/↓ PgUp/PgDn scroll   Esc back   %1").arg(closeHint)
        : QStringLiteral("↑/↓ select   Del cancel   Enter log   %1").arg(closeHint);
    painter.drawText(footerRect(), Qt::AlignLeft | Qt::AlignVCenter, keys);

    if (!m_tasks.isEmpty() && !m_logMode) {
        const TaskInfo selected = Core::Tasks::TaskManager::instance().task(m_selectedId);
        if (selected.id != 0) {
            painter.drawText(footerRect(), Qt::AlignRight | Qt::AlignVCenter,
                             QStringLiteral("#%1").arg(selected.id));
        }
    }
}

bool TasksOverlay::event(QEvent* event) {
    // пока оверлей открыт клавиши del и enter и стрелки идут в него а не в глобальные шорткаты чтобы del не удалил файл в корзину вместо отмены задачи
    if (event->type() == QEvent::ShortcutOverride && isVisible()) {
        auto* key = static_cast<QKeyEvent*>(event);
        switch (key->key()) {
        case Qt::Key_Delete:
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Escape:
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_Home:
        case Qt::Key_End:
        case Qt::Key_PageUp:
        case Qt::Key_PageDown:
            event->accept();
            return true;
        default:
            break;
        }
    }
    return QWidget::event(event);
}

void TasksOverlay::keyPressEvent(QKeyEvent* event) {
    auto scrollLog = [this](int delta) {
        if (!m_logView) return;
        auto* bar = m_logView->verticalScrollBar();
        if (!bar) return;
        bar->setValue(std::clamp(bar->value() + delta, bar->minimum(), bar->maximum()));
    };

    switch (event->key()) {
    case Qt::Key_Escape:
        if (m_logMode) {
            leaveLog();
        } else {
            emit closeRequested();
        }
        return;
    case Qt::Key_Up:
        if (m_logMode) {
            scrollLog(-1);
        } else {
            moveSelection(-1);
        }
        return;
    case Qt::Key_Down:
        if (m_logMode) {
            scrollLog(1);
        } else {
            moveSelection(1);
        }
        return;
    case Qt::Key_PageUp:
        if (m_logMode && m_logView && m_logView->verticalScrollBar()) {
            scrollLog(-m_logView->verticalScrollBar()->pageStep());
        }
        return;
    case Qt::Key_PageDown:
        if (m_logMode && m_logView && m_logView->verticalScrollBar()) {
            scrollLog(m_logView->verticalScrollBar()->pageStep());
        }
        return;
    case Qt::Key_Home:
        if (m_logMode) {
            if (m_logView && m_logView->verticalScrollBar()) {
                m_logView->verticalScrollBar()->setValue(m_logView->verticalScrollBar()->minimum());
            }
        } else if (!m_tasks.isEmpty()) {
            m_selectedId = m_tasks.front().id;
            m_scroll = 0;
            update();
        }
        return;
    case Qt::Key_End:
        if (m_logMode) {
            if (m_logView && m_logView->verticalScrollBar()) {
                m_logView->verticalScrollBar()->setValue(m_logView->verticalScrollBar()->maximum());
            }
        } else if (!m_tasks.isEmpty()) {
            m_selectedId = m_tasks.back().id;
            ensureSelectionVisible();
            update();
        }
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (!m_logMode && m_selectedId != 0) showLog(m_selectedId);
        return;
    case Qt::Key_Delete:
        if (!m_logMode && m_selectedId != 0) {
            Core::Tasks::TaskManager::instance().cancel(m_selectedId);
            refresh();
        }
        return;
    default:
        break;
    }
    QWidget::keyPressEvent(event);
}

void TasksOverlay::mousePressEvent(QMouseEvent* event) {
    if (m_logMode) {
        QWidget::mousePressEvent(event);
        return;
    }
    const int row = rowAt(static_cast<int>(event->position().y()));
    if (row >= 0) {
        m_selectedId = m_tasks[row].id;
        update();
    }
}

void TasksOverlay::mouseDoubleClickEvent(QMouseEvent* event) {
    if (m_logMode) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }
    const int row = rowAt(static_cast<int>(event->position().y()));
    if (row >= 0) {
        m_selectedId = m_tasks[row].id;
        showLog(m_selectedId);
    }
}

void TasksOverlay::wheelEvent(QWheelEvent* event) {
    if (m_logMode) {
        QWidget::wheelEvent(event);
        return;
    }
    const int steps = event->angleDelta().y() > 0 ? -3 : 3;
    m_scroll = std::clamp(m_scroll + steps, 0,
                          std::max(0, static_cast<int>(m_tasks.size()) - visibleRows()));
    update();
    event->accept();
}

void TasksOverlay::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (m_logView) m_logView->setGeometry(bodyRect());
}

void TasksOverlay::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    refresh();
    setFocus();
}

void TasksOverlay::hideEvent(QHideEvent* event) {
    // закрытие по alt+w или esc сбрасывает режим лога чтобы при следующем открытии сразу видеть список задач
    leaveLog();
    QWidget::hideEvent(event);
}

void TasksOverlay::moveSelection(int delta) {
    if (m_tasks.isEmpty()) return;
    int index = 0;
    for (int i = 0; i < m_tasks.size(); ++i) {
        if (m_tasks[i].id == m_selectedId) {
            index = i;
            break;
        }
    }
    index = std::clamp(index + delta, 0, static_cast<int>(m_tasks.size()) - 1);
    m_selectedId = m_tasks[index].id;
    ensureSelectionVisible();
    update();
}

void TasksOverlay::showLog(TaskId id) {
    m_logTask = id;
    m_logMode = true;
    if (m_logView) {
        m_logView->setGeometry(bodyRect());
        m_logView->show();
        m_logView->raise();
    }
    updateLogView();
    update();
}

void TasksOverlay::leaveLog() {
    m_logMode = false;
    m_logTask = 0;
    if (m_logView) {
        m_logView->hide();
        m_logView->clear();
    }
    update();
}

void TasksOverlay::updateLogView() {
    if (!m_logMode || !m_logView) return;
    const QStringList lines = Core::Tasks::TaskManager::instance().log(m_logTask);
    const QString text = lines.isEmpty() ? QStringLiteral("(no log output)")
                                         : lines.join(QChar('\n'));
    m_logView->setPlainText(text);
    // скроллим лог к последним строкам
    if (auto* bar = m_logView->verticalScrollBar()) bar->setValue(bar->maximum());
}

} // namespace widgets
