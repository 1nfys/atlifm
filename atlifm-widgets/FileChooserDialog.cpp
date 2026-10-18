#include "FileChooserDialog.hpp"
#include "BookmarksBar.hpp"
#include "MillerColumns.hpp"
#include "StatusBar.hpp"
#include "../atlifm-core/Icons.hpp"
#include "../atlifm-core/XdgUtils.hpp"
#include "../atlifm-dnd/DragDropHandler.hpp"
#include "../atlifm-theme/Theme.hpp"
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QPainter>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cstdlib>

namespace Widgets {

namespace {

// разделитель breadcrumb рисуется вручную, а не текстовым глифом «›»: при дробном масштабе
// растрирование смещается на пиксель между метками, рисование по центру виджета даёт одинаковую позицию
class CrumbSeparator : public QWidget {
public:
    explicit CrumbSeparator(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedSize(9, 28);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        QPen pen(Theme::Colors::Dim());
        pen.setWidthF(1.4);
        p.setPen(pen);
        const QPointF c(width() / 2.0 - 0.5, height() / 2.0 + 0.5);
        p.drawLine(QPointF(c.x() - 1.6, c.y() - 3.4), QPointF(c.x() + 1.6, c.y()));
        p.drawLine(QPointF(c.x() + 1.6, c.y()), QPointF(c.x() - 1.6, c.y() + 3.4));
    }
};

// стиль сегмента breadcrumb: текущий с заливкой Accent, остальные с hover
QString crumbStyle(bool current) {
    using namespace Theme::Colors;
    if (current) {
        return QStringLiteral(
            "QPushButton { background: %1; color: %2; border: none;"
            " padding: 3px 8px; }")
            .arg(SelectionBg().name(), SelectionFg().name());
    }
    return QStringLiteral(
        "QPushButton { background: transparent; color: %1; border: none;"
        " padding: 3px 8px; }"
        "QPushButton:hover { background: %2; color: %3; }")
        .arg(Subtext().name(), Surface1().name(), Text().name());
}

}

FileChooserDialog::FileChooserDialog(QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("fileChooserPage"));
    setupUi();
}

void FileChooserDialog::setupUi() {
    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // верх: строка путей breadcrumb вместо вкладок основного окна, клик по сегменту переход в него
    m_crumbScroll = new QScrollArea(this);
    m_crumbScroll->setObjectName(QStringLiteral("chooserBreadcrumb"));
    m_crumbScroll->setWidgetResizable(true);
    m_crumbScroll->setFrameShape(QFrame::NoFrame);
    m_crumbScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_crumbScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_crumbScroll->setFixedHeight(28);
    m_crumbScroll->setAttribute(Qt::WA_StyledBackground, true);

    m_crumbHost = new QWidget(m_crumbScroll);
    m_crumbHost->setObjectName(QStringLiteral("chooserBreadcrumbHost"));
    m_crumbHost->setAttribute(Qt::WA_StyledBackground, true);
    m_crumbLayout = new QHBoxLayout(m_crumbHost);
    m_crumbLayout->setContentsMargins(8, 0, 8, 0);
    m_crumbLayout->setSpacing(0);
    m_crumbLayout->addStretch(1);
    m_crumbScroll->setWidget(m_crumbHost);
    mainLayout->addWidget(m_crumbScroll);

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

    // навигация из колонок обновляет breadcrumb и активное место закладок
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

    // низ: адресная строка как в основном окне, добавляется после кнопок
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
    // счётчик файлов отключён, на его месте справа живут кнопки Cancel/Open
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

    // поле имени только для режима сохранения (PortalService SaveFile)
    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setObjectName(QStringLiteral("chooserNameEdit"));
    m_nameEdit->setFont(Theme::getMonospaceFont(9));
    m_nameEdit->setPlaceholderText(QStringLiteral("File name"));
    m_nameEdit->setAutoFillBackground(false);
    m_nameEdit->hide();
    m_nameEdit->installEventFilter(this);
    connect(m_nameEdit, &QLineEdit::returnPressed, this, [this]() { acceptCurrent(); });
    mainLayout->addWidget(m_nameEdit);

    mainLayout->addWidget(m_statusBar);

    // кнопки Cancel/Open прямо в нижней адресной строке, на месте счётчика файлов основного окна
    m_cancelBtn = new QPushButton(QStringLiteral("Cancel"), this);
    m_openBtn = new QPushButton(QStringLiteral("Open"), this);
    m_cancelBtn->setObjectName(QStringLiteral("chooserCancelBtn"));
    m_openBtn->setObjectName(QStringLiteral("chooserOpenBtn"));
    m_cancelBtn->setAutoDefault(false);
    m_openBtn->setAutoDefault(false);
    connect(m_cancelBtn, &QPushButton::clicked, this, [this]() { emit closeRequested(); });
    connect(m_openBtn, &QPushButton::clicked, this, [this]() { acceptCurrent(); });
    m_statusBar->addTrailingWidget(m_cancelBtn);
    m_statusBar->addTrailingWidget(m_openBtn);

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
    rebuildBreadcrumb();
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
    if (m_nameEdit) {
        m_nameEdit->setVisible(enabled);
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
        m_miller->setFocus();
    }
}

