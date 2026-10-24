#include "ArchiveTreeWidget.hpp"
#include "TuiInputPopup.hpp"
#include "../atlifm-core/Icons.hpp"
#include "../atlifm-dnd/DragDropHandler.hpp"
#include "../atlifm-theme/Theme.hpp"
#include "../atlifm-models/ArchiveModel.hpp"

#include <QApplication>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QDrag>
#include <QEventLoop>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHeaderView>
#include <QImageReader>
#include <QKeyEvent>
#include <QMenu>
#include <QMetaObject>
#include <QMimeData>
#include <QMouseEvent>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QPointer>
#include <QRegularExpression>
#include <QRubberBand>
#include <QScrollBar>
#include <QStandardPaths>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTime>
#include <QTimer>
#include <QTreeWidgetItemIterator>
#include <QUrl>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <functional>
#include <map>
#include <memory>
#include <optional>

namespace Widgets {

namespace {

constexpr int kEntryPathRole = Qt::UserRole + 1;
constexpr int kMemberPathRole = Qt::UserRole + 2;
constexpr int kIsDirRole = Qt::UserRole + 3;
constexpr int kIconGlyphRole = Qt::UserRole + 4;
constexpr int kIconColorRole = Qt::UserRole + 5;

bool isEntryItem(const QTreeWidgetItem* item) {
    return item && !item->data(0, kMemberPathRole).toString().isEmpty();
}

bool isVisibleEntry(const QTreeWidgetItem* item) {
    if (!isEntryItem(item) || item->isHidden()) return false;
    for (const QTreeWidgetItem* parent = item->parent(); parent; parent = parent->parent()) {
        if (parent->isHidden() || !parent->isExpanded()) return false;
    }
    return true;
}

void dropNestedSelectedPaths(QStringList& members, QStringList& normalized) {
    QStringList keptMembers;
    QStringList keptNormalized;
    for (int i = 0; i < normalized.size(); ++i) {
        const QString path = normalized.at(i);
        QString parent = path;
        bool nested = false;
        while (parent.contains(QLatin1Char('/'))) {
            parent.truncate(parent.lastIndexOf(QLatin1Char('/')));
            if (normalized.contains(parent)) {
                nested = true;
                break;
            }
        }
        if (nested) continue;
        keptNormalized << path;
        if (i < members.size()) keptMembers << members.at(i);
    }
    members = keptMembers;
    normalized = keptNormalized;
}

QString normalizeEntryPath(const QString& raw) {
    QString p = raw;
    p.replace('\\', '/');
    QStringList segs = p.split('/', Qt::SkipEmptyParts);
    segs.removeAll(QStringLiteral("."));
    while (!segs.isEmpty() && segs.first() == QStringLiteral("..")) segs.removeFirst();
    return segs.join('/');
}

bool rawUsesDotSlash(const QString& raw) {
    return raw.startsWith(QStringLiteral("./")) || raw.startsWith(QStringLiteral(".\\"));
}

Icons::IconInfo iconForName(const QString& name, bool isDir) {
    return Icons::getIcon(std::filesystem::path(name.toStdString()), isDir,
                          false, false, std::optional<bool>(false));
}

struct ArchiveNode {
    QString name;
    QString entryPath;   // нормализованный путь без завершающего слеша
    QString memberPath;  // имя как в архиве для распаковки
    QString modified;
    quint64 size{0};
    bool isDir{false};
    bool hasActualEntry{false};
    bool dotSlash{false};
    std::map<QString, std::unique_ptr<ArchiveNode>> children;
};

void finalizeTreeNode(ArchiveNode* node) {
    for (auto& [name, child] : node->children) {
        Q_UNUSED(name);
        finalizeTreeNode(child.get());
        if (child->memberPath.isEmpty()) {
            child->memberPath = (child->dotSlash ? QStringLiteral("./") : QString()) +
                                child->entryPath;
        }
    }
}

std::vector<ArchiveNode*> sortedChildren(ArchiveNode* node) {
    std::vector<ArchiveNode*> ordered;
    ordered.reserve(node->children.size());
    for (auto& [name, child] : node->children) {
        Q_UNUSED(name);
        ordered.push_back(child.get());
    }
    std::sort(ordered.begin(), ordered.end(), [](const ArchiveNode* a, const ArchiveNode* b) {
        if (a->isDir != b->isDir) return a->isDir;
        const int c = QString::compare(a->name, b->name, Qt::CaseInsensitive);
        if (c != 0) return c < 0;
        return a->name < b->name;
    });
    return ordered;
}

std::unique_ptr<ArchiveNode> buildTree(const std::vector<Core::XdgUtils::ArchiveEntry>& entries) {
    auto root = std::make_unique<ArchiveNode>();
    for (const auto& entry : entries) {
        const QString norm = normalizeEntryPath(entry.path);
        if (norm.isEmpty()) continue;
        const QStringList parts = norm.split('/', Qt::SkipEmptyParts);
        if (parts.isEmpty()) continue;

        ArchiveNode* cur = root.get();
        QString prefix;
        for (int i = 0; i < parts.size(); ++i) {
            const QString& part = parts.at(i);
            prefix += (prefix.isEmpty() ? QString() : QStringLiteral("/")) + part;
            auto& slot = cur->children[part];
            if (!slot) {
                slot = std::make_unique<ArchiveNode>();
                slot->name = part;
                slot->entryPath = prefix;
            }
            ArchiveNode* child = slot.get();
            if (i + 1 < parts.size()) child->isDir = true;
            if (i + 1 == parts.size()) {
                child->isDir = entry.isDir;
                child->size = entry.size;
                child->modified = entry.modified;
                child->hasActualEntry = true;
                child->memberPath = entry.path;
                // tar и zip хранят каталоги с завершающим слешем а extractArchiveEntries ждет имя записи без него
                if (child->isDir) {
                    while (child->memberPath.endsWith(QLatin1Char('/')) ||
                           child->memberPath.endsWith(QLatin1Char('\\'))) {
                        child->memberPath.chop(1);
                    }
                }
            }
            if (rawUsesDotSlash(entry.path)) child->dotSlash = true;
            cur = child;
        }
    }
    finalizeTreeNode(root.get());
    return root;
}

class ArchiveItemDelegate : public QStyledItemDelegate {
public:
    explicit ArchiveItemDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        if (!index.isValid()) return;
        using namespace Theme::Colors;

        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;
        const QColor bg = selected ? SelectionBg() : (hovered ? HoverBg() : Base());
        painter->fillRect(option.rect, bg);

