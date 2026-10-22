#include "FileChooserDialog.hpp"
#include "BookmarksBar.hpp"
#include "MillerColumns.hpp"
#include "StatusBar.hpp"
#include "../atlifm-core/XdgUtils.hpp"
#include "../atlifm-dnd/DragDropHandler.hpp"
#include "../atlifm-theme/Theme.hpp"
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QShortcut>
#include <QVBoxLayout>

namespace Widgets {

FileChooserDialog::FileChooserDialog(QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("fileChooserPage"));
    setupUi();
}

void FileChooserDialog::setupUi() {
    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // верх: в режиме Save имя файла; путь остаётся внизу в адресной строке
    m_nameBar = new QWidget(this);
    m_nameBar->setObjectName(QStringLiteral("chooserNameBar"));
    m_nameBar->setAttribute(Qt::WA_StyledBackground, true);
    auto* nameLayout = new QHBoxLayout(m_nameBar);
    nameLayout->setContentsMargins(8, 5, 8, 5);
    nameLayout->setSpacing(8);

    m_nameLabel = new QLabel(QStringLiteral("File name"), m_nameBar);
    m_nameLabel->setFont(Theme::getMonospaceFont(9, QFont::Bold));
    nameLayout->addWidget(m_nameLabel, 0, Qt::AlignVCenter);

    // поле имени только для режима сохранения (PortalService SaveFile)
    m_nameEdit = new QLineEdit(m_nameBar);
    m_nameEdit->setObjectName(QStringLiteral("chooserNameEdit"));
    m_nameEdit->setFont(Theme::getMonospaceFont(9));
    m_nameEdit->setPlaceholderText(QStringLiteral("File name"));
    m_nameEdit->setAutoFillBackground(false);
    m_nameEdit->setMinimumWidth(220);
    m_nameEdit->setFixedHeight(26);
    m_nameEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_nameEdit->installEventFilter(this);
    connect(m_nameEdit, &QLineEdit::returnPressed, this, [this]() { acceptCurrent(); });
    nameLayout->addWidget(m_nameEdit, 1, Qt::AlignVCenter);
    m_nameBar->hide();
    mainLayout->addWidget(m_nameBar);

    // тело: тот же дизайн основного окна, панель закладок + 3 колонки файлов (родитель / текущая папка / превью)
    m_bodySplitter = new QSplitter(Qt::Horizontal, this);
    m_bodySplitter->setHandleWidth(2);
    m_bodySplitter->setChildrenCollapsible(true);

    m_bookmarksBar = new BookmarksBar(m_bodySplitter);
    m_bookmarksBar->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    m_bookmarksBar->setMinimumWidth(0);
    m_bookmarksBar->setMaximumWidth(380);

    m_miller = new MillerViewContainer(m_bodySplitter);
    m_miller->setMinimumWidth(260);
    // двойной клик/Enter по файлу даёт сигнал выбора, а не запуск приложением
    m_miller->setFileChooseMode(true);

    m_bodySplitter->addWidget(m_bookmarksBar);
    m_bodySplitter->addWidget(m_miller);
    m_bodySplitter->setStretchFactor(0, 0);
    m_bodySplitter->setStretchFactor(1, 1);
    m_bodySplitter->setSizes({190, 640});
    mainLayout->addWidget(m_bodySplitter, 1);

    // навигация из колонок обновляет активное место закладок и путь внизу
    connect(m_miller, &MillerViewContainer::directoryChanged, this,
            [this](const std::filesystem::path& dir) { onDirectoryChanged(dir); });
    connect(m_bookmarksBar, &BookmarksBar::navigateRequested, this,
            [this](const std::filesystem::path& dir) { setDirectory(dir); });
    // двойной клик/Enter по файлу в колонках выбор файла
    connect(m_miller, &MillerViewContainer::fileChosenRequested, this,
            [this](const std::filesystem::path& path) { onFileActivated(path); });
    // Ctrl+D «Bookmark» внутри диалога в ту же панель закладок
    connect(m_miller, &MillerViewContainer::bookmarkAdded, this,
            [this](const std::filesystem::path& path) {
                if (m_bookmarksBar) m_bookmarksBar->addBookmark(path);
            });
    // DnD на панель закладок как в основном окне
    connect(m_bookmarksBar, &BookmarksBar::filesDropped, this,
            [this](const auto& srcs, const auto& dest, auto act, auto mods) {
                if (m_miller) Dnd::DragDropHandler::handleDrop(srcs, dest, act, mods, m_miller);
                if (m_bookmarksBar) m_bookmarksBar->refresh();
            });
    connect(m_bookmarksBar, &BookmarksBar::trashClearRequested, this, [this]() {
        if (m_miller && m_miller->tryPluginOverride(QStringLiteral("clear_trash"))) return;
        Core::XdgUtils::clearTrash();
        if (m_bookmarksBar) m_bookmarksBar->refresh();
        if (m_miller) m_miller->refresh();
    });

    // низ: адресная строка, под ней кнопки Cancel/Open
    m_statusBar = new TuiStatusBar(this);
    connect(m_statusBar, &TuiStatusBar::pathEntered, this,
            [this](const std::filesystem::path& dir) { setDirectory(dir); });
    connect(m_statusBar, &TuiStatusBar::filterChanged, this,
            [this](const QString& filter) {
                if (m_miller) m_miller->setFilter(filter);
            });
    connect(m_statusBar, &TuiStatusBar::filterDismissed, this, [this]() {
        if (m_miller) m_miller->setFilter(QString());
    });
    // счётчик файлов отключён: снизу путь, действие подтверждается кнопками в нижней панели
    m_statusBar->setItemStatsVisible(false);
    connect(m_miller, &MillerViewContainer::messageEmitted, this,
            [this](const QString& msg) {
                if (m_statusBar) m_statusBar->setProgressMessage(msg);
            });
    connect(m_miller, &MillerViewContainer::sudoModeChanged, this, [this](bool isSudo) {
        if (m_statusBar) m_statusBar->setSudoBadge(isSudo);
    });
    // фильтр из колонок (Ctrl+F или набор текста) в статусбар диалога
    connect(m_miller, &MillerViewContainer::filterTriggered, this, [this]() {
        if (m_statusBar) m_statusBar->activateFilter();
    });
    connect(m_miller, &MillerViewContainer::typeAheadSearchTriggered, this,
            [this](const QString& text) {
                if (m_statusBar) m_statusBar->activateFilterWithText(text);
            });

    mainLayout->addWidget(m_statusBar);

    m_actionBar = new QWidget(this);
    m_actionBar->setObjectName(QStringLiteral("chooserActionBar"));
    m_actionBar->setAttribute(Qt::WA_StyledBackground, true);
    auto* actionLayout = new QHBoxLayout(m_actionBar);
    actionLayout->setContentsMargins(8, 4, 8, 6);
    actionLayout->setSpacing(8);
    actionLayout->addStretch(1);

    m_cancelBtn = new QPushButton(QStringLiteral("Cancel"), m_actionBar);
    m_openBtn = new QPushButton(QStringLiteral("Open"), m_actionBar);
    m_cancelBtn->setObjectName(QStringLiteral("chooserCancelBtn"));
    m_openBtn->setObjectName(QStringLiteral("chooserOpenBtn"));
    m_cancelBtn->setAutoDefault(false);
    m_openBtn->setAutoDefault(false);
    m_cancelBtn->setFixedHeight(26);
    m_openBtn->setFixedHeight(26);
    connect(m_cancelBtn, &QPushButton::clicked, this, [this]() { emit closeRequested(); });
    connect(m_openBtn, &QPushButton::clicked, this, [this]() { acceptCurrent(); });
    actionLayout->addWidget(m_cancelBtn, 0, Qt::AlignVCenter);
    actionLayout->addWidget(m_openBtn, 0, Qt::AlignVCenter);
    mainLayout->addWidget(m_actionBar);

    // Esc закрывает диалог из любого места, в т.ч. когда фокус в колонках
    auto* esc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    esc->setContext(Qt::WidgetWithChildrenShortcut);
    connect(esc, &QShortcut::activated, this, [this]() { emit closeRequested(); });

    // Ctrl+L / Alt+D / F4 ввод пути в нижнюю адресную строку, контекст WidgetWithChildren
    // перекрывает шорткаты главного окна, пока фокус внутри диалога
    const QList<QKeySequence> pathKeys{QKeySequence(QStringLiteral("Ctrl+L")),
                                       QKeySequence(QStringLiteral("Alt+D")),
                                       QKeySequence(Qt::Key_F4)};
    for (const auto& key : pathKeys) {
        auto* sc = new QShortcut(key, this);
        sc->setContext(Qt::WidgetWithChildrenShortcut);
        connect(sc, &QShortcut::activated, this, [this]() {
            if (m_statusBar) m_statusBar->focusPathInput();
        });
    }

    updateTheme();
}