// каталог колонок изменился: активное место в закладках и пересборка breadcrumb
void FileChooserDialog::onDirectoryChanged(const std::filesystem::path& dir) {
    if (m_bookmarksBar) m_bookmarksBar->setActiveDirectory(dir);
    if (m_statusBar) {
        m_statusBar->setPath(dir);
        m_statusBar->setMountStats(dir);
    }
    rebuildBreadcrumb();
}

void FileChooserDialog::rebuildBreadcrumb() {
    if (!m_crumbLayout) return;

    while (m_crumbLayout->count() > 0) {
        QLayoutItem* item = m_crumbLayout->takeAt(0);
        if (auto* w = item->widget()) {
            // hide() сразу, deleteLater позже: иначе старые сегменты ещё кадр
            // видны поверх новых и засвечиваются на скриншотах из grab()
            w->hide();
            w->deleteLater(); // клик по сегменту пересобирает его из своего же слота
        }
        delete item;
    }
    m_crumbLayout->addStretch(1);

    const std::filesystem::path cur = currentDirectory();
    if (cur.empty()) return;

    // сегменты: корень (иконка диска без подписи) и каждый компонент пути
    std::vector<std::filesystem::path> segments;
    std::vector<QString> labels;
    segments.push_back(cur.root_path().empty() ? std::filesystem::path("/") : cur.root_path());
    labels.push_back(QString());
    std::filesystem::path acc = segments.front();
    for (const auto& part : cur.relative_path()) {
        if (part.empty() || part == ".") continue;
        acc /= part;
        segments.push_back(acc);
        labels.push_back(QString::fromUtf8(part.c_str()));
    }

    for (size_t i = 0; i < segments.size(); ++i) {
        if (i > 0) {
            auto* sep = new CrumbSeparator(m_crumbHost);
            m_crumbLayout->insertWidget(m_crumbLayout->count() - 1, sep);
        }
        const bool isCurrent = (segments[i] == cur);
        QString text = labels[i];
        if (text.isEmpty()) text = Icons::Glyph::DriveDisk;

        auto* btn = new QPushButton(text, m_crumbHost);
        btn->setFont(Theme::getMonospaceFont(9, isCurrent ? QFont::Bold : QFont::Normal));
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet(crumbStyle(isCurrent));
        btn->setToolTip(QString::fromUtf8(segments[i].c_str()));
        const std::filesystem::path target = segments[i];
        connect(btn, &QPushButton::clicked, this, [this, target]() {
            if (target != currentDirectory()) setDirectory(target);
        });
        m_crumbLayout->insertWidget(m_crumbLayout->count() - 1, btn);
    }

    // длинный путь: правый (текущий) сегмент всегда видим
    QPointer<QScrollArea> scroll = m_crumbScroll;
    QTimer::singleShot(0, this, [scroll]() {
        if (scroll && scroll->horizontalScrollBar())
            scroll->horizontalScrollBar()->setValue(scroll->horizontalScrollBar()->maximum());
    });
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

    if (m_crumbScroll) {
        m_crumbScroll->setStyleSheet(QString(
            "QScrollArea#chooserBreadcrumb { background-color: %1; border: none; }")
            .arg(Surface0().name()));
    }
    if (m_crumbHost) {
        m_crumbHost->setStyleSheet(QString(
            "QWidget#chooserBreadcrumbHost { background-color: %1; }")
            .arg(Surface0().name()));
    }
    if (m_nameEdit) {
        m_nameEdit->setStyleSheet(QString(R"(
            QLineEdit {
                background-color: %1;
                color: %2;
                border: 1px solid %3;
                border-radius: 0px;
                padding: 3px 8px;
                margin: 8px 14px 0px 14px;
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
    rebuildBreadcrumb();
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