        QFont font = Theme::getMonospaceFont(10);
        painter->setFont(font);
        QFontMetrics fm(font);
        const int y = option.rect.top() + (option.rect.height() + fm.ascent() - fm.descent()) / 2;
        QColor fg = selected ? SelectionFg() : Text();
        const QVariant fgData = index.data(Qt::ForegroundRole);
        if (!selected && fgData.canConvert<QBrush>()) {
            const QColor custom = qvariant_cast<QBrush>(fgData).color();
            if (custom.isValid()) fg = custom;
        }

        if (index.column() == 0) {
            int x = option.rect.left() + 4;
            const QString glyph = index.data(kIconGlyphRole).toString();
            const QColor iconColor = index.data(kIconColorRole).value<QColor>();
            if (!glyph.isEmpty()) {
                painter->setPen(selected ? SelectionFg() : iconColor);
                painter->drawText(x, y, glyph);
                x += fm.horizontalAdvance(glyph) + 6;
            }
            painter->setPen(fg);
            const QString name = index.data(Qt::DisplayRole).toString();
            const int available = option.rect.right() - x - 4;
            if (available > 8) {
                painter->drawText(x, y, fm.elidedText(name, Qt::ElideMiddle, available));
            }
        } else if (index.column() == 1) {
            const QString size = index.data(Qt::DisplayRole).toString();
            if (!size.isEmpty()) {
                painter->setPen(selected ? SelectionFg() : Dim());
                painter->drawText(option.rect.adjusted(0, 0, -8, 0),
                                  Qt::AlignRight | Qt::AlignVCenter, size);
            }
        }
    }

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        Q_UNUSED(option);
        Q_UNUSED(index);
        const QFontMetrics fm(Theme::getMonospaceFont(10));
        return QSize(0, fm.height() + 2);
    }
};

// кэш распакованных записей уникален по пути и mtime и размеру чтобы повторные перетаскивания были мгновенными
std::filesystem::path archiveCacheDir(const std::filesystem::path& archive) {
    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(archive, ec);
    const qint64 ns = ec ? 0 : std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    mtime.time_since_epoch()).count();
    const auto size = std::filesystem::file_size(archive, ec);
    const QString sig = QString::fromUtf8(archive.c_str()) + QLatin1Char('|') +
                        QString::number(ns) + QLatin1Char('|') +
                        QString::number(ec ? 0 : static_cast<qulonglong>(size));
    const QByteArray hash = QCryptographicHash::hash(sig.toUtf8(), QCryptographicHash::Sha1)
                                .toHex().left(16);
    const QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    return std::filesystem::path((base + QStringLiteral("/archive-drag/") +
                                  QString::fromLatin1(hash)).toStdString());
}

void cleanupOldArchiveCacheOnce() {
    static bool done = false;
    if (done) return;
    done = true;
    const QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
                         QStringLiteral("/archive-drag");
    QDir dir(base);
    if (!dir.exists()) return;
    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-7);
    const QFileInfoList entries = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo& fi : entries) {
        if (fi.lastModified() < cutoff) {
            QDir(fi.absoluteFilePath()).removeRecursively();
        }
    }
}

} // namespace

ArchiveTreeWidget::ArchiveTreeWidget(QWidget* parent)
    : QTreeWidget(parent) {
    setObjectName("archiveTree");
    setColumnCount(2);
    setHeaderHidden(true);
    setRootIsDecorated(true);
    setItemsExpandable(true);
    setExpandsOnDoubleClick(false);
    setAnimated(false);
    setUniformRowHeights(true);
    setAllColumnsShowFocus(false);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setDragEnabled(true);
    setDragDropMode(QAbstractItemView::DragOnly);
    setDefaultDropAction(Qt::CopyAction);
    setContextMenuPolicy(Qt::CustomContextMenu);
    setFrameShape(QFrame::NoFrame);
    setIndentation(14);
    setWordWrap(false);
    // дерево живет в узкой панели и длинные пути не должны распирать сплиттер или ломать выделение
    setMinimumWidth(0);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    setTextElideMode(Qt::ElideMiddle);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFont(Theme::getMonospaceFont(10));
    setItemDelegate(new ArchiveItemDelegate(this));
    setStyleSheet(QString(
        "QTreeWidget { background-color: %1; color: %2; border: none; outline: 0; }"
        "QTreeWidget::item { padding: 0px; }"
        "QHeaderView::section { background-color: %1; border: none; }")
        .arg(Theme::Colors::Base().name(), Theme::Colors::Text().name()));

    QHeaderView* h = header();
    h->setStretchLastSection(false);
    h->setSectionResizeMode(0, QHeaderView::Stretch);
    h->setSectionResizeMode(1, QHeaderView::Fixed);
    h->resizeSection(1, 84);

    connect(this, &QTreeWidget::customContextMenuRequested,
            this, &ArchiveTreeWidget::showArchiveMenu);
    connect(this, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* current, QTreeWidgetItem*) {
                if (!m_preserveAnchor &&
                    !(QApplication::keyboardModifiers() & Qt::ShiftModifier)) {
                    m_selectionAnchor = isEntryItem(current) ? current : nullptr;
                }
                if (!isEntryItem(current)) {
                    emit entrySelected(QString(), QString(), true);
                    return;
                }
                emit entrySelected(current->data(0, kEntryPathRole).toString(),
                                   current->data(0, kMemberPathRole).toString(),
                                   current->data(0, kIsDirRole).toBool());
            });
}

