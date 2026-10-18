#include "HelpDialog.hpp"
#include "NativePlugins.hpp"
#include "../atlifm-theme/Theme.hpp"
#include "../atlifm-core/Keybinds.hpp"
#include "../atlifm-core/Plugins.hpp"
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QKeyEvent>
#include <QFontMetrics>
#include <QMenu>
#include <QAction>
#include <QSet>
#include <algorithm>

namespace Widgets {

namespace {

QString keyDisplayOf(const QStringList& keys) {
    QStringList pretty;
    pretty.reserve(keys.size());
    for (const QString& k : keys)
        pretty.append(QKeySequence(k).toString(QKeySequence::NativeText));
    return pretty.join(QStringLiteral(" / "));
}

// секция → статические заметки, жесты и режимы списка без QShortcut
const QHash<QString, QStringList>& sectionExtras() {
    static const QHash<QString, QStringList> extras = {
        {"SEARCH & FILTER", {
            "Type-as-you-go    : Just start typing letters to fuzzy search",
            "Enter             : Keep filter for this folder ([Filter] badge)",
            "\"▾\" by the filter : Saved filters for the folder + Reset",
        }},
        {"FILE OPERATIONS", {
            "Space             : Toggle item selection and advance cursor",
            "Open With...      Right-click a file: search apps, Default/Other, Always use, Choose application…",
        }},
    };
    return extras;
}

const QStringList& mouseRows() {
    static const QStringList rows = {
        "Double Click      : Open folder / launch program or file",
        "Right Click (ПКМ) : Terminal context menu (file or empty folder space)",
        "Mouse Drag Select : Drag on empty space or gutter to draw selection box",
        "Drag & Drop       : Drag selected files to any folder, bookmark, or tab",
        "Tab Bar DND       : Hover over another tab while dragging to switch",
        "Side Buttons      : Back / Forward history navigation",
        "Middle Click      : Close tab (on Tab Bar) / Open in new tab (on Bookmark)",
    };
    return rows;
}

}

HelpDialog::HelpDialog(QWidget* parent)
    : QWidget(parent) {
    setFont(Theme::getMonospaceFont(9));
    // захват клавиш через keyPressEvent, фокус обязателен
    setFocusPolicy(Qt::StrongFocus);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 12);
    layout->setSpacing(8);

    auto* title = new QLabel("atlifm Cheat Sheet: Keyboard & Mouse Guide", this);
    title->setFont(Theme::getMonospaceFont(11, QFont::Bold));
    title->setStyleSheet(QString("color: %1;").arg(Theme::Colors::Accent().name()));
    layout->addWidget(title);

    m_captureHint = new QLabel(this);
    m_captureHint->setObjectName(QStringLiteral("helpCaptureHint"));
    m_captureHint->setFont(Theme::getMonospaceFont(9, QFont::Bold));
    m_captureHint->setWordWrap(true);
    m_captureHint->setStyleSheet(
        QString("color: %1; background-color: %2; padding: 6px 8px;")
            .arg(Theme::Colors::AccentFg().name(), Theme::Colors::Accent().name()));
    m_captureHint->hide();
    layout->addWidget(m_captureHint);

    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    // скроллбары выключены, скролл колесом
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setStyleSheet(QString("QScrollArea { background-color: transparent; border: none; }"
                                    "QScrollArea > QWidget > QWidget { background-color: transparent; }"));
    layout->addWidget(m_scroll, 1);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("helpStatus"));
    m_status->setFont(Theme::getMonospaceFont(8));
    m_status->setStyleSheet(QString("color: %1;").arg(Theme::Colors::Dim().name()));
    layout->addWidget(m_status);

    auto* footer = new QLabel(
        QString("Click a key to rebind (Backspace: disable, Esc: cancel) · "
                "Right-click a key: reset to default / disable\n"
                "Config: %1")
            .arg(Core::Keybinds::configPath()),
        this);
    footer->setFont(Theme::getMonospaceFont(8));
    footer->setStyleSheet(QString("color: %1;").arg(Theme::Colors::Dim().name()));
    layout->addWidget(footer);

    auto* closeBtn = new QPushButton("Close (Esc)", this);
    closeBtn->setFont(Theme::getMonospaceFont(9));
    closeBtn->setStyleSheet(QString(R"(
        QPushButton {
            background-color: %1;
            color: %2;
            border: 1px solid %3;
            padding: 6px 16px;
        }
        QPushButton:hover {
            background-color: %3;
        }
    )").arg(Theme::Colors::Surface0().name(), Theme::Colors::Text().name(),
            Theme::Colors::Surface1().name()));
    connect(closeBtn, &QPushButton::clicked, this, &HelpDialog::closeRequested);
    layout->addWidget(closeBtn, 0, Qt::AlignRight);

    rebuild();
}