void FileChooserDialog::setDirectory(const std::filesystem::path& dir) {
    if (dir.empty()) return;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec) || ec) return;

    m_miller->setDirectory(dir);
    // Miller эмитит directoryChanged (→ onDirectoryChanged), но для пустого пути или ранних вызовов подстраховка
    onDirectoryChanged(m_miller->currentDirectory());
}

std::filesystem::path FileChooserDialog::currentDirectory() const {
    return m_miller ? m_miller->currentDirectory() : std::filesystem::path();
}

std::filesystem::path FileChooserDialog::selectedPath() const {
    if (!m_miller) return {};
    const auto paths = m_miller->selectedPaths();
    if (paths.empty()) return {};
    const auto& p = paths.front();
    std::error_code ec;
    if (std::filesystem::is_directory(p, ec) && !ec) return {};
    return p;
}

QStringList FileChooserDialog::selectedFilePaths() const {
    QStringList out;
    if (!m_miller) return out;
    // явный мультивыбор (Space, Ctrl+клик) → выбранные файлы, иначе текущий
    for (const auto& p : m_miller->selectedPaths()) {
        std::error_code ec;
        if (std::filesystem::is_directory(p, ec) && !ec) continue;
        out << QString::fromUtf8(p.c_str());
    }
    out.removeDuplicates();
    return out;
}

