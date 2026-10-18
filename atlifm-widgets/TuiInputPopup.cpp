#include "TuiInputPopup.hpp"
#include "StatusBar.hpp"
#include "../atlifm-theme/Theme.hpp"
#include <QKeyEvent>
#include <QApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QFileInfo>
#include <QEventLoop>

namespace Widgets {

// режим одно окно: вводы и подтверждения идут инлайн в нижнюю статусную строку главного окна
static TuiStatusBar* locateStatusBar(QWidget* parent) {
    QWidget* win = nullptr;
    if (parent) win = parent->window();
    if (!win) win = qApp ? qApp->activeWindow() : nullptr;
    if (win) {
        if (auto* sb = win->findChild<TuiStatusBar*>()) return sb;
    }
    if (qApp) {
        for (QWidget* w : qApp->topLevelWidgets()) {
            if (w && w->isWindow()) {
                if (auto* sb = w->findChild<TuiStatusBar*>()) return sb;
            }
        }
    }
    return nullptr;
}

TuiInputPopup::TuiInputPopup(const QString& /*title*/, const QString& labelText,
                             const QString& initialText, QWidget* parent,
                             bool selectStemOnly, bool isPassword)
    : QDialog(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint) {
    setAttribute(Qt::WA_DeleteOnClose, false);
    setFixedWidth(320);

    using namespace Theme::Colors;

    setStyleSheet(QString(
        "QDialog { background-color: %1; border: 1px solid %2; border-radius: 0px; }"
    ).arg(Mantle().name(), Accent().name()));

    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(12, 10, 12, 10);
    mainLayout->setSpacing(8);

        auto label = new QLabel(labelText, this);
    label->setFont(Theme::getMonospaceFont(9, QFont::Bold));
    label->setStyleSheet(QString("QLabel { color: %1; border: none; }").arg(Text().name()));
    mainLayout->addWidget(label);

        m_lineEdit = new QLineEdit(initialText, this);
    if (isPassword) m_lineEdit->setEchoMode(QLineEdit::Password);
    m_lineEdit->setFont(Theme::getMonospaceFont(9));
    m_lineEdit->setStyleSheet(QString(R"(
        QLineEdit {
            background-color: %1;
            color: %2;
            border: 1px solid %3;
            border-radius: 0px;
            padding: 4px 6px;
            selection-background-color: %4;
            selection-color: %5;
        }
        QLineEdit:focus {
            border: 1px solid %4;
        }
    )").arg(Base().name(), Text().name(), Surface1().name(), Accent().name(), AccentFg().name()));
    mainLayout->addWidget(m_lineEdit);

    auto btnLayout = new QHBoxLayout();
    btnLayout->setSpacing(6);
    btnLayout->addStretch(1);

    m_btnCancel = new QPushButton("Cancel", this);
    m_btnCancel->setFont(Theme::getMonospaceFont(9));
    m_btnCancel->setCursor(Qt::PointingHandCursor);
    m_btnCancel->setStyleSheet(QString(R"(
        QPushButton {
            background-color: %1;
            color: %2;
            border: 1px solid %3;
            border-radius: 0px;
            padding: 4px 14px;
            font-weight: bold;
        }
        QPushButton:hover {
            background-color: %3;
        }
    )").arg(Surface0().name(), Text().name(), Surface1().name()));
    connect(m_btnCancel, &QPushButton::clicked, this, &QDialog::reject);
    btnLayout->addWidget(m_btnCancel);

    m_btnOk = new QPushButton("OK", this);
    m_btnOk->setFont(Theme::getMonospaceFont(9, QFont::Bold));
    m_btnOk->setCursor(Qt::PointingHandCursor);
    m_btnOk->setStyleSheet(QString(R"(
        QPushButton {
            background-color: %1;
            color: %2;
            border: none;
            border-radius: 0px;
            padding: 4px 16px;
            font-weight: bold;
        }
        QPushButton:hover {
            background-color: %1;
            border: 1px solid %2;
        }
    )").arg(Accent().name(), AccentFg().name()));
    connect(m_btnOk, &QPushButton::clicked, this, &QDialog::accept);
    btnLayout->addWidget(m_btnOk);

    mainLayout->addLayout(btnLayout);

    connect(m_lineEdit, &QLineEdit::returnPressed, this, &QDialog::accept);

        if (selectStemOnly && !initialText.isEmpty()) {
        QFileInfo fi(initialText);
        QString stem = fi.completeBaseName();
        if (!stem.isEmpty() && stem.length() < initialText.length()) {
            m_lineEdit->setSelection(0, stem.length());
        } else {
            m_lineEdit->selectAll();
        }
    } else {
        m_lineEdit->selectAll();
    }
}

int TuiInputPopup::exec() {
    setWindowModality(Qt::NonModal);
    show();
    raise();
    activateWindow();
    // возвращаем реальный результат QDialog (Accepted/Rejected), а не код QEventLoop:
    // quit() всегда даёт 0 = Rejected, и все вводы «отменялись»
    int res = QDialog::Rejected;
    QEventLoop loop;
    connect(this, &QDialog::finished, &loop, [&](int r) {
        res = r;
        loop.quit();
    });
    loop.exec();
    return res;
}

QString TuiInputPopup::text() const {
    return m_lineEdit ? m_lineEdit->text() : QString();
}

void TuiInputPopup::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        reject();
        return;
    }
    QDialog::keyPressEvent(event);
}

QString TuiInputPopup::getText(QWidget* parent, const QString& title, const QString& labelText,
                               const QString& initialText, bool* ok,
                               const QPoint& targetPos, bool selectStemOnly, bool isPassword) {
    Q_UNUSED(title);
    // одно окно: инлайн в статусбаре главного окна
    if (TuiStatusBar* sb = locateStatusBar(parent)) {
        return sb->askTextInline(labelText, initialText, selectStemOnly, ok, isPassword);
    }
    // фолбэк, если главного окна нет: отдельный попап
    TuiInputPopup dlg(title, labelText, initialText, parent, selectStemOnly, isPassword);

    dlg.adjustSize();
    QPoint pos = targetPos;
    if (pos.isNull() && parent) {
        pos = parent->mapToGlobal(parent->rect().center()) - QPoint(dlg.width() / 2, dlg.height() / 2);
    }

    if (!pos.isNull()) {
        QScreen* screen = QGuiApplication::screenAt(pos);
        if (!screen) screen = QGuiApplication::primaryScreen();
        if (screen) {
            QRect geom = screen->availableGeometry();
            pos.setX(std::clamp(pos.x(), geom.left() + 4, geom.right() - dlg.width() - 4));
            pos.setY(std::clamp(pos.y(), geom.top() + 4, geom.bottom() - dlg.height() - 4));
        }
        dlg.move(pos);
    }

    dlg.m_lineEdit->setFocus();
    int result = dlg.exec();
    if (ok) {
        *ok = (result == QDialog::Accepted);
    }
    return (result == QDialog::Accepted) ? dlg.text() : QString();
}

// TuiConfirmPopup

TuiConfirmPopup::TuiConfirmPopup(const QString& title, const QString& message,
                                 const QString& confirmText, const QString& cancelText,
                                 bool isDanger, QWidget* parent)
    : QDialog(parent, Qt::Dialog | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint) {
    setAttribute(Qt::WA_DeleteOnClose, false);
    setFixedWidth(420);

    using namespace Theme::Colors;

    QString borderColor = isDanger ? Red().name() : Accent().name();
    setStyleSheet(QString(
        "QDialog { background-color: %1; border: 1px solid %2; border-radius: 0px; }"
    ).arg(Mantle().name(), borderColor));

    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(16, 14, 16, 14);
    mainLayout->setSpacing(10);

    auto titleLabel = new QLabel(title, this);
    titleLabel->setFont(Theme::getMonospaceFont(10, QFont::Bold));
    titleLabel->setStyleSheet(QString("color: %1; border: none; font-weight: bold;").arg(borderColor));
    mainLayout->addWidget(titleLabel);

    auto msgLabel = new QLabel(message, this);
    msgLabel->setFont(Theme::getMonospaceFont(9));
    msgLabel->setWordWrap(true);
    msgLabel->setStyleSheet(QString("color: %1; border: none; line-height: 1.4;").arg(Text().name()));
    mainLayout->addWidget(msgLabel);

    auto btnLayout = new QHBoxLayout();
    btnLayout->setSpacing(8);
    btnLayout->addStretch(1);

    if (!cancelText.isEmpty()) {
        m_btnCancel = new QPushButton(cancelText, this);
        m_btnCancel->setFont(Theme::getMonospaceFont(9));
        m_btnCancel->setCursor(Qt::PointingHandCursor);
        m_btnCancel->setStyleSheet(QString(R"(
            QPushButton {
                background-color: %1;
                color: %2;
                border: 1px solid %3;
                border-radius: 0px;
                padding: 5px 16px;
                font-weight: bold;
            }
            QPushButton:hover {
                background-color: %3;
            }
        )").arg(Surface0().name(), Text().name(), Surface1().name()));
        connect(m_btnCancel, &QPushButton::clicked, this, &QDialog::reject);
        btnLayout->addWidget(m_btnCancel);
    }

    m_btnConfirm = new QPushButton(confirmText, this);
    m_btnConfirm->setFont(Theme::getMonospaceFont(9, QFont::Bold));
    m_btnConfirm->setCursor(Qt::PointingHandCursor);
    QString confirmBg = isDanger ? Red().name() : Accent().name();
    QString confirmFg = isDanger ? Base().name() : AccentFg().name();
    m_btnConfirm->setStyleSheet(QString(R"(
        QPushButton {
            background-color: %1;
            color: %2;
            border: none;
            border-radius: 0px;
            padding: 5px 18px;
            font-weight: bold;
        }
        QPushButton:hover {
            opacity: 0.9;
        }
    )").arg(confirmBg, confirmFg));
    connect(m_btnConfirm, &QPushButton::clicked, this, &QDialog::accept);
    btnLayout->addWidget(m_btnConfirm);

    mainLayout->addLayout(btnLayout);

    if (m_btnCancel) {
        m_btnCancel->setFocus();
    } else {
        m_btnConfirm->setFocus();
    }
}

int TuiConfirmPopup::exec() {
    setWindowModality(Qt::NonModal);
    if (parentWidget()) {
        move(parentWidget()->mapToGlobal(parentWidget()->rect().center()) - QPoint(width() / 2, height() / 2));
    }
    show();
    raise();
    activateWindow();
    int res = QDialog::Rejected;
    QEventLoop loop;
    connect(this, &QDialog::finished, &loop, [&](int r) {
        res = r;
        loop.quit();
    });
    loop.exec();
    return res;
}

void TuiConfirmPopup::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        reject();
        return;
    }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        accept();
        return;
    }
    QDialog::keyPressEvent(event);
}

bool TuiConfirmPopup::ask(QWidget* parent, const QString& title, const QString& message,
                          const QString& confirmText, const QString& cancelText,
                          bool isDanger) {
    // одно окно: инлайн в статусбаре главного окна
    if (TuiStatusBar* sb = locateStatusBar(parent)) {
        return sb->askConfirmInline(title, message, confirmText, cancelText, isDanger);
    }
    TuiConfirmPopup dlg(title, message, confirmText, cancelText, isDanger, parent);
    dlg.adjustSize();
    if (parent) {
        dlg.move(parent->mapToGlobal(parent->rect().center()) - QPoint(dlg.width() / 2, dlg.height() / 2));
    }
    return dlg.exec() == QDialog::Accepted;
}

void TuiConfirmPopup::alert(QWidget* parent, const QString& title, const QString& message) {
    // одно окно: инлайн в статусбаре главного окна
    if (TuiStatusBar* sb = locateStatusBar(parent)) {
        sb->showAlertInline(title, message);
        return;
    }
    TuiConfirmPopup dlg(title, message, "OK", "", false, parent);
    dlg.adjustSize();
    if (parent) {
        dlg.move(parent->mapToGlobal(parent->rect().center()) - QPoint(dlg.width() / 2, dlg.height() / 2));
    }
    dlg.exec();
}

}
