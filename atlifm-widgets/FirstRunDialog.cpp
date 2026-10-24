#include "FirstRunDialog.hpp"

#include <QCursor>
#include <QEasingCurve>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <vector>

#include "../atlifm-theme/Theme.hpp"

namespace {

struct Dot {
    QPointF pos;
    qint64 litAt{-1};
};

const std::vector<QPointF>& route() {
    static const std::vector<QPointF> points = {
        {99.00, 968.00}, {123.21, 923.37}, {150.51, 880.56}, {177.86, 837.78},
        {205.01, 794.88}, {232.12, 751.95}, {259.03, 708.89}, {287.02, 664.36},
        {314.73, 619.65}, {342.22, 574.81}, {369.68, 529.94}, {396.89, 484.93},
        {424.61, 438.87}, {452.36, 392.84}, {479.91, 346.69}, {507.32, 300.44},
        {534.72, 254.20}, {562.13, 207.96}, {589.79, 161.88}, {627.10, 125.50},
        {642.03, 177.10}, {649.39, 230.31}, {656.66, 283.53}, {664.04, 336.74},
        {672.09, 389.84}, {680.84, 442.84}, {689.42, 495.87}, {697.88, 548.91},
        {705.69, 602.06}, {726.55, 651.47}, {736.83, 702.93}, {753.30, 752.77},
        {775.36, 800.40}, {799.81, 846.86}, {828.37, 890.90}, {870.40, 922.10},
        {876.52, 866.20}, {856.82, 812.83}, {830.71, 762.52}, {799.48, 722.14},
        {766.10, 683.72}, {726.55, 651.47},
        {687.32, 615.41}, {642.29, 586.78}, {595.99, 560.23}, {548.31, 536.26},
        {498.96, 515.94}, {448.43, 498.77}, {396.89, 484.93}, {343.53, 476.96},
        {289.72, 473.42}, {235.91, 476.40}, {183.43, 488.47}, {136.76, 515.02},
        {113.88, 562.38}, {130.69, 612.95}, {167.65, 652.06}, {211.95, 682.81},
        {259.03, 708.89}, {309.54, 724.99}, {360.44, 739.78}, {412.07, 751.77},
        {464.30, 760.81}, {516.92, 767.20}, {569.77, 771.27}, {622.76, 772.88},
        {675.77, 772.57}, {728.73, 770.20},
        {781.70, 768.20}, {830.71, 762.52}, {884.48, 753.01}, {939.00, 750.00}
    };
    return points;
}

QColor mix(const QColor& a, const QColor& b, qreal t) {
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t,
                            a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t);
}

QColor dotColor(const Dot& dot, qint64 now, const QColor& base, const QColor& lit) {
    if (dot.litAt < 0) return base;
    const qint64 elapsed = now - dot.litAt;
    QEasingCurve ease(QEasingCurve::InOutQuad);
    if (elapsed < 120) {
        return mix(base, lit, ease.valueForProgress(elapsed / 120.0));
    }
    if (elapsed < 1400) return lit;
    if (elapsed < 1850) {
        return mix(lit, base, ease.valueForProgress((elapsed - 1400) / 450.0));
    }
    return base;
}

class DotsLogo : public QWidget {
public:
    explicit DotsLogo(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedSize(170, 170);
        for (const QPointF& p : route()) {
            bool seen = false;
            for (const Dot& d : m_dots) {
                if (d.pos == p) {
                    seen = true;
                    break;
                }
            }
            if (!seen) m_dots.push_back({p, -1});
        }
        m_runner = route().front();
        m_clock.start();
        m_refresh.setInterval(16);
        connect(&m_refresh, &QTimer::timeout, this, [this]() {
            const qint64 now = m_clock.elapsed();
            if (now - m_lastStep >= 360) {
                m_lastStep = now;
                advance(now);
            }
            update();
        });
    }

protected:
    void showEvent(QShowEvent* event) override {
        QWidget::showEvent(event);
        m_clock.restart();
        m_lastStep = 0;
        m_routeIndex = 0;
        m_runner = route().front();
        for (Dot& d : m_dots) d.litAt = -1;
        advance(0);
        m_refresh.start();
    }

    void hideEvent(QHideEvent* event) override {
        m_refresh.stop();
        QWidget::hideEvent(event);
    }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        const QRectF view(76, 90, 888, 900);
        const qreal scale = std::min(width() / view.width(), height() / view.height());
        p.translate((width() - view.width() * scale) / 2 - view.x() * scale,
                    (height() - view.height() * scale) / 2 - view.y() * scale);
        p.scale(scale, scale);