void ArchiveTreeWidget::setArchiveModel(Models::ArchiveModel* model) {
    m_model = model;
    if (m_archivePath.empty() && model) m_archivePath = model->archivePath();
}

void ArchiveTreeWidget::setArchivePath(const std::filesystem::path& archivePath) {
    m_archivePath = archivePath;
}

void ArchiveTreeWidget::resetSelectionState() {
    m_selectionAnchor = nullptr;
    m_isRubberBanding = false;
    m_boxGesturePending = false;
    m_preserveAnchor = false;
    m_shiftSelecting = false;
    if (m_rubberBand) m_rubberBand->hide();
}

void ArchiveTreeWidget::clearArchive() {
    resetSelectionState();
    clear();
}

void ArchiveTreeWidget::showMessage(const QString& message) {
    resetSelectionState();
    clear();
    auto* item = new QTreeWidgetItem(this);
    item->setText(0, message);
    item->setData(0, kEntryPathRole, QString());
    item->setData(0, kMemberPathRole, QString());
    item->setData(0, kIsDirRole, false);
    item->setData(0, kIconGlyphRole, Icons::Glyph::FileDefault);
    item->setData(0, kIconColorRole, Theme::Colors::Dim());
    item->setFirstColumnSpanned(true);
    item->setFlags(Qt::ItemIsEnabled);
    item->setToolTip(0, message);
}

void ArchiveTreeWidget::selectAllEntries() {
    clearSelection();
    QTreeWidgetItem* first = nullptr;
    QTreeWidgetItemIterator it(this);
    for (; *it; ++it) {
        QTreeWidgetItem* item = *it;
        if (!isVisibleEntry(item)) continue;
        item->setSelected(true);
        if (!first) first = item;
    }
    if (first) {
        setCurrentItem(first, 0, QItemSelectionModel::NoUpdate);
        m_selectionAnchor = first;
        scrollToItem(first, QAbstractItemView::EnsureVisible);
    }
}

void ArchiveTreeWidget::selectFirstEntry() {
    QTreeWidgetItemIterator it(this);
    for (; *it; ++it) {
        QTreeWidgetItem* item = *it;
        if (!isVisibleEntry(item)) continue;
        clearSelection();
        setCurrentItem(item, 0, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        m_selectionAnchor = item;
        scrollToItem(item, QAbstractItemView::PositionAtTop);
        return;
    }
}

void ArchiveTreeWidget::selectLastEntry() {
    QTreeWidgetItem* last = nullptr;
    QTreeWidgetItemIterator it(this);
    for (; *it; ++it) {
        if (isVisibleEntry(*it)) last = *it;
    }
    if (!last) return;
    clearSelection();
    setCurrentItem(last, 0, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    m_selectionAnchor = last;
    scrollToItem(last, QAbstractItemView::PositionAtBottom);
}

void ArchiveTreeWidget::clearEntrySelection() {
    resetSelectionState();
    clearSelection();
    if (selectionModel()) selectionModel()->clearCurrentIndex();
    viewport()->update();
}

bool ArchiveTreeWidget::hasSelectedEntries() const {
    const QList<QTreeWidgetItem*> items = selectedItems();
    for (QTreeWidgetItem* item : items) {
        if (isEntryItem(item)) return true;
    }
    return false;
}

void ArchiveTreeWidget::copySelectedEntries() {
    if (m_extracting) {
        emit messageRequested(QStringLiteral("Extraction in progress"));
        return;
    }
    const QList<QTreeWidgetItem*> items = selectedItems();
    QStringList members;
    QStringList normalized;
    for (QTreeWidgetItem* item : items) {
        if (!isEntryItem(item)) continue;
        const QString norm = item->data(0, kEntryPathRole).toString();
        const QString member = item->data(0, kMemberPathRole).toString();
        if (norm.isEmpty() || member.isEmpty()) continue;
        normalized << norm;
        members << member;
    }
    if (normalized.isEmpty()) return;
    dropNestedSelectedPaths(members, normalized);
    if (normalized.isEmpty()) return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    std::vector<std::filesystem::path> real;
    QString err;
    const bool ok = extractToCache(members, normalized, false, real, err);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        if (!err.isEmpty()) TuiConfirmPopup::alert(this, "Extraction Error", err);
        return;
    }

    auto* mimeData = new QMimeData();
    QList<QUrl> urls;
    QStringList plain;
    QByteArray gnomeCopiedData = "copy\n";
    for (const auto& p : real) {
        const QString qPath = QString::fromUtf8(p.c_str());
        const QUrl url = QUrl::fromLocalFile(qPath);
        urls.append(url);
        plain.append(qPath);
        gnomeCopiedData += url.toEncoded() + "\n";
    }
    mimeData->setUrls(urls);
    mimeData->setText(plain.join(QLatin1Char('\n')).trimmed());
    mimeData->setData("x-special/gnome-copied-files", gnomeCopiedData);

    if (real.size() == 1) {
        std::string ext = real.front().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
            ext == ".webp" || ext == ".bmp" || ext == ".gif") {
            QImageReader reader(QString::fromUtf8(real.front().c_str()));
            reader.setAutoTransform(true);
            const QImage img = reader.read();
            if (!img.isNull()) mimeData->setImageData(img);
        }
    }

    QApplication::clipboard()->setMimeData(mimeData);
    emit messageRequested(QStringLiteral("Copy %1").arg(normalized.size()));
}