void FileChooserDialog::setMultipleSelectionEnabled(bool enabled) {
    m_multiple = enabled;
    if (m_miller) m_miller->setMultiSelectEnabled(enabled);
}

void FileChooserDialog::setDirectoryMode(bool enabled) { m_directoryMode = enabled; }

void FileChooserDialog::setAcceptLabel(const QString& label) {
    m_acceptLabel = label.trimmed().isEmpty() ? QStringLiteral("Open") : label.trimmed();
    if (m_openBtn && !m_saveMode) m_openBtn->setText(m_acceptLabel);
}

void FileChooserDialog::setSaveMode(bool enabled, const QString& initialName) {
    m_saveMode = enabled;
    if (m_nameBar) m_nameBar->setVisible(enabled);
    if (m_nameEdit) {
        if (enabled) {
            m_nameEdit->setText(initialName);
        } else {
            m_nameEdit->clear();
        }
    }
    if (m_openBtn) m_openBtn->setText(enabled ? QStringLiteral("Save") : m_acceptLabel);
}

void FileChooserDialog::focusPrimaryInput() {
    if (m_saveMode && m_nameEdit && m_nameEdit->isVisible()) {
        m_nameEdit->setFocus();
        m_nameEdit->setCursorPosition(m_nameEdit->text().size());
    } else if (m_miller) {
        m_miller->focusCenter();
    }
}

// каталог колонок изменился: активное место в закладках и путь внизу
void FileChooserDialog::onDirectoryChanged(const std::filesystem::path& dir) {
    if (m_bookmarksBar) m_bookmarksBar->setActiveDirectory(dir);
    if (m_statusBar) {
        m_statusBar->setPath(dir);
        m_statusBar->setMountStats(dir);
    }
}

void FileChooserDialog::onFileActivated(const std::filesystem::path& path) {
    if (m_directoryMode) return; // в режиме каталогов файлы не выбираются
    if (m_saveMode) {
        // двойной клик/Enter по файлу в режиме сохранения подставляет имя
        if (m_nameEdit) {
            m_nameEdit->setText(QString::fromUtf8(path.filename().c_str()));
            m_nameEdit->setFocus();
        }
        return;
    }
    if (m_multiple) {
        const QStringList paths = selectedFilePaths();
        if (!paths.isEmpty()) emit filesChosen(paths);
        return;
    }
    emit fileChosen(QString::fromUtf8(path.c_str()));
}