QString HelpDialog::actionLabel(const QString& actionId) {
    for (const Core::Keybinds::Action& a : Core::Keybinds::actionTable())
        if (a.id == actionId) return a.label;
    const auto pluginActions = Core::Keybinds::acceptedPluginActions();
    if (pluginActions.contains(actionId)) {
        const auto plugin = Core::Plugins::findById(pluginActions.value(actionId));
        return plugin.name.isEmpty() ? actionId : plugin.name;
    }
    return actionId;
}

QString HelpDialog::keyDisplay(const QStringList& keys) const {
    return keyDisplayOf(keys);
}

QString HelpDialog::conflictForAction(const QString& actionId, const QString& key) const {
    for (const Core::Keybinds::Action& a : Core::Keybinds::actionTable()) {
        if (a.id == actionId) continue;
        if (Core::Keybinds::keysFor(a.id).contains(key)) return a.id;
    }
    const auto pluginActions = Core::Keybinds::acceptedPluginActions();
    for (auto it = pluginActions.constBegin(); it != pluginActions.constEnd(); ++it) {
        if (it.key() == actionId) continue;
        if (Core::Keybinds::keysFor(it.key()).contains(key)) return it.key();
    }
    return {};
}

void HelpDialog::rebuild() {
    if (!m_scroll) return;

    if (m_content) {
        // takeWidget отвязывает старый контент, удаление отложенное:
        // rebuild может вызываться из слота кнопки этого же контента
        QWidget* old = m_scroll->takeWidget();
        if (old) old->deleteLater();
    }
    m_content = new QWidget;
    m_contentLayout = new QVBoxLayout(m_content);
    m_contentLayout->setContentsMargins(0, 0, 8, 0);
    m_contentLayout->setSpacing(2);
    m_scroll->setWidget(m_content);

    const auto& table = Core::Keybinds::actionTable();
    const auto pluginActions = Core::Keybinds::acceptedPluginActions();
    QStringList pluginActionIds = pluginActions.keys();
    pluginActionIds.sort();

    // колонка клавиш одинаковой ширины во всех секциях (моношрифт)
    int maxChars = 0;
    for (const auto& a : table)
        maxChars = std::max(maxChars, static_cast<int>(keyDisplay(Core::Keybinds::keysFor(a.id)).size()));
    for (const QString& id : pluginActionIds)
        maxChars = std::max(maxChars, static_cast<int>(keyDisplay(Core::Keybinds::keysFor(id)).size()));
    const QFontMetrics fm(font());
    const int keyColumnWidth =
        std::clamp(fm.horizontalAdvance(QString(std::max(maxChars, 14) + 2, QLatin1Char('0'))), 150, 460);

    addSectionHeader(QStringLiteral("MOUSE INTERACTIONS"));
    for (const QString& row : mouseRows()) addStaticRow(row);

    // действия из таблицы по секциям
    QString currentSection;
    for (const auto& a : table) {
        if (a.section != currentSection) {
            currentSection = a.section;
            addSectionHeader(currentSection);
        }
        addActionRow(a.id, a.label, keyColumnWidth);

        // статические заметки секции сразу после её действий
        const auto it = sectionExtras().constFind(currentSection);
        if (it != sectionExtras().constEnd() && (&a == &table.back() || table[&a - &table.front() + 1].section != currentSection)) {
            for (const QString& extra : *it) addStaticRow(extra);
        }
    }

    // плагины
    bool pluginsHeader = false;
    for (const QString& id : pluginActionIds) {
        if (!pluginsHeader) {
            addSectionHeader(QStringLiteral("PLUGINS"));
            pluginsHeader = true;
        }
        QString label = NativePluginHost::instance().actionDisplayName(id);
        if (label.isEmpty()) {
            const auto plugin = Core::Plugins::findById(pluginActions.value(id));
            label = plugin.name.isEmpty() ? id : plugin.name;
        }
        addActionRow(id, QString("Plugin: %1").arg(label), keyColumnWidth);
    }
    for (const auto& p : Core::Plugins::load()) {
        if (p.native || p.overrides.isEmpty()) continue;
        if (!pluginsHeader) {
            addSectionHeader(QStringLiteral("PLUGINS"));
            pluginsHeader = true;
        }
        addStaticRow(QString("%1 : Plugin %2 overrides: %3")
                         .arg(p.overrides.join(", ").leftJustified(16),
                              p.name, p.description.isEmpty() ? p.id : p.description));
    }
    for (const QString& line : NativePluginHost::instance().overrideSummaries()) {
        if (!pluginsHeader) {
            addSectionHeader(QStringLiteral("PLUGINS"));
            pluginsHeader = true;
        }
        addStaticRow(line);
    }

    m_contentLayout->addStretch(1);
}