        const QColor base = Theme::Colors::Accent();
        const QColor lit = Theme::Colors::Text();
        const qint64 now = m_clock.elapsed();

        p.setPen(Qt::NoPen);
        for (const Dot& dot : m_dots) {
            p.setBrush(dotColor(dot, now, base, lit));
            p.drawEllipse(dot.pos, 9.8, 9.8);
        }

        QPen pen(Qt::white);
        pen.setWidthF(std::max(2.6, 1.0 / scale));
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(m_runner, 16.5, 16.5);
    }

private:
    void advance(qint64 now) {
        const auto& points = route();
        if (m_routeIndex >= static_cast<int>(points.size())) m_routeIndex = 0;
        m_runner = points[m_routeIndex];
        for (Dot& dot : m_dots) {
            if (dot.pos == m_runner) dot.litAt = now;
        }
        ++m_routeIndex;
    }

    std::vector<Dot> m_dots;
    QPointF m_runner;
    QElapsedTimer m_clock;
    QTimer m_refresh;
    qint64 m_lastStep{0};
    int m_routeIndex{0};
};

}

namespace Widgets {

FirstRunDialog::FirstRunDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("firstRunDialog"));
    setAttribute(Qt::WA_StyledBackground, true);
    setWindowTitle(QStringLiteral("atlifm"));
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setFixedSize(560, 420);

    const QColor bg = Theme::Colors::AccentFg();
    const QColor fg = Theme::Colors::Accent();
    const QColor hover = fg.lighter(115);

    setStyleSheet(QStringLiteral(
        "QDialog#firstRunDialog { background-color: %1; border: 2px solid %2; }"
        "QLabel { background: transparent; }"
        "QLabel#firstRunTitle { color: %2; font-size: 20px; font-weight: 600; }"
        "QLabel#firstRunHint { color: rgba(%3, %4, %5, %6); font-size: 12px; }"
        "QPushButton { background-color: transparent; color: %2; border: 1px solid %2;"
        " font-size: 14px; padding: 0 18px; }"
        "QPushButton:hover { background-color: rgba(%3, %4, %5, 38); }"
        "QPushButton#firstRunYes { background-color: %2; color: %1; }"
        "QPushButton#firstRunYes:hover { background-color: %7; }"
    ).arg(bg.name(), fg.name())
     .arg(fg.red()).arg(fg.green()).arg(fg.blue()).arg(150)
     .arg(hover.name()));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(32, 24, 32, 22);
    layout->setSpacing(0);

    layout->addStretch(1);
    layout->addWidget(new DotsLogo(this), 0, Qt::AlignHCenter);
    layout->addSpacing(10);

    auto* title = new QLabel(QStringLiteral("want to set atlifm as default file manager?"), this);
    title->setObjectName(QStringLiteral("firstRunTitle"));
    title->setAlignment(Qt::AlignCenter);
    title->setWordWrap(true);
    layout->addWidget(title);
    layout->addSpacing(22);

    auto* buttons = new QHBoxLayout();
    buttons->setSpacing(12);
    buttons->addStretch(1);

    auto* yes = new QPushButton(QStringLiteral("yes"), this);
    yes->setObjectName(QStringLiteral("firstRunYes"));
    yes->setFixedSize(120, 38);
    yes->setCursor(Qt::PointingHandCursor);
    yes->setAutoDefault(true);
    yes->setDefault(true);
    buttons->addWidget(yes);

    auto* no = new QPushButton(QStringLiteral("no"), this);
    no->setObjectName(QStringLiteral("firstRunNo"));
    no->setFixedSize(120, 38);
    no->setCursor(Qt::PointingHandCursor);
    no->setAutoDefault(false);
    buttons->addWidget(no);
    buttons->addStretch(1);
    layout->addLayout(buttons);
    layout->addSpacing(18);

    auto* hintLabel = new QLabel(
        QStringLiteral("you can set it in any time by atlifm --setdefault"), this);
    hintLabel->setObjectName(QStringLiteral("firstRunHint"));
    hintLabel->setAlignment(Qt::AlignCenter);
    hintLabel->setWordWrap(true);
    layout->addWidget(hintLabel);
    layout->addStretch(1);

    connect(yes, &QPushButton::clicked, this, &QDialog::accept);
    connect(no, &QPushButton::clicked, this, &QDialog::reject);
    yes->setFocus();

    QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (screen) move(screen->availableGeometry().center() - rect().center());
}

}