void ArchiveTreeWidget::buildFromEntries(const std::vector<Core::XdgUtils::ArchiveEntry>& entries) {
    if (m_archivePath.empty() && m_model) m_archivePath = m_model->archivePath();

    setUpdatesEnabled(false);
    resetSelectionState();
    clear();
    auto rootNode = buildTree(entries);

    auto* rootItem = new QTreeWidgetItem(this);
    const QString archiveName = m_archivePath.empty()
        ? QStringLiteral("Archive")
        : QString::fromUtf8(m_archivePath.filename().c_str());
    const auto rootIcon = Icons::getIcon(m_archivePath, true, false, true,
                                         std::optional<bool>(false));
    rootItem->setText(0, archiveName);
    rootItem->setData(0, kEntryPathRole, QString());
    rootItem->setData(0, kMemberPathRole, QString());
    rootItem->setData(0, kIsDirRole, true);
    rootItem->setData(0, kIconGlyphRole, rootIcon.glyph);
    rootItem->setData(0, kIconColorRole, rootIcon.color);
    rootItem->setToolTip(0, QString::fromUtf8(m_archivePath.c_str()));

    std::function<void(ArchiveNode*, QTreeWidgetItem*)> addNode =
        [&](ArchiveNode* node, QTreeWidgetItem* parentItem) {
            auto* item = new QTreeWidgetItem(parentItem);
            item->setText(0, node->name);
            item->setText(1, node->isDir ? QString()
                                         : Core::XdgUtils::formatSize(node->size));
            item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
            item->setData(0, kEntryPathRole, node->entryPath);
            item->setData(0, kMemberPathRole, node->memberPath);
            item->setData(0, kIsDirRole, node->isDir);
            const auto icon = iconForName(node->name, node->isDir);
            item->setData(0, kIconGlyphRole, icon.glyph);
            item->setData(0, kIconColorRole, icon.color);
            item->setToolTip(0, node->entryPath);
            if (!node->isDir && node->size > 0) {
                item->setToolTip(1, Core::XdgUtils::formatSize(node->size));
            }
            for (ArchiveNode* child : sortedChildren(node)) {
                addNode(child, item);
            }
        };

    for (ArchiveNode* child : sortedChildren(rootNode.get())) {
        addNode(child, rootItem);
    }

    rootItem->setExpanded(true);
    expandAll();
    if (rootItem->childCount() > 0) {
        setCurrentItem(rootItem->child(0), 0, QItemSelectionModel::NoUpdate);
        m_selectionAnchor = rootItem->child(0);
        scrollToItem(rootItem, QAbstractItemView::PositionAtTop);
    } else {
        setCurrentItem(rootItem, 0, QItemSelectionModel::NoUpdate);
        m_selectionAnchor = nullptr;
    }
    setUpdatesEnabled(true);
    viewport()->update();
}

bool ArchiveTreeWidget::extractEntryForPreview(const QString& normalizedPath,
                                               const QString& memberPath,
                                               std::filesystem::path& outPath,
                                               QString& error) {
    outPath.clear();
    error.clear();
    if (normalizedPath.isEmpty() || memberPath.isEmpty()) return false;
    std::vector<std::filesystem::path> real;
    if (!extractToCache({memberPath}, {normalizedPath}, false, real, error)) return false;
    if (real.empty()) return false;
    outPath = real.front();
    return true;
}

void ArchiveTreeWidget::activateItem(QTreeWidgetItem* item) {
    if (!item) return;
    if (item->data(0, kIsDirRole).toBool()) {
        item->setExpanded(!item->isExpanded());
        return;
    }
    openEntry(item);
}

void ArchiveTreeWidget::openEntry(QTreeWidgetItem* item) {
    if (!item) return;
    if (m_extracting) {
        emit messageRequested(QStringLiteral("Extraction in progress"));
        return;
    }
    const QString norm = item->data(0, kEntryPathRole).toString();
    const QString member = item->data(0, kMemberPathRole).toString();
    if (norm.isEmpty() || member.isEmpty()) return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    std::vector<std::filesystem::path> real;
    QString err;
    const bool ok = extractToCache({member}, {norm}, false, real, err);
    QApplication::restoreOverrideCursor();

    if (!ok || real.empty()) {
        TuiConfirmPopup::alert(this, "Extraction Error",
                               err.isEmpty() ? QStringLiteral("cannot extract entry") : err);
        return;
    }
    Core::XdgUtils::openWithDefault(real.front());
}