void FileChooserDialog::acceptCurrent() {
    if (m_saveMode) {
        QString name = m_nameEdit ? m_nameEdit->text().trimmed() : QString();
        if (name.isEmpty()) {
            if (m_statusBar) m_statusBar->setProgressMessage(QStringLiteral("Enter a file name"));
            return;
        }
        if (name.contains(QChar(u'\0'))) {
            if (m_statusBar) m_statusBar->setProgressMessage(QStringLiteral("Invalid file name"));
            return;
        }
        std::filesystem::path target(name.toUtf8().toStdString());
        // имя из портала не должно уводить запись из показанного каталога: абсолютные пути
        // и ".."/"." отклоняем до нормализации, иначе lexically_normal() их легализует (../evil, /etc/passwd)
        if (target.is_absolute()) {
            if (m_statusBar) m_statusBar->setProgressMessage(QStringLiteral("Invalid file name"));
            return;
        }
        for (const auto& part : target) {
            if (part == "." || part == "..") {
                if (m_statusBar) m_statusBar->setProgressMessage(QStringLiteral("Invalid file name"));
                return;
            }
        }
        target = (currentDirectory() / target).lexically_normal();
        const std::string base = target.filename().string();
        if (base.empty()) {
            if (m_statusBar) m_statusBar->setProgressMessage(QStringLiteral("Invalid file name"));
            return;
        }
        emit fileChosen(QString::fromUtf8(target.c_str()));
        return;
    }
    if (m_directoryMode) {
        // выделенный каталог, иначе текущий открытый
        if (m_miller) {
            for (const auto& p : m_miller->selectedPaths()) {
                std::error_code ec;
                if (std::filesystem::is_directory(p, ec) && !ec) {
                    emit fileChosen(QString::fromUtf8(p.c_str()));
                    return;
                }
            }
        }
        const auto cur = currentDirectory();
        if (!cur.empty()) emit fileChosen(QString::fromUtf8(cur.c_str()));
        return;
    }
    if (m_multiple) {
        const QStringList paths = selectedFilePaths();
        if (!paths.isEmpty()) emit filesChosen(paths);
        return;
    }
    const auto path = selectedPath();
    if (!path.empty()) emit fileChosen(QString::fromUtf8(path.c_str()));
}

void FileChooserDialog::updateTheme() {
    using namespace Theme::Colors;

    if (m_nameBar) {
        m_nameBar->setStyleSheet(QString(
            "QWidget#chooserNameBar { background-color: %1; border-bottom: 1px solid %2; }")
            .arg(Surface0().name(), Surface1().name()));
    }
    if (m_actionBar) {
        m_actionBar->setStyleSheet(QString(
            "QWidget#chooserActionBar { background-color: %1; border-top: 1px solid %2; }")
            .arg(Base().name(), Surface1().name()));
    }
    if (m_nameLabel) {
        m_nameLabel->setStyleSheet(QString("color: %1;").arg(Subtext().name()));
    }
    if (m_nameEdit) {
        m_nameEdit->setStyleSheet(QString(R"(
            QLineEdit {
                background-color: %1;
                color: %2;
                border: 1px solid %3;
                border-radius: 0px;
                padding: 3px 8px;
            }
            QLineEdit:focus { border: 1px solid %4; }
        )").arg(Base().name(), InputFg().name(), InputBorder().name(), InputFocusBorder().name()));
    }
    if (m_openBtn) {
        // метрики Open идентичны Cancel (border 1px + padding 2px 14px), иначе кнопки разного размера
        m_openBtn->setStyleSheet(QString(R"(
            QPushButton {
                background-color: %1;
                color: %2;
                border: 1px solid transparent;
                border-radius: 0px;
                padding: 2px 14px;
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
                padding: 2px 14px;
            }
        )").arg(Surface0().name(), Text().name(), Surface1().name()));
    }
    if (m_statusBar) m_statusBar->updateTheme();
}

bool FileChooserDialog::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_nameEdit && event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Escape) {
            emit closeRequested();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

}
