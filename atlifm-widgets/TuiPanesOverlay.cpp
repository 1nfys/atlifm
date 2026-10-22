#include "TuiPanesOverlay.hpp"
#include "../atlifm-theme/Theme.hpp"
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTextCursor>
#include <QVBoxLayout>

namespace Widgets {

namespace {
constexpr int kPaneCount = 3;
const QByteArray kPaneMarker("@@pane:");
}

TuiPanesOverlay::TuiPanesOverlay(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("tuiPanesOverlay"));
    setAttribute(Qt::WA_StyledBackground, true);
    setFocusPolicy(Qt::StrongFocus);
    setStyleSheet(QString(
        "QWidget#tuiPanesOverlay { background-color: %1; border: 2px solid %2; border-radius: 0px; }"
    ).arg(Theme::Colors::OverlayBg().name(), Theme::Colors::OverlayBorder().name()));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(6);

    auto* header = new QHBoxLayout();
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(8);

    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("tuiPanesTitle"));
    m_title->setTextFormat(Qt::PlainText);
    m_title->setFont(Theme::getMonospaceFont(9, QFont::Bold));
    m_title->setStyleSheet(QString("color: %1; background: transparent;")
                               .arg(Theme::Colors::Accent().name()));
    header->addWidget(m_title, 1);

    auto* hint = new QLabel(QStringLiteral("Esc close"), this);
    hint->setFont(Theme::getMonospaceFont(8));
    hint->setStyleSheet(QString("color: %1; background: transparent;")
                            .arg(Theme::Colors::Dim().name()));
    header->addWidget(hint, 0);

    m_close = new QPushButton("Close", this);
    m_close->setObjectName(QStringLiteral("tuiPanesCloseBtn"));
    m_close->setFont(Theme::getMonospaceFont(8));
    m_close->setFocusPolicy(Qt::NoFocus);
    m_close->setStyleSheet(QString(R"(
        QPushButton {
            background-color: %1;
            color: %2;
            border: 1px solid %3;
            padding: 2px 10px;
        }
        QPushButton:hover {
            background-color: %3;
        }
    )").arg(Theme::Colors::Surface0().name(), Theme::Colors::Text().name(),
            Theme::Colors::Surface1().name()));
    connect(m_close, &QPushButton::clicked, this, &TuiPanesOverlay::closeOverlay);
    header->addWidget(m_close, 0);

    layout->addLayout(header);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setObjectName(QStringLiteral("tuiPanesSplitter"));
    splitter->setHandleWidth(2);
    splitter->setChildrenCollapsible(false);
    splitter->setOpaqueResize(true);

    const QString viewStyle = QString(R"(
        QPlainTextEdit {
            background-color: %1;
            color: %2;
            border: 1px solid %3;
            padding: 6px;
        }
    )").arg(Theme::Colors::Mantle().name(), Theme::Colors::Text().name(),
            Theme::Colors::Surface1().name());

    for (int i = 0; i < kPaneCount; ++i) {
        auto* view = new QPlainTextEdit(splitter);
        view->setObjectName(QStringLiteral("tuiPanesView%1").arg(i));
        view->setReadOnly(true);
        view->setFont(Theme::getMonospaceFont(9));
        view->setLineWrapMode(QPlainTextEdit::NoWrap);
        view->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        view->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        view->setMaximumBlockCount(5000);
        view->setTabStopDistance(QFontMetricsF(view->font()).horizontalAdvance(QLatin1Char(' ')) * 4);
        view->setStyleSheet(viewStyle);
        splitter->addWidget(view);
        m_views[i] = view;
    }
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    splitter->setStretchFactor(2, 1);
    splitter->setSizes({1, 1, 1});
    layout->addWidget(splitter, 1);

    hide();
}

void TuiPanesOverlay::begin(const QString& title) {
    m_running = true;
    m_pending.clear();
    m_currentPane = 1;
    for (QPlainTextEdit* view : m_views) {
        if (view) view->clear();
    }
    m_title->setText(QStringLiteral("▸ %1 (running…)").arg(title));
    repositionWithinParent();
    show();
    raise();
    if (m_views[1]) m_views[1]->setFocus(Qt::OtherFocusReason);
}

void TuiPanesOverlay::appendOutput(const QByteArray& chunk) {
    m_pending += chunk;
    int start = 0;
    while (true) {
        const int nl = m_pending.indexOf('\n', start);
        if (nl < 0) break;
        QByteArray line = m_pending.mid(start, nl - start);
        start = nl + 1;
        int pane = -1;
        if (parseMarker(line, &pane)) {
            m_currentPane = pane;
            continue;
        }
        line.append('\n');
        appendToPane(m_currentPane, line);
    }
    if (start > 0) m_pending.remove(0, start);
}

void TuiPanesOverlay::appendPane(int pane, const QByteArray& chunk) {
    if (pane < 0 || pane >= kPaneCount) return;
    appendToPane(pane, chunk);
}

void TuiPanesOverlay::finish(int exitCode) {
    flushPending();
    m_running = false;
    const QString suffix = (exitCode == 0)
        ? QStringLiteral(" [done]")
        : QStringLiteral(" [failed: exit %1]").arg(exitCode);
    QString title = m_title->text();
    title.remove(QStringLiteral(" (running…)"));
    m_title->setText(title + suffix);
}

void TuiPanesOverlay::closeOverlay() {
    hide();
    emit closed();
}

QString TuiPanesOverlay::paneText(int pane) const {
    if (pane < 0 || pane >= kPaneCount || !m_views[pane]) return {};
    return m_views[pane]->toPlainText();
}

void TuiPanesOverlay::repositionWithinParent() {
    if (QWidget* p = parentWidget()) setGeometry(p->rect());
}

void TuiPanesOverlay::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        closeOverlay();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void TuiPanesOverlay::appendToPane(int pane, const QByteArray& chunk) {
    if (pane < 0 || pane >= kPaneCount || !m_views[pane] || chunk.isEmpty()) return;
    QPlainTextEdit* view = m_views[pane];
    QTextCursor cursor = view->textCursor();
    cursor.movePosition(QTextCursor::End);
    view->setTextCursor(cursor);
    view->insertPlainText(QString::fromUtf8(chunk));
    view->ensureCursorVisible();
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