bool ArchiveTreeWidget::extractToCache(const QStringList& members, const QStringList& normalizedPaths,
                                       bool abortOnRelease,
                                       std::vector<std::filesystem::path>& outRealPaths,
                                       QString& outError) {
    outRealPaths.clear();
    outError.clear();
    if (m_archivePath.empty() || normalizedPaths.isEmpty()) return false;
    if (m_extracting) {
        outError = QStringLiteral("Extraction already in progress");
        return false;
    }
    m_extracting = true;
    struct ExtractionReset {
        bool& flag;
        ~ExtractionReset() { flag = false; }
    } extractionReset{m_extracting};

    // прошлая распаковка могла остаться в фоне после отпускания кнопки и ждем ее чтобы не принять недописанные файлы за готовые
    const QString archiveKey = QString::fromUtf8(m_archivePath.c_str());
    const auto pendingIt = m_pendingExtractions.constFind(archiveKey);
    if (pendingIt != m_pendingExtractions.constEnd()) {
        const Core::Tasks::TaskId pending = pendingIt.value();
        Core::Tasks::TaskManager& tm = Core::Tasks::TaskManager::instance();
        auto isPending = [&tm, pending]() {
            const Core::Tasks::TaskInfo info = tm.task(pending);
            return info.id != 0 &&
                   (info.state == Core::Tasks::State::Queued ||
                    info.state == Core::Tasks::State::Running);
        };
        if (isPending()) {
            QEventLoop waitLoop;
            QTimer waitPoll;
            waitPoll.setInterval(15);
            QObject::connect(&waitPoll, &QTimer::timeout, &waitLoop, [&]() {
                if (!isPending()) waitLoop.quit();
            });
            QTimer::singleShot(30000, &waitLoop, &QEventLoop::quit);
            waitPoll.start();
            waitLoop.exec();
            waitPoll.stop();
        }
        m_pendingExtractions.remove(archiveKey);
    }

    cleanupOldArchiveCacheOnce();
    const std::filesystem::path cacheDir = archiveCacheDir(m_archivePath);
    std::error_code ec;
    std::filesystem::create_directories(cacheDir, ec);
    if (ec) {
        outError = QStringLiteral("Cannot create cache folder: %1")
                       .arg(QString::fromUtf8(ec.message().c_str()));
        return false;
    }

    const bool singleStream = Core::XdgUtils::isSingleStreamArchive(m_archivePath);

    // дозагружаем только отсутствующее
    bool needExtract = false;
    QStringList extractMembers;
    if (singleStream) {
        const QDir d(QString::fromUtf8(cacheDir.c_str()));
        needExtract = d.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).isEmpty();
    } else {
        for (int i = 0; i < normalizedPaths.size(); ++i) {
            std::error_code e2;
            const auto real = cacheDir / normalizedPaths.at(i).toUtf8().toStdString();
            if (!std::filesystem::exists(real, e2)) {
                needExtract = true;
                if (i < members.size()) extractMembers << members.at(i);
            }
        }
    }

    if (needExtract) {
        struct ExtractState {
            QMutex mutex;
            bool done{false};
            bool ok{false};
            QString error;
        };
        auto state = std::make_shared<ExtractState>();
        const Core::Tasks::TaskId taskId =
            Core::Tasks::TaskManager::instance().submit(
                QStringLiteral("Extract %1 for preview")
                    .arg(QString::fromUtf8(m_archivePath.filename().c_str())),
                QStringLiteral("preview"),
                [state, archive = m_archivePath, extractMembers, normalizedPaths, cacheDir](
                    Core::Tasks::TaskContext& ctx) {
                    QString err;
                    bool ok = false;
                    try {
                        ok = extractMembers.isEmpty()
                            ? Core::XdgUtils::extractArchive(archive, cacheDir, err, nullptr,
                                                             ctx.cancelToken())
                            : Core::XdgUtils::extractArchiveEntries(archive, extractMembers,
                                                                    cacheDir, err,
                                                                    ctx.cancelToken());
                    } catch (const std::exception& e) {
                        err = QString::fromUtf8(e.what());
                    } catch (...) {
                        err = QStringLiteral("Archive extraction failed");
                    }
                    if (!ok) {
                        std::error_code ec;
                        if (extractMembers.isEmpty()) {
                            std::filesystem::remove_all(cacheDir, ec);
                        } else {
                            for (const QString& p : normalizedPaths) {
                                std::error_code rmEc;
                                std::filesystem::remove_all(
                                    cacheDir / p.toUtf8().toStdString(), rmEc);
                            }
                        }
                    }
                    if (ok) {
                        ctx.log(QStringLiteral("prepared archive preview"));
                    } else if (!ctx.cancelled() && !err.isEmpty()) {
                        ctx.log(err);
                        ctx.fail(err);
                    }
                    ctx.progress(ctx.cancelled() ? 0 : 1, 1);
                    QMutexLocker lock(&state->mutex);
                    state->done = true;
                    state->ok = ok;
                    state->error = err;
                });

        // вложенный цикл событий держит интерфейс живым и при отпускании кнопки ожидание прекращается
        QEventLoop loop;
        QTimer poll;
        poll.setInterval(15);
        bool aborted = false;
        QObject::connect(&poll, &QTimer::timeout, &loop, [&]() {
            QMutexLocker lock(&state->mutex);
            if (state->done) {
                loop.quit();
            } else if (abortOnRelease && !m_gestureButtonDown) {
                aborted = true;
                loop.quit();
            }
        });
        poll.start();
        loop.exec();
        poll.stop();

        if (aborted) {
            // кнопку отпустили раньше завершения распаковки но процессы не убиваем пусть догреют кэш
            m_pendingExtractions.insert(archiveKey, taskId);
            return false;
        }
        {
            QMutexLocker lock(&state->mutex);
            if (!state->ok) {
                outError = state->error;
                return false;
            }
        }
    }

    if (singleStream) {
        const QDir d(QString::fromUtf8(cacheDir.c_str()));
        const QFileInfoList infos = d.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo& fi : infos) {
            outRealPaths.emplace_back(fi.absoluteFilePath().toUtf8().toStdString());
        }
    } else {
        for (const QString& n : normalizedPaths) {
            std::error_code e2;
            const auto real = cacheDir / n.toUtf8().toStdString();
            if (std::filesystem::exists(real, e2)) outRealPaths.push_back(real);
        }
    }
    return !outRealPaths.empty();
}

void ArchiveTreeWidget::startDrag(Qt::DropActions /*supportedActions*/) {
    if (m_extracting) {
        emit messageRequested(QStringLiteral("Extraction in progress"));
        return;
    }
    QList<QTreeWidgetItem*> items = selectedItems();
    if (items.isEmpty() && currentItem()) items << currentItem();

    QStringList members;
    QStringList normalized;
    for (QTreeWidgetItem* item : items) {
        if (!item) continue;
        const QString norm = item->data(0, kEntryPathRole).toString();
        const QString member = item->data(0, kMemberPathRole).toString();
        if (norm.isEmpty() || member.isEmpty()) continue;
        normalized << norm;
        members << member;
    }
    if (normalized.isEmpty()) return;
    dropNestedSelectedPaths(members, normalized);
    if (normalized.isEmpty()) return;

    const bool abortOnRelease = m_gestureButtonDown;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    std::vector<std::filesystem::path> real;
    QString err;
    const bool ok = extractToCache(members, normalized, abortOnRelease, real, err);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        if (!err.isEmpty()) TuiConfirmPopup::alert(this, "Extraction Error", err);
        return;
    }
    if (abortOnRelease &&
        (!m_gestureButtonDown || !(QGuiApplication::mouseButtons() & Qt::LeftButton))) {
        return;
    }

    QDrag* drag = Dnd::DragDropHandler::createDrag(real, this);
    if (drag) {
        // из архива идет только копирование так как реальные файлы лежат во временном кэше
        QTimer releaseWatchdog;
        releaseWatchdog.setSingleShot(true);
        releaseWatchdog.setInterval(50);
        QObject::connect(&releaseWatchdog, &QTimer::timeout, []() {
            if (!(QGuiApplication::mouseButtons() & Qt::LeftButton)) QDrag::cancel();
        });
        releaseWatchdog.start();
        drag->exec(Qt::CopyAction, Qt::CopyAction);
        releaseWatchdog.stop();
        m_gestureButtonDown = false;
    }
}