void HelpDialog::addSectionHeader(const QString& title) {
    auto* header = new QLabel(QString("===== %1 =====").arg(title), m_content);
    header->setFont(Theme::getMonospaceFont(9, QFont::Bold));
    header->setStyleSheet(QString("color: %1; padding-top: 8px;")
                              .arg(Theme::Colors::Surface2().name()));
    m_contentLayout->addWidget(header);
}

void HelpDialog::addStaticRow(const QString& text) {
    auto* label = new QLabel(QString("• %1").arg(text), m_content);
    label->setFont(font());
    label->setStyleSheet(QString("color: %1;").arg(Theme::Colors::Dim().name()));
    m_contentLayout->addWidget(label);
}

void HelpDialog::addActionRow(const QString& actionId, const QString& label, int keyColumnWidth) {
    const QStringList keys = Core::Keybinds::keysFor(actionId);
    const QStringList defaults = Core::Keybinds::defaultKeysFor(actionId);
    const bool custom = keys != defaults;
    const bool disabled = keys.isEmpty();

    auto* row = new QWidget(m_content);
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(8);

    auto* keyBtn = new QPushButton(disabled ? QStringLiteral("none") : keyDisplay(keys), row);
    keyBtn->setObjectName(QStringLiteral("helpKeyBtn"));
    keyBtn->setProperty("actionId", actionId);
    keyBtn->setFont(font());
    keyBtn->setFlat(true);
    keyBtn->setCursor(Qt::PointingHandCursor);
    keyBtn->setFocusPolicy(Qt::NoFocus);
    keyBtn->setFixedWidth(keyColumnWidth);
    const QString color = disabled ? Theme::Colors::Dim().name()
                          : custom ? Theme::Colors::Accent().name()
                                   : Theme::Colors::Text().name();
    keyBtn->setStyleSheet(QString(R"(
        QPushButton {
            background-color: transparent;
            border: 1px solid transparent;
            padding: 1px 6px;
            text-align: left;
            color: %1;
        }
        QPushButton:hover {
            background-color: %2;
            border: 1px solid %3;
        }
    )").arg(color, Theme::Colors::Surface0().name(), Theme::Colors::Surface1().name()));
    keyBtn->setToolTip(QString("%1\nClick: rebind · Right-click: reset / disable")
                           .arg(actionId));
    connect(keyBtn, &QPushButton::clicked, this, [this, actionId]() { beginCapture(actionId); });

    keyBtn->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(keyBtn, &QPushButton::customContextMenuRequested, this,
            [this, keyBtn, actionId](const QPoint& pos) {
                auto* menu = new QMenu(this);
                menu->setFont(font());
                menu->setStyleSheet(Theme::getApplicationStyleSheet());
                QAction* rebind = menu->addAction(QStringLiteral("Rebind…"));
                QAction* reset = menu->addAction(QStringLiteral("Reset to default"));
                QAction* disable = menu->addAction(QStringLiteral("Disable"));
                disable->setEnabled(!Core::Keybinds::keysFor(actionId).isEmpty());
                connect(rebind, &QAction::triggered, this, [this, actionId]() { beginCapture(actionId); });
                connect(reset, &QAction::triggered, this, [this, actionId]() { resetBinding(actionId); });
                connect(disable, &QAction::triggered, this, [this, actionId]() { disableBinding(actionId); });
                menu->exec(keyBtn->mapToGlobal(pos));
                menu->deleteLater();
            });

    h->addWidget(keyBtn);

    auto* desc = new QLabel(label, row);
    desc->setFont(font());
    desc->setStyleSheet(QString("color: %1;").arg(
        disabled ? Theme::Colors::Dim().name() : Theme::Colors::Text().name()));
    h->addWidget(desc, 1);

    m_contentLayout->addWidget(row);
}

void HelpDialog::beginCapture(const QString& actionId) {
    if (actionId.isEmpty()) return;
    const bool wasCapturing = isCapturing();
    m_captureAction = actionId;
    m_captureHint->setText(QString("Press new shortcut for «%1» (Esc: cancel · Backspace: disable)")
                               .arg(actionLabel(actionId)));
    m_captureHint->show();
    if (m_status) m_status->clear();
    setFocus(Qt::OtherFocusReason);
    if (!wasCapturing) emit captureChanged(true);
}

void HelpDialog::cancelCapture() {
    if (!isCapturing()) return;
    m_captureAction.clear();
    m_captureHint->hide();
    if (m_status) m_status->setText(QStringLiteral("Rebind cancelled"));
    emit captureChanged(false);
}

void HelpDialog::resetBinding(const QString& actionId) {
    if (actionId.isEmpty()) return;
    if (isCapturing()) cancelCapture();
    Core::Keybinds::resetUserBinding(actionId);
    Core::Keybinds::reload();
    rebuild();
    if (m_status) {
        m_status->setText(QString("Reset to default: %1 (%2)")
                              .arg(actionId, keyDisplayOf(Core::Keybinds::keysFor(actionId))));
    }
    emit keybindChanged();
}

void HelpDialog::disableBinding(const QString& actionId) {
    if (actionId.isEmpty()) return;
    if (isCapturing()) cancelCapture();
    Core::Keybinds::saveUserBinding(actionId, {});
    Core::Keybinds::reload();
    rebuild();
    if (m_status) m_status->setText(QString("Disabled: %1").arg(actionId));
    emit keybindChanged();
}

void HelpDialog::saveAndFinish(const QStringList& keys, const QString& status) {
    if (m_captureAction.isEmpty()) return;
    Core::Keybinds::saveUserBinding(m_captureAction, keys);
    finishCapture(status);
}

void HelpDialog::finishCapture(const QString& status) {
    m_captureAction.clear();
    if (m_captureHint) m_captureHint->hide();
    Core::Keybinds::reload();
    rebuild();
    if (m_status) m_status->setText(status);
    emit captureChanged(false);
    emit keybindChanged();
}

void HelpDialog::keyPressEvent(QKeyEvent* event) {
    if (!isCapturing()) {
        QWidget::keyPressEvent(event);
        return;
    }
    if (event->isAutoRepeat()) {
        event->accept();
        return;
    }
    const int key = event->key();
    if (key == Qt::Key_Escape) {
        cancelCapture();
        event->accept();
        return;
    }
    if (key == Qt::Key_Backspace || key == Qt::Key_Delete) {
        const QString id = m_captureAction;
        saveAndFinish({}, QString("Disabled: %1").arg(actionLabel(id)));
        event->accept();
        return;
    }
    // чистые модификаторы и «неизвестные» нажатия не завершают захват
    if (key == Qt::Key_Shift || key == Qt::Key_Control || key == Qt::Key_Alt ||
        key == Qt::Key_Meta || key == Qt::Key_unknown) {
        event->accept();
        return;
    }
    const QKeySequence seq(event->keyCombination());
    const QString portable = seq.toString(QKeySequence::PortableText);
    if (portable.isEmpty()) {
        event->accept();
        return;
    }
    const QString id = m_captureAction;
    // конфликт не блокирует назначение, но показывается: при неоднозначном
    // QShortcut Qt глушит оба действия
    const QString conflict = conflictForAction(id, portable);
    QString status = QString("Rebound: %1 → %2")
                         .arg(actionLabel(id), seq.toString(QKeySequence::NativeText));
    if (!conflict.isEmpty())
        status += QString("  · already bound to: %1").arg(actionLabel(conflict));
    saveAndFinish(QStringList{portable}, status);
    event->accept();
}

}
