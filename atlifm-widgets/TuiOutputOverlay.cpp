#include "TuiOutputOverlay.hpp"
#include "../atlifm-theme/Theme.hpp"
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <algorithm>

namespace Widgets {

TuiOutputOverlay::TuiOutputOverlay(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("tuiOutputOverlay"));
    setAttribute(Qt::WA_StyledBackground, true);
    // 2px рамка accent и отступы лэйаута 2px, содержимое не перекрывает рамку
    setStyleSheet(QString(
        "QWidget#tuiOutputOverlay { background-color: %1; border: 2px solid %2; border-radius: 0px; }"
    ).arg(Theme::Colors::OverlayBg().name(), Theme::Colors::OverlayBorder().name()));
    setFocusPolicy(Qt::NoFocus);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(6);

    auto* header = new QHBoxLayout();
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(8);

    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("tuiOutputTitle"));
    m_title->setFont(Theme::getMonospaceFont(9, QFont::Bold));
    m_title->setStyleSheet(QString("color: %1; background: transparent;")
                               .arg(Theme::Colors::Accent().name()));
    header->addWidget(m_title, 1);

    m_close = new QPushButton("Close", this);
    m_close->setObjectName(QStringLiteral("tuiOutputCloseBtn"));
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
    connect(m_close, &QPushButton::clicked, this, [this]() { hide(); });
    header->addWidget(m_close, 0);

    layout->addLayout(header);

    m_view = new QPlainTextEdit(this);
    m_view->setObjectName(QStringLiteral("tuiOutputView"));
    m_view->setReadOnly(true);
    m_view->setFont(Theme::getMonospaceFont(9));
    // скроллбары скрыты (как в helpdialog): прокрутка колесом
    m_view->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_view->setMaximumBlockCount(5000); // кап памяти на длинных сессиях
    m_view->setStyleSheet(QString(R"(
        QPlainTextEdit {
            background-color: %1;
            color: %2;
            border: 1px solid %3;
            padding: 6px;
        }
    )").arg(Theme::Colors::Mantle().name(), Theme::Colors::Text().name(),
            Theme::Colors::Surface1().name()));
    layout->addWidget(m_view, 1);

    hide();
}

void TuiOutputOverlay::begin(const QString& title) {
    m_running = true;
    m_pending.clear();
    m_view->clear();
    m_title->setText("▸ " + title + " (running…)");
    repositionWithinParent();
    show();
    raise();
}

void TuiOutputOverlay::appendOutput(const QByteArray& chunk) {
    m_pending += chunk;
    // печатаем целые строки, остаток ждет следующий чанк
    const int nl = m_pending.lastIndexOf('\n');
    if (nl < 0) return;
    const QByteArray complete = m_pending.left(nl + 1);
    m_pending = m_pending.mid(nl + 1);

    m_view->moveCursor(QTextCursor::End);
    m_view->insertPlainText(QString::fromUtf8(complete));
    m_view->moveCursor(QTextCursor::End);
}

void TuiOutputOverlay::finish(int exitCode) {
    if (!m_pending.isEmpty()) {
        m_view->moveCursor(QTextCursor::End);
        m_view->insertPlainText(QString::fromUtf8(m_pending));
        m_pending.clear();
    }
    m_running = false;
    const QString suffix = (exitCode == 0)
        ? QStringLiteral(" [done]")
        : QStringLiteral(" [failed: exit %1]").arg(exitCode);
    QString t = m_title->text();
    t.remove(QStringLiteral(" (running…)"));
    m_title->setText(t + suffix);
}

QString TuiOutputOverlay::outputText() const {
    return m_view->toPlainText();
}

void TuiOutputOverlay::repositionWithinParent() {
    QWidget* p = parentWidget();
    if (!p) return;
    const int pw = p->width();
    const int ph = p->height();
    constexpr int kMargin = 24;
    const int w = std::min(640, std::max(360, pw * 45 / 100));
    const int h = std::min(420, std::max(220, ph * 40 / 100));
    const int x = std::max(8, pw - w - kMargin);
    const int y = std::max(8, ph - h - kMargin);
    setGeometry(x, y, w, h);
}

}
