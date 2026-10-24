#include "TuiPanesOverlay.hpp"
#include "../atlifm-theme/Theme.hpp"
#include <QEvent>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QPalette>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QSplitter>
#include <QTextCursor>
#include <QTextOption>
#include <QVBoxLayout>

namespace Widgets {

namespace {
constexpr int kPaneCount = 3;
const QByteArray kPaneMarker("@@pane:");
}

TuiPanesOverlay::TuiPanesOverlay(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("tuiPanesOverlay"));
    setFocusPolicy(Qt::StrongFocus);

    QPalette background = palette();
    background.setColor(QPalette::Window, Theme::Colors::Base());
    setAutoFillBackground(true);
    setPalette(background);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setObjectName(QStringLiteral("tuiPanesSplitter"));
    m_splitter->setHandleWidth(2);
    m_splitter->setChildrenCollapsible(false);
    m_splitter->setOpaqueResize(true);

    const QString viewStyle = QString(R"(
        QPlainTextEdit {
            background-color: %1;
            color: %2;
            border: none;
            padding: 6px;
            selection-background-color: %3;
            selection-color: %4;
        }
    )").arg(Theme::Colors::Base().name(), Theme::Colors::Text().name(),
            Theme::Colors::SelectionBg().name(), Theme::Colors::SelectionFg().name());

    for (int i = 0; i < kPaneCount; ++i) {
        auto* view = new QPlainTextEdit(m_splitter);
        view->setObjectName(QStringLiteral("tuiPanesView%1").arg(i));
        view->setReadOnly(true);
        view->setFont(Theme::getMonospaceFont(9));
        view->setLineWrapMode(QPlainTextEdit::WidgetWidth);
        view->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        view->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        view->setMaximumBlockCount(5000);
        view->setMinimumWidth(40);
        view->setTabStopDistance(QFontMetricsF(view->font()).horizontalAdvance(QLatin1Char(' ')) * 4);
        view->setStyleSheet(viewStyle);
        view->installEventFilter(this);
        m_splitter->addWidget(view);
        m_views[i] = view;
    }
    for (int i = 0; i < kPaneCount; ++i) m_splitter->setStretchFactor(i, 1);
    m_splitter->setSizes({1, 1, 1});
    layout->addWidget(m_splitter, 1);
}

void TuiPanesOverlay::begin() {
    reset();
    m_running = true;
}

void TuiPanesOverlay::appendOutput(const QByteArray& chunk) {
    m_pending += chunk;
    QByteArray panes[kPaneCount];
    int pane = m_currentPane;
    int start = 0;
    while (true) {
        const int nl = m_pending.indexOf('\n', start);
        if (nl < 0) break;
        QByteArray line = m_pending.mid(start, nl - start);
        start = nl + 1;
        int marker = -1;
        if (parseMarker(line, &marker)) {
            pane = marker;
            continue;
        }
        line.append('\n');
        panes[pane] += line;
    }
    if (start > 0) m_pending.remove(0, start);
    m_currentPane = pane;
    for (int i = 0; i < kPaneCount; ++i) {
        if (!panes[i].isEmpty()) appendToPane(i, panes[i]);
    }
}

void TuiPanesOverlay::appendPane(int pane, const QByteArray& chunk) {
    if (pane < 0 || pane >= kPaneCount) return;
    appendToPane(pane, chunk);
}

void TuiPanesOverlay::finish(int) {
    flushPending();
    m_running = false;
}

void TuiPanesOverlay::closeOverlay() {
    emit closeRequested();
}

void TuiPanesOverlay::reset() {
    m_pending.clear();
    m_currentPane = 1;
    m_running = false;
    for (QPlainTextEdit* view : m_views) {
        if (view) view->clear();
    }
}

void TuiPanesOverlay::setPaneSizes(const QList<int>& sizes) {
    if (!m_splitter || sizes.size() < kPaneCount) return;
    m_splitter->setSizes(sizes.mid(0, kPaneCount));
}

void TuiPanesOverlay::focusCenterPane() {
    if (m_views[1]) m_views[1]->setFocus(Qt::OtherFocusReason);
}

QString TuiPanesOverlay::paneText(int pane) const {
    if (pane < 0 || pane >= kPaneCount || !m_views[pane]) return {};
    return m_views[pane]->toPlainText();
}

void TuiPanesOverlay::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        closeOverlay();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

bool TuiPanesOverlay::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) {
            closeOverlay();
            key->accept();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void TuiPanesOverlay::appendToPane(int pane, const QByteArray& chunk) {
    if (pane < 0 || pane >= kPaneCount || !m_views[pane] || chunk.isEmpty()) return;
    QPlainTextEdit* view = m_views[pane];
    QScrollBar* bar = view->verticalScrollBar();
    const bool atBottom = bar->value() >= bar->maximum();
    QTextCursor cursor(view->document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(QString::fromUtf8(chunk));
    if (atBottom) bar->setValue(bar->maximum());
}

void TuiPanesOverlay::flushPending() {
    if (m_pending.isEmpty()) return;
    const QByteArray rest = m_pending;
    m_pending.clear();
    int pane = -1;
    if (parseMarker(rest, &pane)) {
        m_currentPane = pane;
        return;
    }
    appendToPane(m_currentPane, rest);
}

bool TuiPanesOverlay::parseMarker(const QByteArray& line, int* pane) {
    const QByteArray text = line.trimmed();
    if (!text.startsWith(kPaneMarker)) return false;
    bool ok = false;
    const int index = text.mid(kPaneMarker.size()).toInt(&ok);
    if (!ok || index < 0 || index >= kPaneCount) return false;
    if (pane) *pane = index;
    return true;
}

}