void ArchiveTreeWidget::drawBranches(QPainter* painter, const QRect& rect,
                                     const QModelIndex& index) const {
    // делегат заливает только ячейки а полосу отступа и стрелки рисует вью иначе слева будет темный обрубок
    const bool selected = index.isValid() && selectionModel() &&
                          selectionModel()->isSelected(index);
    const QColor bg = selected ? Theme::Colors::SelectionBg() : Theme::Colors::Base();
    painter->fillRect(rect, bg);
    QTreeWidget::drawBranches(painter, rect, index);

    // стандартная стрелка цветом текста на светлом выделении не видна поэтому перерисовываем ее контрастным цветом
    if (index.isValid() && model() && model()->hasChildren(index)) {
        const QRect patch(rect.right() - 15, rect.top(), 15, rect.height());
        painter->fillRect(patch, bg);

        const qreal cx = patch.center().x();
        const qreal cy = patch.center().y();
        QPolygonF arrow;
        if (isExpanded(index)) {
            arrow << QPointF(cx - 4, cy - 2) << QPointF(cx + 4, cy - 2)
                  << QPointF(cx, cy + 3);
        } else {
            arrow << QPointF(cx - 2, cy - 4) << QPointF(cx + 3, cy)
                  << QPointF(cx - 2, cy + 4);
        }
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(Qt::NoPen);
        painter->setBrush(selected ? Theme::Colors::SelectionFg() : Theme::Colors::Text());
        painter->drawPolygon(arrow);
        painter->restore();
    }
}

void ArchiveTreeWidget::beginBoxSelection(const QPoint& origin, bool additive) {
    m_isRubberBanding = true;
    m_boxAdditive = additive;
    m_rubberBandOrigin = origin;
    m_preserveAnchor = true;
    if (!m_rubberBand) {
        m_rubberBand = new QRubberBand(QRubberBand::Rectangle, viewport());
    }
    m_rubberBand->setGeometry(QRect(m_rubberBandOrigin, QSize()));
    m_rubberBand->show();
    if (!additive) {
        clearSelection();
        if (selectionModel()) selectionModel()->clearCurrentIndex();
    }
}

void ArchiveTreeWidget::updateBoxSelection(const QPoint& pos) {
    QPoint clamped = pos;
    clamped.setX(std::clamp(clamped.x(), 0, viewport()->width()));
    clamped.setY(std::clamp(clamped.y(), 0, viewport()->height()));
    const QRect box = QRect(m_rubberBandOrigin, clamped).normalized();
    if (m_rubberBand) m_rubberBand->setGeometry(box);

    if (pos.y() < 20) {
        verticalScrollBar()->triggerAction(QAbstractSlider::SliderSingleStepSub);
    } else if (pos.y() > viewport()->height() - 20) {
        verticalScrollBar()->triggerAction(QAbstractSlider::SliderSingleStepAdd);
    }

    QItemSelection selection;
    QTreeWidgetItemIterator it(this);
    for (; *it; ++it) {
        QTreeWidgetItem* item = *it;
        if (!isVisibleEntry(item)) continue;
        const QRect itemRect = visualItemRect(item);
        if (!itemRect.isValid() || itemRect.isEmpty() || !box.intersects(itemRect)) {
            continue;
        }
        const QModelIndex idx = indexFromItem(item, 0);
        if (idx.isValid()) selection.select(idx, idx);
    }

    if (selection.isEmpty()) {
        if (!m_boxAdditive) selectionModel()->clearSelection();
    } else {
        selectionModel()->select(selection, (m_boxAdditive ? QItemSelectionModel::Select
                                                            : QItemSelectionModel::ClearAndSelect)
                                                 | QItemSelectionModel::Rows);
    }
}

void ArchiveTreeWidget::finalizeBoxSelection() {
    QTreeWidgetItem* anchor = nullptr;
    QTreeWidgetItemIterator it(this);
    for (; *it; ++it) {
        if (isVisibleEntry(*it) && (*it)->isSelected()) {
            anchor = *it;
            break;
        }
    }
    m_selectionAnchor = anchor;
    if (anchor) {
        setCurrentItem(anchor, 0, QItemSelectionModel::NoUpdate);
    } else if (selectionModel()) {
        selectionModel()->clearCurrentIndex();
    }
    m_preserveAnchor = false;
}

void ArchiveTreeWidget::selectRangeTo(QTreeWidgetItem* target, bool additive) {
    if (!isVisibleEntry(target)) return;

    QTreeWidgetItem* anchor = isVisibleEntry(m_selectionAnchor) ? m_selectionAnchor
                                                                : currentItem();
    if (!isVisibleEntry(anchor)) anchor = target;
    m_selectionAnchor = anchor;

    QList<QTreeWidgetItem*> rows;
    int anchorIndex = -1;
    int targetIndex = -1;
    QTreeWidgetItemIterator it(this);
    for (; *it; ++it) {
        if (!isVisibleEntry(*it)) continue;
        rows.append(*it);
        if (*it == anchor) anchorIndex = rows.size() - 1;
        if (*it == target) targetIndex = rows.size() - 1;
    }
    if (anchorIndex < 0 || targetIndex < 0) return;

    const int first = std::min(anchorIndex, targetIndex);
    const int last = std::max(anchorIndex, targetIndex);
    QItemSelection selection;
    for (int i = first; i <= last; ++i) {
        const QModelIndex idx = indexFromItem(rows.at(i), 0);
        if (idx.isValid()) selection.select(idx, idx);
    }

    m_preserveAnchor = true;
    if (selectionModel()) {
        setCurrentItem(target, 0, QItemSelectionModel::NoUpdate);
        selectionModel()->select(selection, (additive ? QItemSelectionModel::Select
                                                      : QItemSelectionModel::ClearAndSelect)
                                               | QItemSelectionModel::Rows);
    }
    m_preserveAnchor = false;
    scrollToItem(target, QAbstractItemView::EnsureVisible);
}

void ArchiveTreeWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_gestureButtonDown = true;
        m_boxGesturePending = false;
        m_shiftSelecting = false;
        emit selectionInteractionStarted();
        const bool inRightGutter = event->pos().x() >= viewport()->width() - 18;
        QTreeWidgetItem* item = itemAt(event->pos());
        if (item && (event->modifiers() & Qt::ShiftModifier)) {
            m_isRubberBanding = false;
            m_boxGesturePending = false;
            m_shiftSelecting = true;
            selectRangeTo(item, event->modifiers().testFlag(Qt::ControlModifier));
            setFocus(Qt::MouseFocusReason);
            event->accept();
            return;
        }
        if (!item || inRightGutter) {
            beginBoxSelection(event->pos(), event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier));
            setFocus(Qt::MouseFocusReason);
            event->accept();
            return;
        }
        const QRect itemRect = visualItemRect(item);
        const bool inLeftGutter = event->pos().x() < 16 ||
            (itemRect.isValid() && event->pos().x() < itemRect.left() + 4);
        if (inLeftGutter) {
            m_isRubberBanding = false;
            m_pressPos = event->pos();
            m_boxGesturePending = true;
            m_boxAdditive = event->modifiers().testFlag(Qt::ControlModifier);
            QTreeWidget::mousePressEvent(event);
            if (isEntryItem(item)) m_selectionAnchor = item;
            return;
        }
        m_isRubberBanding = false;
        m_pressPos = event->pos();
        m_boxAdditive = event->modifiers().testFlag(Qt::ControlModifier);
        QTreeWidget::mousePressEvent(event);
        if (isEntryItem(item)) m_selectionAnchor = item;
        return;
    }
    if (event->button() == Qt::RightButton) {
        m_isRubberBanding = false;
        m_boxGesturePending = false;
        m_shiftSelecting = false;
        m_preserveAnchor = false;
        if (m_rubberBand) m_rubberBand->hide();
        emit selectionInteractionStarted();
        QTreeWidgetItem* item = itemAt(event->pos());
        if (item) {
            if (!item->isSelected()) {
                clearSelection();
                setCurrentItem(item, 0, QItemSelectionModel::ClearAndSelect |
                                         QItemSelectionModel::Rows);
            } else {
                setCurrentItem(item, 0, QItemSelectionModel::NoUpdate);
            }
            m_selectionAnchor = isEntryItem(item) ? item : nullptr;
        } else {
            clearSelection();
            m_selectionAnchor = nullptr;
            if (selectionModel()) selectionModel()->clearCurrentIndex();
        }
        setFocus(Qt::MouseFocusReason);
        event->accept();
        return;
    }
    QTreeWidget::mousePressEvent(event);
}

void ArchiveTreeWidget::mouseMoveEvent(QMouseEvent* event) {
    if (!(event->buttons() & Qt::LeftButton)) {
        m_boxGesturePending = false;
    }
    if (m_shiftSelecting && (event->buttons() & Qt::LeftButton)) {
        QTreeWidgetItem* hovered = itemAt(event->pos());
        if (isVisibleEntry(hovered)) {
            selectRangeTo(hovered, event->modifiers().testFlag(Qt::ControlModifier));
        }
        event->accept();
        return;
    }
    if (m_boxGesturePending && (event->buttons() & Qt::LeftButton)) {
        if ((event->pos() - m_pressPos).manhattanLength() < QApplication::startDragDistance()) {
            event->accept();
            return;
        }
        m_boxGesturePending = false;
        beginBoxSelection(m_pressPos, m_boxAdditive);
    }
    if (m_isRubberBanding && (event->buttons() & Qt::LeftButton)) {
        updateBoxSelection(event->pos());
        event->accept();
        return;
    }
    QTreeWidget::mouseMoveEvent(event);
}

void ArchiveTreeWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_gestureButtonDown = false;
        m_boxGesturePending = false;
    }
    if (m_shiftSelecting) {
        m_shiftSelecting = false;
        event->accept();
        return;
    }
    if (m_isRubberBanding) {
        m_isRubberBanding = false;
        if (m_rubberBand) m_rubberBand->hide();
        finalizeBoxSelection();
        event->accept();
        return;
    }
    QTreeWidget::mouseReleaseEvent(event);
}

void ArchiveTreeWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_gestureButtonDown = true;
        m_shiftSelecting = false;
        m_boxGesturePending = false;
        emit selectionInteractionStarted();
        const bool inRightGutter = event->pos().x() >= viewport()->width() - 18;
        QTreeWidgetItem* item = itemAt(event->pos());
        if (item) {
            const QRect itemRect = visualItemRect(item);
            const bool inGutter = inRightGutter || event->pos().x() < 16 ||
                (itemRect.isValid() && event->pos().x() < itemRect.left() + 4);
            if (inGutter) {
                event->accept();
                return;
            }
            activateItem(item);
            event->accept();
            return;
        }
    }
    QTreeWidget::mouseDoubleClickEvent(event);
}

void ArchiveTreeWidget::focusInEvent(QFocusEvent* event) {
    if (event->reason() != Qt::PopupFocusReason) emit selectionInteractionStarted();
    QTreeWidget::focusInEvent(event);
}

