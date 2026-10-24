#include "OpenWithDialog.hpp"
#include "../atlifm-theme/Theme.hpp"
#include "../atlifm-core/Icons.hpp"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QIcon>
#include <QPixmap>
#include <QFrame>
#include <QPainter>
#include <QHBoxLayout>
#include <QSet>
#include <algorithm>

namespace Widgets {

namespace {

// заголовок секции как в propertiesdialog: только название, без рамок и линеек
QLabel* makeSectionHeader(const QString& title, QWidget* parent) {
    auto* lbl = new QLabel(parent);
    lbl->setTextFormat(Qt::RichText);
    lbl->setText(QStringLiteral(
        "<span style=\"color:%1; font-weight:bold;\">%2</span>")
        .arg(Theme::Colors::Accent().name(), title.toHtmlEscaped()));
    lbl->setFont(Theme::getMonospaceFont(9, QFont::Bold));
    return lbl;
}

QString rowStyle(bool selected) {
    using namespace Theme::Colors;
    if (selected) {
        return QStringLiteral(
            "QWidget#appRow { background-color: %1; }"
            "QWidget#appRow QLabel { color: %2; }")
            .arg(SelectionBg().name(), SelectionFg().name());
    }
    return QStringLiteral(
        "QWidget#appRow { background-color: transparent; }"
        "QWidget#appRow QLabel { color: %1; }")
        .arg(Text().name());
}

}

OpenWithDialog::OpenWithDialog(QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("openWithPage"));
    setupUi();
}

void OpenWithDialog::setupUi() {
    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(14, 12, 14, 12);
    mainLayout->setSpacing(8);

    m_headerLabel = makeSectionHeader(QStringLiteral("OPEN WITH"), this);
    mainLayout->addWidget(m_headerLabel);

    m_fileLabel = new QLabel(this);
    m_fileLabel->setFont(Theme::getMonospaceFont(9));
    m_fileLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    mainLayout->addWidget(m_fileLabel);

    // поиск по списку приложений как поле фильтра в строке статуса
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText("Search apps...");
    m_searchEdit->setFont(Theme::getMonospaceFont(9));
    m_searchEdit->setFrame(false);
    m_searchEdit->installEventFilter(this);
    connect(m_searchEdit, &QLineEdit::textChanged, this, [this]() { rebuildList(); });
    mainLayout->addWidget(m_searchEdit);

    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_listContainer = new QWidget(m_scroll);
    m_listLayout = new QVBoxLayout(m_listContainer);
    m_listLayout->setContentsMargins(0, 0, 0, 0);
    m_listLayout->setSpacing(2);
    m_scroll->setWidget(m_listContainer);
    mainLayout->addWidget(m_scroll, 1);

    m_alwaysUse = new QCheckBox(QStringLiteral("[ ] Always use for this file type"), this);
    m_alwaysUse->setFont(Theme::getMonospaceFont(9));
    m_alwaysUse->setCursor(Qt::PointingHandCursor);
    connect(m_alwaysUse, &QCheckBox::toggled, this, [this](bool checked) {
        m_alwaysUse->setText(checked ? QStringLiteral("[x] Always use for this file type")
                                     : QStringLiteral("[ ] Always use for this file type"));
    });
    mainLayout->addWidget(m_alwaysUse);

    auto* btnLayout = new QHBoxLayout();
    btnLayout->addStretch();
    m_cancelBtn = new QPushButton("Cancel", this);
    m_openBtn = new QPushButton("Open", this);
    m_cancelBtn->setObjectName(QStringLiteral("openWithCancelBtn"));
    m_openBtn->setObjectName(QStringLiteral("openWithOpenBtn"));
    m_cancelBtn->setAutoDefault(false);
    m_openBtn->setAutoDefault(false);
    connect(m_cancelBtn, &QPushButton::clicked, this, [this]() { emit closeRequested(); });
    connect(m_openBtn, &QPushButton::clicked, this, [this]() { activateSelected(); });
    btnLayout->addWidget(m_cancelBtn);
    btnLayout->addWidget(m_openBtn);
    mainLayout->addLayout(btnLayout);

    updateTheme();
}