void ArchiveTreeWidget::keyPressEvent(QKeyEvent* event) {
    const int k = event->key();
    const bool isNav = k == Qt::Key_Up || k == Qt::Key_Down ||
                       k == Qt::Key_Home || k == Qt::Key_End ||
                       k == Qt::Key_PageUp || k == Qt::Key_PageDown;
    if (isNav || k == Qt::Key_Space || k == Qt::Key_Return || k == Qt::Key_Enter) {
        emit selectionInteractionStarted();
    }
    if (k == Qt::Key_A && (event->modifiers() & Qt::ControlModifier)) {
        emit selectionInteractionStarted();
        selectAllEntries();
        event->accept();
        return;
    }
    if (k == Qt::Key_C && (event->modifiers() & Qt::ControlModifier) &&
        !(event->modifiers() & Qt::ShiftModifier)) {
        emit selectionInteractionStarted();
        copySelectedEntries();
        event->accept();
        return;
    }
    if (k == Qt::Key_Escape) {
        clearEntrySelection();
        event->accept();
        return;
    }
    if (k == Qt::Key_Return || k == Qt::Key_Enter) {
        activateItem(currentItem());
        return;
    }
    if (k == Qt::Key_Space) {
        QTreeWidgetItem* cur = currentItem();
        if (cur) {
            cur->setSelected(!cur->isSelected());
            QTreeWidgetItem* next = itemBelow(cur);
            if (next) setCurrentItem(next, 0, QItemSelectionModel::NoUpdate);
        }
        return;
    }
    QTreeWidget::keyPressEvent(event);
}

void ArchiveTreeWidget::showArchiveMenu(const QPoint& pos) {
    QTreeWidgetItem* item = itemAt(pos);
    if (item && !item->isSelected()) {
        clearSelection();
        setCurrentItem(item, 0, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    }
    m_selectionAnchor = isEntryItem(item) ? item : nullptr;

    auto* menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setFont(Theme::getMonospaceFont(9));
    menu->setStyleSheet(Theme::getApplicationStyleSheet());

    if (item) {
        auto* actOpen = menu->addAction("\U000F0214 Open\tEnter");
        connect(actOpen, &QAction::triggered, this, [this, item]() { activateItem(item); });
    }

    QStringList selectedMembers;
    QStringList selectedNormalized;
    const QList<QTreeWidgetItem*> selected = selectedItems();
    for (QTreeWidgetItem* selectedItem : selected) {
        if (!isEntryItem(selectedItem)) continue;
        const QString norm = selectedItem->data(0, kEntryPathRole).toString();
        const QString member = selectedItem->data(0, kMemberPathRole).toString();
        if (norm.isEmpty() || member.isEmpty()) continue;
        selectedNormalized << norm;
        selectedMembers << member;
    }
    dropNestedSelectedPaths(selectedMembers, selectedNormalized);
    const int selectedEntryCount = selectedNormalized.size();
    if (selectedEntryCount > 0) {
        const QString copyLabel = selectedEntryCount == 1
            ? QStringLiteral("\U000F0337 Copy 1 Item\tCtrl+C")
            : QStringLiteral("\U000F0337 Copy %1 Items\tCtrl+C").arg(selectedEntryCount);
        auto* actCopyItems = menu->addAction(copyLabel);
        connect(actCopyItems, &QAction::triggered, this,
                [this]() { copySelectedEntries(); });
    }
    if (item || selectedEntryCount > 0) menu->addSeparator();

    auto* actExtract = menu->addAction("\U000F06EB Extract In");
    connect(actExtract, &QAction::triggered, this, [this]() {
        const std::filesystem::path arc = m_archivePath;
        if (arc.empty()) return;
        const std::filesystem::path dest =
            arc.parent_path() / Core::XdgUtils::archiveExtractDirName(arc);
        QPointer<ArchiveTreeWidget> self(this);
        Core::Tasks::TaskManager::instance().submit(
            QStringLiteral("Extract %1").arg(QString::fromUtf8(arc.filename().c_str())),
            QStringLiteral("preview"),
            [self, arc, dest](Core::Tasks::TaskContext& ctx) {
                QString err;
                std::filesystem::path actualDest;
                bool ok = false;
                try {
                    ok = Core::XdgUtils::extractArchive(arc, dest, err, &actualDest,
                                                        ctx.cancelToken());
                } catch (const std::exception& e) {
                    err = QString::fromUtf8(e.what());
                } catch (...) {
                    err = QStringLiteral("Archive extraction failed");
                }
                const bool cancelled = ctx.cancelled();
                if (ok) {
                    ctx.log(QStringLiteral("extracted to %1")
                                .arg(QString::fromUtf8(actualDest.c_str())));
                } else if (!cancelled && !err.isEmpty()) {
                    ctx.log(err);
                    ctx.fail(err);
                }
                ctx.progress(cancelled ? 0 : 1, 1);
                QMetaObject::invokeMethod(qApp, [self, ok, cancelled, err]() {
                    if (!self || cancelled) return;
                    if (!ok) TuiConfirmPopup::alert(self.data(), "Extraction Error", err);
                }, Qt::QueuedConnection);
            });
    });

    auto* actCopy = menu->addAction("\U000F0337 Copy Archive Path");
    connect(actCopy, &QAction::triggered, this, [this]() {
        if (!m_archivePath.empty()) {
            QApplication::clipboard()->setText(QString::fromUtf8(m_archivePath.c_str()));
        }
    });

    menu->addSeparator();
    auto* actSelectAll = menu->addAction("Select All\tCtrl+A");
    connect(actSelectAll, &QAction::triggered, this, [this]() { selectAllEntries(); });
    auto* actExpand = menu->addAction("Expand All");
    connect(actExpand, &QAction::triggered, this, [this]() { expandAll(); });
    auto* actCollapse = menu->addAction("Collapse All");
    connect(actCollapse, &QAction::triggered, this, [this]() {
        if (topLevelItemCount() > 0) {
            for (int i = 0; i < topLevelItemCount(); ++i) {
                topLevelItem(i)->setExpanded(false);
            }
        }
    });

    menu->popup(viewport()->mapToGlobal(pos));
}

} // namespace Widgets