void OpenWithDialog::updateTheme() {
    using namespace Theme::Colors;

    if (m_headerLabel) {
        m_headerLabel->setText(QStringLiteral(
            "<span style=\"color:%1; font-weight:bold;\">OPEN WITH</span>")
            .arg(Accent().name()));
    }
    if (m_fileLabel) {
        m_fileLabel->setStyleSheet(QString("color: %1;").arg(Dim().name()));
    }
    if (m_searchEdit) {
        m_searchEdit->setStyleSheet(QString(R"(
            QLineEdit {
                background-color: transparent;
                color: %1;
                border: none;
                border-bottom: 1px solid %2;
                padding: 1px 4px;
                selection-background-color: %2;
                selection-color: %3;
            }
            QLineEdit:focus {
                border: none;
                border-bottom: 1px solid %2;
            }
        )").arg(InputFg().name(), SelectionBg().name(), SelectionFg().name()));
    }
    if (m_openBtn) {
        // метрики идентичны cancel (border 1px + padding 3px 14px), иначе кнопки разной высоты
        m_openBtn->setStyleSheet(QString(R"(
            QPushButton {
                background-color: %1;
                color: %2;
                border: 1px solid transparent;
                border-radius: 0px;
                padding: 3px 14px;
                font-weight: bold;
            }
            QPushButton:hover { background-color: %3; }
        )").arg(Accent().name(), AccentFg().name(), Sapphire().name()));
    }
    if (m_cancelBtn) {
        m_cancelBtn->setStyleSheet(QString(R"(
            QPushButton {
                background-color: %1;
                color: %2;
                border: 1px solid %3;
                border-radius: 0px;
                padding: 3px 14px;
            }
        )").arg(Surface0().name(), Text().name(), Surface1().name()));
    }
    if (m_alwaysUse) {
        // чекбокс в стиле tui: нативный индикатор скрыт, текст несёт [ ]/[x]
        m_alwaysUse->setStyleSheet(QString(R"(
            QCheckBox { color: %1; }
            QCheckBox::indicator { width: 0px; height: 0px; border: none; background: transparent; }
        )").arg(Subtext().name()));
    }
    for (int i = 0; i < int(m_rows.size()); ++i) {
        m_rows[i].widget->setStyleSheet(rowStyle(i == m_selected));
    }
}

void OpenWithDialog::setFile(const std::filesystem::path& path) {
    m_path = path;
    QString fname = QString::fromUtf8(path.filename().c_str());
    if (fname.isEmpty()) fname = QString::fromUtf8(path.c_str());
    m_fileLabel->setText(fname);

    m_apps = Core::XdgUtils::getAppsForFile(path);
    // сразу все установленные программы, без отдельного пункта с папкой .desktop
    QSet<QString> ids;
    QSet<QString> names;
    for (const auto& app : m_apps) {
        ids.insert(app.id);
        names.insert(app.name);
    }
    for (auto& app : Core::XdgUtils::getAllApps()) {
        if (ids.contains(app.id) || names.contains(app.name)) continue;
        ids.insert(app.id);
        names.insert(app.name);
        m_apps.push_back(std::move(app));
    }
    std::sort(m_apps.begin(), m_apps.end(), [](const Core::DesktopApp& a, const Core::DesktopApp& b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    m_defaultApp = Core::XdgUtils::getDefaultAppForFile(path);

    if (m_searchEdit) {
        m_searchEdit->blockSignals(true);
        m_searchEdit->clear();
        m_searchEdit->blockSignals(false);
    }
    if (m_alwaysUse) m_alwaysUse->setChecked(false);

    rebuildList();
    m_searchEdit->setFocus();
}

void OpenWithDialog::rebuildList() {
    // полная перестройка, список короткий, без устаревших состояний
    while (QLayoutItem* item = m_listLayout->takeAt(0)) {
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }
    m_rows.clear();
    m_selected = -1;

    const QString query = m_searchEdit ? m_searchEdit->text().trimmed() : QString();
    auto matches = [&query](const Core::DesktopApp& app) {
        return query.isEmpty() || app.name.contains(query, Qt::CaseInsensitive);
    };

    bool addedDefault = false;
    if (!m_defaultApp.id.isEmpty() && matches(m_defaultApp)) {
        m_listLayout->addWidget(makeSectionHeader(QStringLiteral("DEFAULT APP"), m_listContainer));
        addRow(m_defaultApp, true);
        addedDefault = true;
    }

    int otherCount = 0;
    for (const auto& app : m_apps) {
        if (app.id == m_defaultApp.id) continue;
        if (!matches(app)) continue;
        if (otherCount == 0) {
            if (addedDefault) m_listLayout->addSpacing(6);
            m_listLayout->addWidget(makeSectionHeader(QStringLiteral("OTHER APPS"), m_listContainer));
        }
        addRow(app, false);
        ++otherCount;
    }

    if (!addedDefault && otherCount == 0) {
        auto* none = new QLabel(query.isEmpty() ? QStringLiteral("No applications found")
                                                 : QStringLiteral("No matches"), m_listContainer);
        none->setFont(Theme::getMonospaceFont(9));
        none->setStyleSheet(QString("color: %1; padding: 6px;").arg(Theme::Colors::Dim().name()));
        m_listLayout->addWidget(none);
    }

    m_listLayout->addStretch(1);

    if (!m_rows.empty()) setSelected(0);
}

void OpenWithDialog::addRow(const Core::DesktopApp& app, bool isDefault) {
    AppRow row;
    row.app = app;
    row.isDefault = isDefault;
    row.widget = new QWidget(m_listContainer);
    row.widget->setObjectName(QStringLiteral("appRow"));
    row.widget->setAttribute(Qt::WA_StyledBackground, true);
    row.widget->setCursor(Qt::PointingHandCursor);
    row.widget->setStyleSheet(rowStyle(false));
    row.widget->installEventFilter(this);

    auto* h = new QHBoxLayout(row.widget);
    h->setContentsMargins(6, 3, 6, 3);
    h->setSpacing(8);

    auto* iconLabel = new QLabel(row.widget);
    QIcon ic = QIcon::fromTheme(app.iconName);
    if (ic.isNull()) ic = QIcon::fromTheme(QStringLiteral("application-x-executable"));
    if (!ic.isNull()) {
        iconLabel->setPixmap(ic.pixmap(16, 16));
    } else {
        // без темы иконок (qt_qpa_platformtheme выключен) fromtheme всегда пустой:
        // рисуем глиф nerd font чтобы строки не оставались без иконки
        iconLabel->setText(Icons::Glyph::Executable);
        iconLabel->setAlignment(Qt::AlignCenter);
        iconLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::Colors::Dim().name()));
    }
    iconLabel->setFixedSize(18, 18);
    h->addWidget(iconLabel);

    QString name = app.name;
    if (isDefault) name += QStringLiteral("  (default)");
    auto* nameLabel = new QLabel(name.toHtmlEscaped(), row.widget);
    nameLabel->setFont(Theme::getMonospaceFont(9));
    h->addWidget(nameLabel, 1);

    m_listLayout->addWidget(row.widget);
    m_rows.push_back(row);
}

void OpenWithDialog::setSelected(int index) {
    if (index < 0 || index >= int(m_rows.size())) return;
    m_selected = index;
    for (int i = 0; i < int(m_rows.size()); ++i) {
        m_rows[i].widget->setStyleSheet(rowStyle(i == m_selected));
    }
    m_scroll->ensureWidgetVisible(m_rows[m_selected].widget);
}

void OpenWithDialog::moveSelection(int delta) {
    if (m_rows.empty()) return;
    int next = m_selected + delta;
    if (next < 0) next = int(m_rows.size()) - 1;
    if (next >= int(m_rows.size())) next = 0;
    setSelected(next);
}

void OpenWithDialog::activateSelected() {
    if (m_selected < 0 || m_selected >= int(m_rows.size())) return;
    launch(m_rows[m_selected].app);
}

void OpenWithDialog::launch(const Core::DesktopApp& app) {
    if (m_path.empty() || app.execCommand.isEmpty()) return;
    if (m_alwaysUse && m_alwaysUse->isChecked()) {
        // ошибку записи не показываем модально: запуск файла важнее пометки
        QString err;
        Core::XdgUtils::setDefaultAppForFile(m_path, app, err);
    }
    Core::XdgUtils::openWithApp(app, m_path);
    emit closeRequested();
}

bool OpenWithDialog::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_searchEdit) {
        if (event->type() == QEvent::KeyPress) {
            auto* ke = static_cast<QKeyEvent*>(event);
            switch (ke->key()) {
            case Qt::Key_Down: moveSelection(1); return true;
            case Qt::Key_Up: moveSelection(-1); return true;
            case Qt::Key_Return:
            case Qt::Key_Enter: activateSelected(); return true;
            case Qt::Key_Escape: emit closeRequested(); return true;
            default: break;
            }
        }
        return QWidget::eventFilter(watched, event);
    }

    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick) {
        for (int i = 0; i < int(m_rows.size()); ++i) {
            if (m_rows[i].widget != watched) continue;
            setSelected(i);
            if (event->type() == QEvent::MouseButtonDblClick) activateSelected();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void OpenWithDialog::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
    case Qt::Key_Down: moveSelection(1); return;
    case Qt::Key_Up: moveSelection(-1); return;
    case Qt::Key_Return:
    case Qt::Key_Enter: activateSelected(); return;
    case Qt::Key_Escape: emit closeRequested(); return;
    default: break;
    }
    QWidget::keyPressEvent(event);
}

}
