#include "PreviewPane.hpp"
#include "NativePlugins.hpp"
#include "../atlifm-core/Plugins.hpp"
#include "MillerColumns.hpp"
#include "FileItemDelegate.hpp"
#include "ContextMenu.hpp"
#include "TuiInputPopup.hpp"
#include "../atlifm-theme/Theme.hpp"
#include "../atlifm-core/XdgUtils.hpp"
#include "../atlifm-core/tasks/TaskManager.hpp"
#include "../atlifm-core/Icons.hpp"
#include "../atlifm-core/SearchUtils.hpp"
#include "../atlifm-core/Thumbnails.hpp"
#include "../atlifm-models/ArchiveModel.hpp"
#include "ArchiveTreeWidget.hpp"
#include "../atlifm-dnd/DragDropHandler.hpp"
#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPainter>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QRegularExpression>
#include <QPixmap>
#include <QImageReader>
#include <sys/stat.h>
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QApplication>
#include <QClipboard>
#include <QThreadPool>
#include <QTimer>
#include <QPointer>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QPushButton>
#include <QFrame>
#include <QGridLayout>
#include <QMenu>
#include <QDir>
#include <QMutex>
#include <QMutexLocker>
#include <QEventLoop>
#include <QCryptographicHash>
#include <QStandardPaths>
#include <algorithm>
#include <cstddef>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <ctime>



namespace Widgets {

namespace {
// Превью декодируется не в полном разрешении: панель всё равно показывает
// несколько сотен пикселей а оригинал высокого разрешения занимает десятки мегабайт
// Оригинал подгружается фоном только когда пользователь зумит в изображение.
constexpr int kPreviewMaxSide = 1280;
constexpr int kFullResMaxSide = 4096;
constexpr qint64 kImageCacheBudgetBytes = 16LL * 1024 * 1024;
constexpr int kImageCacheMaxEntries = 4;
constexpr qint64 kScaledPixmapBudgetBytes = 16LL * 1024 * 1024;
// ширина рендера страницы PDF: панель превью заметно уже, а каждый лишний
// пиксель страницы стоит памяти и времени рендера
constexpr int kPdfPageRenderWidth = 720;
// полноразмерные кадры нужны только последним страницам (пересчёт при
// изменении ширины панели), остальным достаточно pixmap в QLabel
constexpr std::size_t kMaxCachedPdfPageImages = 2;
// оригинал подгружается когда уменьшенное превью начинают растягивать
// выше его собственных пикселей с небольшим запасом
constexpr double kFullResTriggerScale = 0.9;

QSize boundedSize(const QSize& size, int maxSide) {
    if (!size.isValid() || size.isEmpty()) return size;
    if (size.width() <= maxSide && size.height() <= maxSide) return size;
    return size.scaled(maxSide, maxSide, Qt::KeepAspectRatio);
}

// glibc оставляет освобождённые крупные декоды в арене: после фото 6000x4000
// это ~20 МБ rss навсегда, поэтому отдаём свободные страницы ядру сразу
void releaseDecodeBuffers() {
#if defined(__GLIBC__)
    malloc_trim(0);
#endif
}
}

// встроенный редактор кода с номерами строк


class SimpleSyntaxHighlighter : public QSyntaxHighlighter {
    Q_OBJECT

public:
    explicit SimpleSyntaxHighlighter(QTextDocument* parent = nullptr);
    void setLanguage(const QString& ext);

protected:
    void highlightBlock(const QString& text) override;

private:
    struct HighlightingRule {
        QRegularExpression pattern;
        QTextCharFormat format;
    };
    std::vector<HighlightingRule> m_rules;
    QTextCharFormat m_commentFormat;
    QRegularExpression m_commentPattern;
};

class LineNumberArea;

class CodeEditorWithLineNumbers : public QPlainTextEdit {
    Q_OBJECT

public:
    explicit CodeEditorWithLineNumbers(QWidget* parent = nullptr);

    void lineNumberAreaPaintEvent(QPaintEvent* event);
    int lineNumberAreaWidth();
    void setSourceCode(const QString& code, const QString& ext);

protected:
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void updateLineNumberAreaWidth(int newBlockCount);
    void updateLineNumberArea(const QRect& rect, int dy);

private:
    LineNumberArea* m_lineNumberArea{nullptr};
    SimpleSyntaxHighlighter* m_highlighter{nullptr};
};

class LineNumberArea : public QWidget {
public:
    explicit LineNumberArea(CodeEditorWithLineNumbers* editor)
        : QWidget(editor), m_editor(editor) {}

    QSize sizeHint() const override {
        return QSize(m_editor->lineNumberAreaWidth(), 0);
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        m_editor->lineNumberAreaPaintEvent(event);
    }

private:
    CodeEditorWithLineNumbers* m_editor;
};



SimpleSyntaxHighlighter::SimpleSyntaxHighlighter(QTextDocument* parent)
    : QSyntaxHighlighter(parent) {
    setLanguage("cpp");
}

void SimpleSyntaxHighlighter::setLanguage(const QString& rawExt) {
    m_rules.clear();

    QString ext = rawExt.trimmed().toLower();
    if (ext.startsWith('.')) {
        ext = ext.mid(1);
    }

    using namespace Theme::Colors;

    QTextCharFormat keywordFormat;
    keywordFormat.setForeground(Mauve());
    keywordFormat.setFontWeight(QFont::Bold);

    QTextCharFormat typeFormat;
    typeFormat.setForeground(Yellow());

    QTextCharFormat stringFormat;
    stringFormat.setForeground(Green());

    QTextCharFormat numberFormat;
    numberFormat.setForeground(Peach());

    m_commentFormat.setForeground(Dim());
    m_commentFormat.setFontItalic(true);

    QStringList keywords;
    QStringList types;

    if (ext == "rs") {
        keywords = {"as", "break", "const", "continue", "crate", "else", "enum", "extern",
                    "false", "fn", "for", "if", "impl", "in", "let", "loop", "match", "mod",
                    "move", "mut", "pub", "ref", "return", "self", "Self", "static", "struct",
                    "super", "trait", "true", "type", "unsafe", "use", "where", "while", "async", "await"};
        types = {"i8", "i16", "i32", "i64", "i128", "isize", "u8", "u16", "u32", "u64", "u128",
                 "usize", "f32", "f64", "str", "bool", "char", "Option", "Result", "String", "Vec"};
        m_commentPattern = QRegularExpression(R"(//[^\n]*)");
    } else if (ext == "py") {
        keywords = {"and", "as", "assert", "async", "await", "break", "class", "continue", "def",
                    "del", "elif", "else", "except", "False", "finally", "for", "from", "global",
                    "if", "import", "in", "is", "lambda", "None", "nonlocal", "not", "or",
                    "pass", "raise", "return", "True", "try", "while", "with", "yield"};
        types = {"int", "float", "str", "list", "dict", "set", "tuple", "bool", "bytes"};
        m_commentPattern = QRegularExpression(R"(#[^\n]*)");
    } else if (ext == "sh" || ext == "bash" || ext == "zsh" || ext == "toml" || ext == "yaml" || ext == "yml" || ext == "conf" || ext == "ini") {
        keywords = {"if", "then", "else", "elif", "fi", "case", "esac", "for", "while", "until",
                    "do", "done", "in", "function", "select", "time", "return", "exit", "export", "local", "true", "false"};
        types = {"echo", "printf", "cat", "grep", "sed", "awk", "find", "mkdir", "rm", "cp", "mv"};
        m_commentPattern = QRegularExpression(R"(#[^\n]*)");
    } else if (ext == "json") {
        keywords = {"true", "false", "null"};
        m_commentPattern = QRegularExpression(R"(//[^\n]*)");
    } else {
        // ветка по умолчанию для C/C++/JS/TS
        keywords = {"alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor",
                    "bool", "break", "case", "catch", "char", "char8_t", "char16_t", "char32_t",
                    "class", "compl", "concept", "const", "consteval", "constexpr", "constinit",
                    "const_cast", "continue", "co_await", "co_return", "co_yield", "decltype",
                    "default", "delete", "do", "double", "dynamic_cast", "else", "enum",
                    "explicit", "export", "extern", "false", "float", "for", "friend", "goto",
                    "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept",
                    "not", "not_eq", "nullptr", "operator", "or", "or_eq", "private", "protected",
                    "public", "register", "reinterpret_cast", "requires", "return", "short",
                    "signed", "sizeof", "static", "static_assert", "static_cast", "struct",
                    "switch", "template", "this", "thread_local", "throw", "true", "try",
                    "typedef", "typeid", "typename", "union", "unsigned", "using", "virtual",
                    "void", "volatile", "wchar_t", "while", "xor", "xor_eq"};
        types = {"std", "string", "vector", "map", "unordered_map", "unique_ptr", "shared_ptr",
                 "optional", "expected", "int32_t", "uint32_t", "int64_t", "uint64_t", "size_t"};
        m_commentPattern = QRegularExpression(R"(//[^\n]*)");
    }

    // ключевые слова и типы в единые regex с альтернацией, так быстрее на строку
    if (!keywords.isEmpty()) {
        HighlightingRule kwRule;
        kwRule.pattern = QRegularExpression(QString(R"(\b(?:%1)\b)").arg(keywords.join('|')));
        kwRule.pattern.optimize();
        kwRule.format = keywordFormat;
        m_rules.push_back(std::move(kwRule));
    }

    if (!types.isEmpty()) {
        HighlightingRule tyRule;
        tyRule.pattern = QRegularExpression(QString(R"(\b(?:%1)\b)").arg(types.join('|')));
        tyRule.pattern.optimize();
        tyRule.format = typeFormat;
        m_rules.push_back(std::move(tyRule));
    }

    HighlightingRule numRule;
    numRule.pattern = QRegularExpression(R"(\b(0x[0-9a-fA-F]+|[0-9]+(\.[0-9]+)?)\b)");
    numRule.pattern.optimize();
    numRule.format = numberFormat;
    m_rules.push_back(std::move(numRule));

    HighlightingRule strRule;
    strRule.pattern = QRegularExpression("\"[^\"\\\\]*(\\\\.[^\"\\\\]*)*\"");
    strRule.pattern.optimize();
    strRule.format = stringFormat;
    m_rules.push_back(std::move(strRule));

    HighlightingRule singleStrRule;
    singleStrRule.pattern = QRegularExpression("'[^'\\\\]*(\\\\.[^'\\\\]*)*'");
    singleStrRule.pattern.optimize();
    singleStrRule.format = stringFormat;
    m_rules.push_back(std::move(singleStrRule));

    if (!m_commentPattern.pattern().isEmpty()) {
        m_commentPattern.optimize();
    }

    rehighlight();
}

void SimpleSyntaxHighlighter::highlightBlock(const QString& text) {
    for (const auto& rule : m_rules) {
        auto matchIterator = rule.pattern.globalMatch(text);
        while (matchIterator.hasNext()) {
            auto match = matchIterator.next();
            setFormat(match.capturedStart(), match.capturedLength(), rule.format);
        }
    }

    if (!m_commentPattern.pattern().isEmpty()) {
        auto matchIterator = m_commentPattern.globalMatch(text);
        while (matchIterator.hasNext()) {
            auto match = matchIterator.next();
            setFormat(match.capturedStart(), match.capturedLength(), m_commentFormat);
        }
    }
}

CodeEditorWithLineNumbers::CodeEditorWithLineNumbers(QWidget* parent)
    : QPlainTextEdit(parent) {
    m_lineNumberArea = new LineNumberArea(this);
    m_highlighter = new SimpleSyntaxHighlighter(document());

    setFont(Theme::getMonospaceFont(10));
    setReadOnly(true);
    setWordWrapMode(QTextOption::NoWrap);

    connect(this, &QPlainTextEdit::blockCountChanged, this, &CodeEditorWithLineNumbers::updateLineNumberAreaWidth);
    connect(this, &QPlainTextEdit::updateRequest, this, &CodeEditorWithLineNumbers::updateLineNumberArea);

    updateLineNumberAreaWidth(0);

    setStyleSheet(QString(R"(
        QPlainTextEdit {
            background-color: transparent;
            border: none;
            color: %1;
        }
    )").arg(Theme::Colors::Dim().name()));
}

int CodeEditorWithLineNumbers::lineNumberAreaWidth() {
    int digits = 1;
    int maxBlock = std::max(1, blockCount());
    while (maxBlock >= 10) {
        maxBlock /= 10;
        ++digits;
    }
    int space = 14 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
    return space;
}

void CodeEditorWithLineNumbers::updateLineNumberAreaWidth(int /* newBlockCount */) {
    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
}

void CodeEditorWithLineNumbers::updateLineNumberArea(const QRect& rect, int dy) {
    if (dy) {
        m_lineNumberArea->scroll(0, dy);
    } else {
        m_lineNumberArea->update(0, rect.y(), m_lineNumberArea->width(), rect.height());
    }

    if (rect.contains(viewport()->rect())) {
        updateLineNumberAreaWidth(0);
    }
}

void CodeEditorWithLineNumbers::resizeEvent(QResizeEvent* e) {
    QPlainTextEdit::resizeEvent(e);
    QRect cr = contentsRect();
    m_lineNumberArea->setGeometry(QRect(cr.left(), cr.top(), lineNumberAreaWidth(), cr.height()));
}

void CodeEditorWithLineNumbers::lineNumberAreaPaintEvent(QPaintEvent* event) {
    QPainter painter(m_lineNumberArea);
    using namespace Theme::Colors;

    painter.fillRect(event->rect(), Mantle());

    // рамка 1px справа у колонки номеров строк
    painter.setPen(QPen(Surface0(), 1));
    painter.drawLine(m_lineNumberArea->width() - 1, event->rect().top(),
                     m_lineNumberArea->width() - 1, event->rect().bottom());

    QTextBlock block = firstVisibleBlock();
    int blockNumber = block.blockNumber();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());

    painter.setFont(font());

    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            QString number = QString::number(blockNumber + 1);
            painter.setPen(Dim());
            painter.drawText(0, top, m_lineNumberArea->width() - 6, fontMetrics().height(),
                             Qt::AlignRight | Qt::AlignVCenter, number);
        }

        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
        ++blockNumber;
    }
}

void CodeEditorWithLineNumbers::setSourceCode(const QString& code, const QString& ext) {
    m_highlighter->setLanguage(ext);
    setPlainText(code);
}


class InteractiveImageViewer : public QWidget {
    Q_OBJECT
public:
    explicit InteractiveImageViewer(QWidget* parent = nullptr)
        : QWidget(parent) {
        setMouseTracking(true);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        // не начинаем тяжелую догрузку оригинала на каждом колесе а ждем
        // пока пользователь закончит жест.
        m_fullResTimer.setSingleShot(true);
        m_fullResTimer.setInterval(120);
        connect(&m_fullResTimer, &QTimer::timeout, this, [this]() {
            startFullResolutionLoad();
        });
    }

    void setImage(const QImage& img, const QSize& originalSize, const QString& baseInfo,
                  const std::filesystem::path& filePath = {}) {
        bool sameFileAndSize = (m_filePath == filePath && !m_image.isNull() && m_image.size() == img.size());
        m_fullResTimer.stop();
        ++m_fullResRequest; // отменяем незавершённую догрузку другого файла
        m_fullResRequested = false;
        m_originalSize = (originalSize.isValid() && !originalSize.isEmpty()) ? originalSize : img.size();
        m_isPreviewScaled = m_originalSize.width() > img.width() ||
                            m_originalSize.height() > img.height();
        m_image = img;
        m_scaledPixmap = QPixmap();
        m_scaledTargetSize = QSize();
        m_baseInfo = baseInfo;
        m_filePath = filePath;
        if (!sameFileAndSize || m_isInitialFit) {
            m_isInitialFit = true;
            m_pan = QPointF(0, 0);
            m_scale = 1.0;
            resetToFit();
        } else {
            emit zoomChanged(currentZoomText());
        }
        update();
    }

    void clear() {
        m_fullResTimer.stop();
        ++m_fullResRequest;
        m_image = QImage();
        m_scaledPixmap = QPixmap();
        m_scaledTargetSize = QSize();
        m_baseInfo.clear();
        m_filePath.clear();
        m_originalSize = QSize();
        m_isPreviewScaled = false;
        m_fullResRequested = false;
        m_isInitialFit = true;
        update();
    }

    void resetToFit() {
        if (m_image.isNull() || width() <= 0 || height() <= 0) return;
        double sx = static_cast<double>(width()) / m_image.width();
        double sy = static_cast<double>(height()) / m_image.height();
        // Уменьшенное превью можно растягивать до размера оригинала, но не
        // выше: мелкие картинки по прежнему не увеличиваем больше ста процентов
        double maxScale = 1.0;
        if (m_isPreviewScaled && !m_originalSize.isEmpty()) {
            maxScale = std::max(1.0, double(m_originalSize.width()) / m_image.width());
        }
        m_scale = std::min({sx, sy, maxScale});
        m_pan = QPointF((width() - m_image.width() * m_scale) / 2.0,
                        (height() - m_image.height() * m_scale) / 2.0);
        emit zoomChanged(currentZoomText());
        maybeRequestFullResolution();
    }

    QString currentZoomText() const {
        if (m_image.isNull() || m_originalSize.isEmpty()) return "";
        // Процент считаем относительно исходника, а не уменьшенного превью,
        // иначе он скачет в момент подгрузки оригинала
        const double effectiveScale = m_scale * double(m_image.width()) /
                                      double(m_originalSize.width());
        int pct = qRound(effectiveScale * 100.0);
        return QString("%1 \u2022 Zoom: %2%").arg(m_baseInfo).arg(pct);
    }

signals:
    void zoomChanged(const QString& infoText);

protected:
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        if (m_isInitialFit && !m_image.isNull()) {
            resetToFit();
        }
    }

    void paintEvent(QPaintEvent* /*event*/) override {
        QPainter p(this);
        p.fillRect(rect(), Theme::Colors::Base());

        if (m_image.isNull()) return;

        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        p.setRenderHint(QPainter::Antialiasing, true);

        QSize targetSize( std::max(1, qRound(m_image.width() * m_scale)),
                          std::max(1, qRound(m_image.height() * m_scale)) );
        QRectF targetRect(m_pan.x(), m_pan.y(), targetSize.width(), targetSize.height());

        // Кэшированный pixmap ускоряет отрисовку, но на большом зуме он сам
        // может занять десятки мегабайт. Выше лимита рисуем напрямую из QImage.
        const qint64 pixmapBytes = qint64(targetSize.width()) * targetSize.height() * 4;
        if (m_scale <= 2.0 && pixmapBytes <= kScaledPixmapBudgetBytes) {
            if (m_scaledPixmap.isNull() || m_scaledTargetSize != targetSize) {
                m_scaledTargetSize = targetSize;
                m_scaledPixmap = QPixmap::fromImage(
                    m_image.scaled(targetSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
            }
            p.drawPixmap(targetRect.topLeft(), m_scaledPixmap);
        } else {
            // На большом зуме масштабируем только видимую часть картинки,
            // иначе Qt каждый кадр обрабатывает весь огромный targetRect.
            const QRectF visible = targetRect.intersected(QRectF(rect()));
            if (visible.isEmpty()) return;
            const QRectF source((visible.left() - targetRect.left()) / m_scale,
                                (visible.top() - targetRect.top()) / m_scale,
                                visible.width() / m_scale,
                                visible.height() / m_scale);
            p.drawImage(visible, m_image, source);
        }
    }

    void wheelEvent(QWheelEvent* event) override {
        if (m_image.isNull()) return;

        m_isInitialFit = false;
        double factor = (event->angleDelta().y() > 0) ? 1.15 : (1.0 / 1.15);
        double newScale = std::clamp(m_scale * factor, 0.05, 50.0);

        QPointF mousePos = event->position();
        // точка под курсором остаётся на месте
        m_pan = mousePos - (mousePos - m_pan) * (newScale / m_scale);
        m_scale = newScale;
        // если пользователь отзумился до вписанного вида оригинал снова
        // не нужен и освобождаем десятки мегабайт
        const double fitScale = std::min(double(width()) / m_image.width(),
                                         double(height()) / m_image.height());
        if (!m_isPreviewScaled && m_scale <= fitScale) {
            dropFullResolution();
        }
        maybeRequestFullResolution();

        emit zoomChanged(currentZoomText());
        update();
        event->accept();
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && !m_image.isNull()) {
            m_dragging = true;
            m_dragStartPos = event->pos();
            m_dragStartPan = m_pan;
            if (!m_isInitialFit) {
                setCursor(Qt::ClosedHandCursor);
            }
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (m_dragging) {
            // если изображение вписано (без зума), drag отдаёт файл наружу,
            // а не панорамирует
            if (m_isInitialFit && !m_filePath.empty()) {
                int dist = (event->pos() - m_dragStartPos).manhattanLength();
                if (dist >= QApplication::startDragDistance()) {
                    m_dragging = false;
                    setCursor(Qt::ArrowCursor);
                    QDrag* drag = Dnd::DragDropHandler::createDrag({m_filePath}, this);
                    if (drag) {
                        drag->exec(Qt::CopyAction | Qt::MoveAction | Qt::LinkAction, Qt::CopyAction);
                    }
                    event->accept();
                    return;
                }
            } else {
                m_pan = m_dragStartPan + (event->pos() - m_dragStartPos);
                update();
            }
            event->accept();
            return;
        }
        QWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && m_dragging) {
            m_dragging = false;
            setCursor(Qt::ArrowCursor);
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && !m_image.isNull()) {
            dropFullResolution();
            m_isInitialFit = true;
            resetToFit();
            update();
            event->accept();
            return;
        }
        QWidget::mouseDoubleClickEvent(event);
    }

private:
    // возврат к вписанному виду: оригинал больше не нужен и оставляем только превью
    // уменьшенную копию, чтобы не держать десятки мегабайт в памяти.
    void dropFullResolution() {
        // отменяем возможную незавершенную догрузку оригинала
        m_fullResTimer.stop();
        ++m_fullResRequest;
        m_fullResRequested = false;
        if (m_isPreviewScaled || m_image.isNull()) return;
        // Если на вписанном виде превью всё равно пришлось бы растягивать
        // если панель большая то оригинал нужен для качества
        const QSize previewSize = boundedSize(m_originalSize, kPreviewMaxSide);
        if (previewSize.isValid()) {
            const double fitScale = std::min(double(width()) / previewSize.width(),
                                             double(height()) / previewSize.height());
            if (fitScale > kFullResTriggerScale) return;
        }
        const QSize target = boundedSize(m_image.size(), kPreviewMaxSide);
        if (target == m_image.size()) return;
        // Сохраняем визуальный размер: scale считается от пикселей картинки.
        const double factor = double(m_image.width()) / double(target.width());
        m_image = m_image.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        m_scale *= factor;
        m_scaledPixmap = QPixmap();
        m_scaledTargetSize = QSize();
        m_isPreviewScaled = true;
        emit zoomChanged(currentZoomText());
    }

    void maybeRequestFullResolution() {
        if (!m_isPreviewScaled || m_image.isNull() ||
            m_filePath.empty() || m_originalSize.isEmpty()) {
            return;
        }
        // Пока превью показывается 1:1 или уменьшенным, его пикселей хватает.
        // если догрузка уже началась то отменяем ее потому что она больше не нужна
        if (m_scale < kFullResTriggerScale) {
            m_fullResTimer.stop();
            if (m_fullResRequested) {
                ++m_fullResRequest;
                m_fullResRequested = false;
            }
            return;
        }
        if (m_fullResRequested) return;
        m_fullResTimer.start();
    }

    void startFullResolutionLoad() {
        if (!m_isPreviewScaled || m_fullResRequested || m_image.isNull() ||
            m_filePath.empty() || m_originalSize.isEmpty() ||
            m_scale < kFullResTriggerScale) {
            return;
        }

        m_fullResRequested = true;
        const quint64 requestId = ++m_fullResRequest;
        const std::filesystem::path path = m_filePath;
        const QSize original = m_originalSize;
        QPointer<InteractiveImageViewer> self(this);
        Core::Tasks::TaskManager::instance().poolForKind(QStringLiteral("preview"))
            ->start([self, path, original, requestId]() {
            QImageReader reader(QString::fromUtf8(path.c_str()));
            reader.setAutoTransform(true);
            const QSize decodeSize = boundedSize(original, kFullResMaxSide);
            if (decodeSize.isValid() && decodeSize != original) {
                reader.setScaledSize(decodeSize);
            }
            QImage full = reader.read();
            if (full.isNull()) {
                if (self) {
                    QMetaObject::invokeMethod(self.data(), [self, requestId]() {
                        if (self && requestId == self->m_fullResRequest) {
                            self->m_fullResRequested = false;
                        }
                    }, Qt::QueuedConnection);
                }
                return;
            }
            if (!self) return;
            if (full.width() > kFullResMaxSide || full.height() > kFullResMaxSide) {
                full = full.scaled(boundedSize(full.size(), kFullResMaxSide),
                                   Qt::KeepAspectRatio, Qt::SmoothTransformation);
            }
            QMetaObject::invokeMethod(self.data(),
                [self, full = std::move(full), requestId, path]() mutable {
                    if (!self || requestId != self->m_fullResRequest ||
                        self->m_filePath != path || self->m_image.isNull()) {
                        return;
                    }
                    // scale привязан к пикселям текущей картинки: при замене
                    // превью на полный размер пересчитываем его чтобы изображение
                    // не прыгнуло на экране.
                    const double factor = double(self->m_image.width()) / double(full.width());
                    self->m_scale *= factor;
                    self->m_image = std::move(full);
                    self->m_isPreviewScaled = false;
                    self->m_scaledPixmap = QPixmap();
                    self->m_scaledTargetSize = QSize();
                    self->zoomChanged(self->currentZoomText());
                    self->update();
                }, Qt::QueuedConnection);
        });
    }

    QImage m_image;
    QPixmap m_scaledPixmap;
    QSize m_scaledTargetSize;
    QSize m_originalSize;
    QString m_baseInfo;
    std::filesystem::path m_filePath;
    bool m_isPreviewScaled{false};
    bool m_fullResRequested{false};
    QTimer m_fullResTimer;
    quint64 m_fullResRequest{0};
    double m_scale{1.0};
    QPointF m_pan{0, 0};
    bool m_isInitialFit{true};
    bool m_dragging{false};
    QPoint m_dragStartPos;
    QPointF m_dragStartPan;
};


static QString formatConflictFileDate(const std::filesystem::path& p) {
    std::error_code ec;
    auto ftime = std::filesystem::last_write_time(p, ec);
    if (ec) return "--";
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    std::time_t tt = std::chrono::system_clock::to_time_t(sctp);
    std::tm tmVal;
    localtime_r(&tt, &tmVal);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tmVal);
    return QString::fromUtf8(buf);
}

static QString formatConflictFileSize(const std::filesystem::path& p) {
    std::error_code ec;
    if (std::filesystem::is_directory(p, ec)) {
        return "Folder";
    }
    uintmax_t sz = std::filesystem::file_size(p, ec);
    if (ec) return "--";
    return Core::XdgUtils::formatSize(sz);
}

class ConflictResolverWidget : public QWidget {
    Q_OBJECT
public:
    explicit ConflictResolverWidget(QWidget* parent = nullptr)
        : QWidget(parent) {
        setupUi();
    }

    void setupConflicts(const std::vector<ConflictItem>& items,
                        std::function<void(const std::vector<ConflictItem>&)> onApply,
                        std::function<void()> onCancel) {
        m_items = items;
        m_onApply = std::move(onApply);
        m_onCancel = std::move(onCancel);
        rebuildCards();
    }

private:
    void setupUi() {
        using namespace Theme::Colors;

        auto mainLayout = new QVBoxLayout(this);
        mainLayout->setContentsMargins(12, 12, 12, 12);
        mainLayout->setSpacing(8);

        m_titleLabel = new QLabel("Resolve Conflicts", this);
        m_titleLabel->setFont(Theme::getMonospaceFont(10, QFont::Bold));
        m_titleLabel->setStyleSheet(QString("color: %1; font-weight: bold; border: none;").arg(Accent().name()));
        mainLayout->addWidget(m_titleLabel);

        m_subtitleLabel = new QLabel("Choose how to resolve duplicate items:", this);
        m_subtitleLabel->setFont(Theme::getMonospaceFont(8));
        m_subtitleLabel->setStyleSheet(QString("color: %1; border: none;").arg(Subtext().name()));
        mainLayout->addWidget(m_subtitleLabel);

        // строгий порядок replace и skip, без cancel
        auto toolbarLayout = new QHBoxLayout();
        toolbarLayout->setSpacing(6);

        auto btnReplaceAll = new QPushButton("Replace", this);
        btnReplaceAll->setFont(Theme::getMonospaceFont(8, QFont::Bold));
        btnReplaceAll->setCursor(Qt::PointingHandCursor);
        btnReplaceAll->setStyleSheet(QString("background-color: %1; color: %2; font-weight: bold; border: none; border-radius: 0px; padding: 5px 12px;")
                                         .arg(Accent().name(), AccentFg().name()));
        connect(btnReplaceAll, &QPushButton::clicked, this, [this]() {
            for (auto& it : m_items) {
                it.replace = true;
            }
            if (m_onApply) m_onApply(m_items);
        });
        toolbarLayout->addWidget(btnReplaceAll);

        auto btnSkipAll = new QPushButton("Skip", this);
        btnSkipAll->setFont(Theme::getMonospaceFont(8, QFont::Bold));
        btnSkipAll->setCursor(Qt::PointingHandCursor);
        btnSkipAll->setStyleSheet(QString("background-color: %1; color: %2; border: 1px solid %3; border-radius: 0px; padding: 5px 12px;")
                                      .arg(Surface0().name(), Subtext().name(), Surface1().name()));
        connect(btnSkipAll, &QPushButton::clicked, this, [this]() {
            for (auto& it : m_items) {
                it.replace = false;
            }
            if (m_onApply) m_onApply(m_items);
        });
        toolbarLayout->addWidget(btnSkipAll);
        toolbarLayout->addStretch(1);

        mainLayout->addLayout(toolbarLayout);

        m_scrollArea = new QScrollArea(this);
        m_scrollArea->setWidgetResizable(true);
        m_scrollArea->setFrameShape(QFrame::NoFrame);
        m_scrollArea->setStyleSheet("QScrollArea { background-color: transparent; border: none; }");
        // скроллбары убраны, прокрутка колесом
        m_scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

        m_cardsContainer = new QWidget(m_scrollArea);
        m_cardsContainer->setStyleSheet("background-color: transparent;");
        m_cardsLayout = new QVBoxLayout(m_cardsContainer);
        m_cardsLayout->setContentsMargins(0, 4, 0, 4);
        m_cardsLayout->setSpacing(6);

        m_scrollArea->setWidget(m_cardsContainer);
        mainLayout->addWidget(m_scrollArea, 1);

        // только apply choices, без cancel
        auto footerLayout = new QHBoxLayout();
        footerLayout->setSpacing(8);
        footerLayout->addStretch(1);

        auto btnApply = new QPushButton("Apply Choices", this);
        btnApply->setFont(Theme::getMonospaceFont(8, QFont::Bold));
        btnApply->setCursor(Qt::PointingHandCursor);
        btnApply->setStyleSheet(QString("background-color: %1; color: %2; font-weight: bold; border: none; border-radius: 0px; padding: 6px 20px;")
                                    .arg(Accent().name(), AccentFg().name()));
        connect(btnApply, &QPushButton::clicked, this, [this]() {
            if (m_onApply) m_onApply(m_items);
        });
        footerLayout->addWidget(btnApply);

        mainLayout->addLayout(footerLayout);
    }

    void rebuildCards() {
        using namespace Theme::Colors;

        m_rowUpdaters.clear();

        QLayoutItem* item;
        while ((item = m_cardsLayout->takeAt(0)) != nullptr) {
            if (item->widget()) {
                delete item->widget();
            }
            delete item;
        }

        m_titleLabel->setText(QString("Resolve Conflicts (%1 %2)")
                                  .arg(m_items.size())
                                  .arg(m_items.size() == 1 ? "item" : "items"));

        for (size_t i = 0; i < m_items.size(); ++i) {
            const auto& conf = m_items[i];
            auto card = new QFrame(m_cardsContainer);
            card->setStyleSheet(QString(
                "QFrame {"
                "  background-color: %1;"
                "  border: 1px solid %2;"
                "  border-radius: 0px;"
                "}"
            ).arg(Surface0().name(), Surface1().name()));

            auto cardLayout = new QVBoxLayout(card);
            cardLayout->setContentsMargins(8, 6, 8, 6);
            cardLayout->setSpacing(4);

            auto topRow = new QHBoxLayout();
            topRow->setSpacing(6);

            std::error_code ec;
            bool isDir = std::filesystem::is_directory(conf.dest, ec);
            auto iconInfo = Icons::getIcon(conf.dest, isDir, false);

            auto iconLabel = new QLabel(iconInfo.glyph, card);
            iconLabel->setFont(Theme::getMonospaceFont(10));
            iconLabel->setStyleSheet(QString("color: %1; border: none;").arg(iconInfo.color.name()));
            topRow->addWidget(iconLabel);

            auto nameLabel = new QLabel(QString::fromUtf8(conf.dest.filename().c_str()), card);
            nameLabel->setFont(Theme::getMonospaceFont(9, QFont::Bold));
            nameLabel->setStyleSheet(QString("color: %1; border: none;").arg(Text().name()));
            topRow->addWidget(nameLabel, 1);

            cardLayout->addLayout(topRow);

            // сетка метаданных: сравнение incoming и existing
            auto metaGrid = new QGridLayout();
            metaGrid->setHorizontalSpacing(8);
            metaGrid->setVerticalSpacing(2);

            auto lblInTag = new QLabel("Incoming:", card);
            lblInTag->setFont(Theme::getMonospaceFont(8));
            lblInTag->setStyleSheet(QString("color: %1; border: none;").arg(Accent().name()));

            auto lblInVal = new QLabel(QString("%1  \u2022  %2")
                                           .arg(formatConflictFileSize(conf.source))
                                           .arg(formatConflictFileDate(conf.source)), card);
            lblInVal->setFont(Theme::getMonospaceFont(8));
            lblInVal->setStyleSheet(QString("color: %1; border: none;").arg(Text().name()));

            auto lblExTag = new QLabel("Existing:", card);
            lblExTag->setFont(Theme::getMonospaceFont(8));
            lblExTag->setStyleSheet(QString("color: %1; border: none;").arg(Subtext().name()));

            auto lblExVal = new QLabel(QString("%1  \u2022  %2")
                                           .arg(formatConflictFileSize(conf.dest))
                                           .arg(formatConflictFileDate(conf.dest)), card);
            lblExVal->setFont(Theme::getMonospaceFont(8));
            lblExVal->setStyleSheet(QString("color: %1; border: none;").arg(Dim().name()));

            metaGrid->addWidget(lblInTag, 0, 0);
            metaGrid->addWidget(lblInVal, 0, 1);
            metaGrid->addWidget(lblExTag, 1, 0);
            metaGrid->addWidget(lblExVal, 1, 1);

            cardLayout->addLayout(metaGrid);

            auto actRow = new QHBoxLayout();
            actRow->setSpacing(6);
            actRow->addStretch(1);

            auto btnRep = new QPushButton("Replace", card);
            btnRep->setFont(Theme::getMonospaceFont(8, QFont::Bold));
            btnRep->setCursor(Qt::PointingHandCursor);

            auto btnSkp = new QPushButton("Skip", card);
            btnSkp->setFont(Theme::getMonospaceFont(8, QFont::Bold));
            btnSkp->setCursor(Qt::PointingHandCursor);

            auto updateRowStyle = [this, i, btnRep, btnSkp]() {
                using namespace Theme::Colors;
                if (m_items[i].replace) {
                    btnRep->setStyleSheet(QString("background-color: %1; color: %2; font-weight: bold; border: none; border-radius: 0px; padding: 3px 12px;")
                                              .arg(Accent().name(), AccentFg().name()));
                    btnSkp->setStyleSheet(QString("background-color: %1; color: %2; border: 1px solid %3; border-radius: 0px; padding: 3px 12px;")
                                              .arg(Surface0().name(), Dim().name(), Surface1().name()));
                } else {
                    btnRep->setStyleSheet(QString("background-color: %1; color: %2; border: 1px solid %3; border-radius: 0px; padding: 3px 12px;")
                                              .arg(Surface0().name(), Dim().name(), Surface1().name()));
                    btnSkp->setStyleSheet(QString("background-color: %1; color: %2; font-weight: bold; border: 1px solid %1; border-radius: 0px; padding: 3px 12px;")
                                              .arg(Surface1().name(), Text().name()));
                }
            };

            updateRowStyle();

            connect(btnRep, &QPushButton::clicked, this, [this, i, updateRowStyle]() {
                m_items[i].replace = true;
                updateRowStyle();
            });

            connect(btnSkp, &QPushButton::clicked, this, [this, i, updateRowStyle]() {
                m_items[i].replace = false;
                updateRowStyle();
            });

            m_rowUpdaters.push_back(updateRowStyle);

            actRow->addWidget(btnRep);
            actRow->addWidget(btnSkp);
            cardLayout->addLayout(actRow);

            m_cardsLayout->addWidget(card);
        }

        m_cardsLayout->addStretch(1);
    }

    std::vector<ConflictItem> m_items;
    std::function<void(const std::vector<ConflictItem>&)> m_onApply;
    std::function<void()> m_onCancel;
    std::vector<std::function<void()>> m_rowUpdaters;

    QLabel* m_titleLabel{nullptr};
    QLabel* m_subtitleLabel{nullptr};
    QScrollArea* m_scrollArea{nullptr};
    QWidget* m_cardsContainer{nullptr};
    QVBoxLayout* m_cardsLayout{nullptr};
};

// постраничный PDF: страницы добавляются по мере прокрутки, рендер идёт в
// фоновом пуле PreviewPane, поэтому виджет только показывает готовые кадры
class DocumentViewer : public QWidget {
    Q_OBJECT

public:
    explicit DocumentViewer(QWidget* parent = nullptr)
        : QWidget(parent) {
        setFocusPolicy(Qt::StrongFocus);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);

        m_scroll = new QScrollArea(this);
        m_scroll->setWidgetResizable(true);
        m_scroll->setFrameShape(QFrame::NoFrame);
        m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_scroll->setStyleSheet(QString("QScrollArea { background-color: %1; border: none; }")
                                    .arg(Theme::Colors::Base().name()));
        m_pages = new QWidget(m_scroll);
        m_pages->setStyleSheet(QString("background-color: %1;").arg(Theme::Colors::Base().name()));
        m_layout = new QVBoxLayout(m_pages);
        m_layout->setContentsMargins(12, 12, 12, 12);
        m_layout->setSpacing(8);
        m_scroll->setWidget(m_pages);
        layout->addWidget(m_scroll, 1);
    }

    void setDocument(const std::filesystem::path& path, int pageCount) {
        m_path = path;
        m_pageCount = pageCount;
        m_nextPage = 1;
        m_requestedPage = 0;
        m_complete = false;
        clearPages();
        m_scroll->verticalScrollBar()->setValue(0);
        m_placeholder = new QLabel(QStringLiteral("Loading page 1…"), m_pages);
        m_placeholder->setAlignment(Qt::AlignCenter);
        m_placeholder->setFont(Theme::getMonospaceFont(9));
        m_placeholder->setStyleSheet(QString("color: %1;").arg(Theme::Colors::Dim().name()));
        m_layout->addWidget(m_placeholder);
        QTimer::singleShot(0, this, [this]() { maybeRequestNext(); });
    }

    void reset() {
        m_requestedPage = 0;
        m_complete = true;
        clearPages();
        m_pageCount = -1;
        m_nextPage = 1;
    }

    void addPage(int page, const QImage& image) {
        if (page != m_requestedPage || image.isNull()) return;
        if (m_placeholder) {
            m_layout->removeWidget(m_placeholder);
            m_placeholder->deleteLater();
            m_placeholder = nullptr;
        }
        auto* label = new QLabel(m_pages);
        label->setAlignment(Qt::AlignCenter);
        label->setStyleSheet(QString("background-color: %1;").arg(Theme::Colors::Base().name()));
        m_layout->addWidget(label);
        m_pagesData.push_back({page, image, label});
        scalePage(m_pagesData.back());
        // кадры нужны только последним страницам для пересчёта при
        // изменении ширины панели; у остальных остаётся готовый pixmap
        if (m_pagesData.size() > kMaxCachedPdfPageImages) {
            m_pagesData[m_pagesData.size() - kMaxCachedPdfPageImages - 1].image = QImage();
        }

        m_requestedPage = 0;
        m_nextPage = page + 1;
        if (m_pageCount > 0 && m_nextPage > m_pageCount) m_complete = true;
        QTimer::singleShot(0, this, [this]() {
            auto* bar = m_scroll->verticalScrollBar();
            if (!m_complete && (bar->maximum() <= 0 ||
                                bar->value() > bar->maximum() - m_scroll->viewport()->height())) {
                maybeRequestNext();
            }
        });
    }

    void pageFailed(int page, const QString& error, bool endOfDocument) {
        if (page != m_requestedPage) return;
        m_requestedPage = 0;
        m_complete = true;
        if (page == 1) {
            if (m_placeholder) {
                m_placeholder->setText(error.isEmpty() ? QStringLiteral("PDF preview unavailable")
                                                       : error);
            }
            return;
        }
        Q_UNUSED(endOfDocument);
        Q_UNUSED(error);
    }

signals:
    void pageRequested(int page);

protected:
    void keyPressEvent(QKeyEvent* event) override {
        auto* bar = m_scroll->verticalScrollBar();
        const int step = qMax(40, m_scroll->viewport()->height() / 3);
        if (event->key() == Qt::Key_Down) {
            bar->setValue(bar->value() + step);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Up) {
            bar->setValue(bar->value() - step);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_PageDown || event->key() == Qt::Key_Space) {
            bar->setValue(bar->value() + m_scroll->viewport()->height());
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_PageUp) {
            bar->setValue(bar->value() - m_scroll->viewport()->height());
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Home) {
            bar->setValue(0);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_End) {
            bar->setValue(bar->maximum());
            event->accept();
            return;
        }
        QWidget::keyPressEvent(event);
    }

    void mousePressEvent(QMouseEvent* event) override {
        setFocus(Qt::MouseFocusReason);
        QWidget::mousePressEvent(event);
    }

    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        for (Page& page : m_pagesData) scalePage(page);
    }

private:
    struct Page {
        int number{0};
        QImage image;
        QLabel* label{nullptr};
    };

    void clearPages() {
        while (QLayoutItem* item = m_layout->takeAt(0)) {
            if (QWidget* widget = item->widget()) widget->deleteLater();
            delete item;
        }
        m_pagesData.clear();
        m_placeholder = nullptr;
    }

    void maybeRequestNext() {
        if (m_complete || m_requestedPage > 0) return;
        if (m_pageCount > 0 && m_nextPage > m_pageCount) {
            m_complete = true;
            return;
        }
        m_requestedPage = m_nextPage;
        emit pageRequested(m_requestedPage);
    }

    void scalePage(Page& page) {
        if (!page.label) return;
        const int available = qMax(80, m_scroll->viewport()->width() - 32);
        if (page.image.isNull()) {
            // shortcut: у старых страниц остался только pixmap, поэтому
            // повторный resize масштабирует уже уменьшенную копию и может
            // мылить картинку; для превью это допустимо
            const QPixmap current = page.label->pixmap();
            if (current.isNull() || current.width() == available) return;
            page.label->setPixmap(current.scaledToWidth(available, Qt::SmoothTransformation));
            return;
        }
        const QPixmap pixmap = (page.image.width() > available)
            ? QPixmap::fromImage(page.image.scaledToWidth(available, Qt::SmoothTransformation))
            : QPixmap::fromImage(page.image);
        page.label->setPixmap(pixmap);
    }

    QScrollArea* m_scroll{nullptr};
    QWidget* m_pages{nullptr};
    QVBoxLayout* m_layout{nullptr};
    QLabel* m_placeholder{nullptr};
    std::filesystem::path m_path;
    std::vector<Page> m_pagesData;
    int m_pageCount{-1};
    int m_nextPage{1};
    int m_requestedPage{0};
    bool m_complete{false};
};

PreviewPane::PreviewPane(QWidget* parent)
    : QWidget(parent) {
    setAcceptDrops(true);
    setupUi();
}

PreviewPane::~PreviewPane() {
    (*m_currentJobId)++;
}

void PreviewPane::setupUi() {
    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    m_stackedWidget = new QStackedWidget(this);

    // PageEmpty: заглушка пустого состояния
    m_emptyLabel = new QLabel("Select an item to preview", this);
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setFont(Theme::getMonospaceFont(9));
    m_emptyLabel->setStyleSheet(QString("color: %1;").arg(Theme::Colors::Dim().name()));
    m_stackedWidget->addWidget(m_emptyLabel);

    // PageImage: интерактивный просмотр изображения с панорамированием и зумом
    m_imageContainer = new QWidget(this);
    auto imageLayout = new QVBoxLayout(m_imageContainer);
    imageLayout->setContentsMargins(8, 8, 8, 8);
    imageLayout->setSpacing(4);

    m_imageViewer = new InteractiveImageViewer(m_imageContainer);
    imageLayout->addWidget(m_imageViewer, 1);

    m_imageInfoLabel = new QLabel(m_imageContainer);
    m_imageInfoLabel->setAlignment(Qt::AlignCenter);
    m_imageInfoLabel->setFont(Theme::getMonospaceFont(8));
    m_imageInfoLabel->setStyleSheet(QString("color: %1;").arg(Theme::Colors::Subtext().name()));
    imageLayout->addWidget(m_imageInfoLabel, 0);

    connect(m_imageViewer, &InteractiveImageViewer::zoomChanged, this, [this](const QString& infoText) {
        m_imageInfoLabel->setText(infoText);
    });

    m_stackedWidget->addWidget(m_imageContainer);

    // PageCode: редактор кода с подсветкой синтаксиса
    m_codeViewer = new CodeEditorWithLineNumbers(this);
    m_codeViewer->setReadOnly(true);
    m_stackedWidget->addWidget(m_codeViewer);

    // PageDir: превью листинга каталога, приглушённое как родительская колонка
    m_dirListView = new MillerListView(true, this);
    m_dirModel = new Models::FileSystemModel(this);
    // Превью не показывает размер, дату и права, зато листинг больших папок
    // становится заметно быстрее.
    m_dirModel->setLightweightScan(true);
    m_dirDelegate = new FileItemDelegate(true, this);
    m_dirListView->setModel(m_dirModel);
    m_dirListView->setItemDelegate(m_dirDelegate);
    m_dirListView->setContextMenuPolicy(Qt::CustomContextMenu);

    connect(m_dirListView, &MillerListView::enterDirectoryRequested, this, &PreviewPane::enterDirectoryRequested);
    connect(m_dirListView, &MillerListView::openFileRequested, this, [this](const std::filesystem::path& p) {
        if (Core::XdgUtils::isArchive(p)) {
            emit archiveOpenRequested(p);
            return;
        }
        Core::XdgUtils::openWithDefault(p);
    });
    connect(m_dirListView, &MillerListView::filesDropped, this, [this](const auto& sources, const auto& targetDir, auto action, auto mods) {
        // QPointer защита: фоновый copy/move может завершиться после разрушения панели
        QPointer<PreviewPane> guard(this);
        Dnd::DragDropHandler::handleDrop(sources, targetDir, action, mods, this, [guard]() {
            if (guard && guard->m_dirModel) guard->m_dirModel->refresh();
        });
    });

    connect(m_dirListView, &QListView::doubleClicked, this, [this](const QModelIndex& index) {
        if (!index.isValid()) return;
        bool isDir = index.data(Models::FileSystemModel::IsDirRole).toBool();
        QString pathStr = index.data(Models::FileSystemModel::PathRole).toString();
        std::filesystem::path p(pathStr.toUtf8().toStdString());
        if (isDir) {
            emit enterDirectoryRequested(p);
        } else if (Core::XdgUtils::isArchive(p)) {
            emit archiveOpenRequested(p);
        } else {
            Core::XdgUtils::openWithDefault(p);
        }
    });

    connect(m_dirListView, &MillerListView::contextMenuRequested, this, &PreviewPane::onDirContextMenu);

    m_stackedWidget->addWidget(m_dirListView);

    // PageHex: форматированный hex дамп
    m_hexViewer = new QPlainTextEdit(this);
    m_hexViewer->setReadOnly(true);
    m_hexViewer->setFont(Theme::getMonospaceFont(9));
    m_hexViewer->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_hexViewer->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_hexViewer->setStyleSheet(QString("background-color: %1; color: %2; border: none; padding: 6px;")
                                .arg(Theme::Colors::Base().name(), Theme::Colors::Dim().name()));
    m_stackedWidget->addWidget(m_hexViewer);

    // PageMedia: метаданные медиа
    m_mediaViewer = new QPlainTextEdit(this);
    m_mediaViewer->setReadOnly(true);
    m_mediaViewer->setFont(Theme::getMonospaceFont(9));
    m_mediaViewer->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_mediaViewer->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_mediaViewer->setStyleSheet(QString("background-color: %1; color: %2; border: none; padding: 12px;")
                                  .arg(Theme::Colors::Base().name(), Theme::Colors::Dim().name()));
    m_stackedWidget->addWidget(m_mediaViewer);

    // PageConflictResolver: интерактивный выбор при конфликтах
    m_conflictResolver = new ConflictResolverWidget(this);
    m_stackedWidget->addWidget(m_conflictResolver);

    // PageArchive: интерактивный листинг архива, перетаскивание с распаковкой в кэш
    m_archivePage = new QWidget(this);
    {
        auto* archiveLayout = new QVBoxLayout(m_archivePage);
        archiveLayout->setContentsMargins(0, 0, 0, 0);
        archiveLayout->setSpacing(0);

        m_archiveModel = new Models::ArchiveModel(this);
        m_archiveTree = new ArchiveTreeWidget(m_archivePage);
        m_archiveTree->setArchiveModel(m_archiveModel);
        archiveLayout->addWidget(m_archiveTree, 1);
    }
    connect(m_archiveModel, &Models::ArchiveModel::archiveReset, this, [this]() {
        if (m_archiveTree) m_archiveTree->clearArchive();
    });
    connect(m_archiveModel, &Models::ArchiveModel::loaded, this, [this]() {
        if (m_archiveTree) {
            m_archiveTree->buildFromEntries(m_archiveModel->entries());
        }
    });
    connect(m_archiveModel, &Models::ArchiveModel::loadFailed, this, [this](const QString& error) {
        if (m_archiveTree) m_archiveTree->showMessage(error);
    });
    connect(m_archiveTree, &ArchiveTreeWidget::selectionInteractionStarted,
            this, &PreviewPane::archiveSelectionInteractionStarted);
    connect(m_archiveTree, &ArchiveTreeWidget::messageRequested,
            this, &PreviewPane::messageRequested);
    m_stackedWidget->addWidget(m_archivePage);

    // PageDocument: PDF, постраничная прокрутка и фоновая догрузка страниц
    m_documentViewer = new DocumentViewer(this);
    connect(m_documentViewer, &DocumentViewer::pageRequested,
            this, &PreviewPane::requestDocumentPage);
    m_stackedWidget->addWidget(m_documentViewer);

    mainLayout->addWidget(m_stackedWidget);
    m_stackedWidget->setCurrentIndex(PageEmpty);

    // дебаунс превью: коалесцирует быстрые смены выделения и предотвращает
    // ложный сброс на PageEmpty при перезагрузке модели (watcher / rename / delete)
    m_previewDebounce = new QTimer(this);
    m_previewDebounce->setSingleShot(true);
    m_previewDebounce->setInterval(16);
    connect(m_previewDebounce, &QTimer::timeout, this, [this]() {
        if (m_pendingClear && !m_hasPending) {
            m_pendingClear = false;
            performClearNow();
            return;
        }
        if (!m_hasPending) return;
        m_hasPending = false;
        m_pendingClear = false;
        auto p = m_pendingPath;
        m_pendingPath.clear();
        loadPathNow(p);
    });
}

void PreviewPane::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
}

void PreviewPane::dragEnterEvent(QDragEnterEvent* event) {
    if (m_stackedWidget && m_stackedWidget->currentIndex() == PageArchive) {
        event->ignore();
        return;
    }
    if (Dnd::DragDropHandler::canAcceptDrop(event->mimeData())) {
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

void PreviewPane::dragMoveEvent(QDragMoveEvent* event) {
    if (m_stackedWidget && m_stackedWidget->currentIndex() == PageArchive) {
        event->ignore();
        return;
    }
    if (Dnd::DragDropHandler::canAcceptDrop(event->mimeData())) {
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

void PreviewPane::dropEvent(QDropEvent* event) {
    if (m_stackedWidget && m_stackedWidget->currentIndex() == PageArchive) {
        event->ignore();
        return;
    }
    if (!Dnd::DragDropHandler::canAcceptDrop(event->mimeData())) {
        event->ignore();
        return;
    }

    std::filesystem::path targetDir;
    std::error_code ec;
    if (!m_activePath.empty() && std::filesystem::is_directory(m_activePath, ec)) {
        targetDir = m_activePath;
    } else if (!m_activePath.empty() && m_activePath.has_parent_path()) {
        targetDir = m_activePath.parent_path();
    } else if (auto* mvc = qobject_cast<MillerViewContainer*>(parentWidget())) {
        targetDir = mvc->currentDirectory();
    }

    if (!targetDir.empty()) {
        QPointer<PreviewPane> guard(this);
        if (Dnd::DragDropHandler::handleMimeDrop(event->mimeData(), targetDir,
                                                     event->proposedAction(), event->modifiers(),
                                                     parentWidget(), [guard]() {
                                                         if (guard && guard->m_dirModel
                                                             && guard->m_stackedWidget->currentIndex() == PageDir) {
                                                             guard->m_dirModel->refresh();
                                                         }
                                                     })) {
            event->acceptProposedAction();
            return;
        }
    }
    event->ignore();
}

void PreviewPane::onDirContextMenu(const QPoint& globalPos, const QModelIndex& idx) {
    std::vector<std::filesystem::path> sel;
    if (idx.isValid()) {
        QString pathStr = idx.data(Models::FileSystemModel::PathRole).toString();
        if (!pathStr.isEmpty()) {
            sel.push_back(std::filesystem::path(pathStr.toUtf8().toStdString()));
        }
    }

    auto menu = new TuiContextMenu(sel, m_activePath, this);
    menu->setAttribute(Qt::WA_DeleteOnClose);

    connect(menu, &TuiContextMenu::openRequested, this, [this, sel]() {
        if (!sel.empty()) {
            std::error_code ec;
            if (std::filesystem::is_directory(sel.front(), ec)) {
                emit enterDirectoryRequested(sel.front());
            } else if (Core::XdgUtils::isArchive(sel.front())) {
                emit archiveOpenRequested(sel.front());
            } else {
                Core::XdgUtils::openWithDefault(sel.front());
            }
        }
    });
    connect(menu, &TuiContextMenu::openWithAppRequested, this, [sel](const QString& execCmd) {
        if (!sel.empty()) {
            Core::DesktopApp app;
            app.execCommand = execCmd;
            Core::XdgUtils::openWithApp(app, sel.front());
        }
    });
    // «Open With...» полноразмерный диалог (оверлей в MainWindow)
    connect(menu, &TuiContextMenu::openWithRequested, this, [this, sel]() {
        if (!sel.empty()) emit openWithDialogRequested(sel.front());
    });
    // extract: here прямо рядом с архивом (в его папке), in: каталог назначения
    // выбирает MainWindow оверлеем FileChooserDialog с кнопкой Extract
    connect(menu, &TuiContextMenu::extractHereRequested, this, [this, sel]() {
        if (sel.empty()) return;
        const std::filesystem::path arc = sel.front();
        const std::filesystem::path dest = arc.parent_path();
        QPointer<PreviewPane> self(this);
        Core::Tasks::TaskManager::instance().submit(
            QStringLiteral("Extract %1").arg(QString::fromUtf8(arc.filename().c_str())),
            QStringLiteral("preview"),
            [self, arc, dest](Core::Tasks::TaskContext& ctx) {
                QString err;
                std::filesystem::path actualDest;
                bool ok = false;
                // исключение воркера не должно убить процесс
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
                    ctx.log(QStringLiteral("extracted to %1").arg(QString::fromUtf8(actualDest.c_str())));
                } else if (!cancelled && !err.isEmpty()) {
                    ctx.log(err);
                    ctx.fail(err);
                }
                ctx.progress(cancelled ? 0 : 1, 1);
                QMetaObject::invokeMethod(qApp, [self, ok, cancelled, err]() {
                    if (!self || cancelled) return;
                    if (ok) {
                        if (self->m_dirModel) self->m_dirModel->refresh();
                    } else {
                        TuiConfirmPopup::alert(self.data(), "Extraction Error", err);
                    }
                }, Qt::QueuedConnection);
            });
    });
    connect(menu, &TuiContextMenu::extractInRequested, this, [this, sel]() {
        if (sel.empty()) return;
        // hideEvent меню возвращает фокус панели; оверлей открываем после этого
        QTimer::singleShot(0, this, [this, archive = sel.front()]() {
            emit extractInRequested(archive);
        });
    });
    connect(menu, &TuiContextMenu::copyPathRequested, this, [this, sel](int mode) {
        std::filesystem::path target = sel.empty() ? m_activePath : sel.front();
        if (mode == 0) {
            QApplication::clipboard()->setText(QString::fromUtf8(target.c_str()));
        } else if (mode == 1) {
            QApplication::clipboard()->setText(QString::fromUtf8(target.filename().c_str()));
        } else {
            std::error_code ec;
            auto rel = std::filesystem::relative(target, m_activePath, ec);
            QApplication::clipboard()->setText(QString::fromUtf8(rel.c_str()));
        }
    });
    connect(menu, &TuiContextMenu::openTerminalRequested, this, [this]() {
        Core::XdgUtils::openTerminalAt(m_activePath);
    });
    connect(menu, &TuiContextMenu::propertiesRequested, this, [this, sel]() {
        emit propertiesRequested(sel.empty() ? m_activePath : sel.front());
    });
    connect(menu, &TuiContextMenu::pluginRequested, this, [this, sel](const QString& id) {
        emit pluginRunRequested(id, sel.empty() ? m_activePath : sel.front(), m_activePath);
    });

    menu->popup(globalPos);
}

void PreviewPane::contextMenuEvent(QContextMenuEvent* event) {
    if (m_activePath.empty()) {
        QWidget::contextMenuEvent(event);
        return;
    }

    auto menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setFont(Theme::getMonospaceFont(9));
    menu->setStyleSheet(Theme::getApplicationStyleSheet());

    auto actOpen = menu->addAction("\U000F0214 Open\tEnter");
    auto actTerm = menu->addAction("\U000F0489 Open Terminal\tF4");
    menu->addSeparator();
    auto actCopyPath = menu->addAction("\U000F0337 Copy Path\tCtrl+Shift+C");
    auto actCopyName = menu->addAction("\U000F018F Copy Name");
    menu->addSeparator();
    auto actProps = menu->addAction("\U000F02FD Properties\tAlt+Enter");

    connect(actOpen, &QAction::triggered, this, [this]() {
        if (Core::XdgUtils::isArchive(m_activePath)) {
            emit archiveOpenRequested(m_activePath);
            return;
        }
        Core::XdgUtils::openWithDefault(m_activePath);
    });
    connect(actTerm, &QAction::triggered, this, [this]() {
        std::error_code ec;
        bool isDir = std::filesystem::is_directory(m_activePath, ec);
        Core::XdgUtils::openTerminalAt(isDir ? m_activePath : m_activePath.parent_path());
    });
    connect(actProps, &QAction::triggered, this, [this]() {
        emit propertiesRequested(m_activePath);
    });
    connect(actCopyPath, &QAction::triggered, this, [this]() {
        QApplication::clipboard()->setText(QString::fromUtf8(m_activePath.c_str()));
    });
    connect(actCopyName, &QAction::triggered, this, [this]() {
        QApplication::clipboard()->setText(QString::fromUtf8(m_activePath.filename().c_str()));
    });

    menu->popup(event->globalPos());
}

static bool getFileStatSignature(const std::filesystem::path& p,
                                 int64_t& outMtimeSec,
                                 int64_t& outMtimeNsec,
                                 uintmax_t& outSize) {
    struct stat st{};
    if (stat(p.c_str(), &st) != 0) {
        return false;
    }
    outMtimeSec = static_cast<int64_t>(st.st_mtim.tv_sec);
    outMtimeNsec = static_cast<int64_t>(st.st_mtim.tv_nsec);
    outSize = static_cast<uintmax_t>(st.st_size);
    return true;
}

void PreviewPane::performClearNow() {
    if (m_stackedWidget) {
        int pg = m_stackedWidget->currentIndex();
        if (pg == PageConflictResolver) {
            return;
        }
    }
    resetDocumentPreview();
    (*m_currentJobId)++;
    m_activePath.clear();
    m_activeMtimeSec = 0;
    m_activeMtimeNsec = 0;
    m_activeFileSize = 0;
    m_currentImage = QImage();
    if (m_imageViewer) {
        m_imageViewer->clear();
    }
    if (m_emptyLabel) m_emptyLabel->setText("Select an item to preview");
    if (m_stackedWidget) m_stackedWidget->setCurrentIndex(PageEmpty);
}

void PreviewPane::prefetchPath(const std::filesystem::path& path) {
    if (path.empty() || path == m_activePath) return;
    const bool video = Core::Search::isVideoFile(path);
    const bool pdf = Core::Search::isPdfFile(path);
    if (!video && !pdf) return;

    const QString key = QString::fromUtf8(path.c_str());
    if (m_prefetchInFlight.contains(key) || m_prefetchInFlight.size() >= 2) return;
    for (const CachedImageEntry& entry : m_imageCache) {
        if (entry.path == path) return;
    }
    m_prefetchInFlight.insert(key);

    int64_t mtimeSec = 0, mtimeNsec = 0;
    uintmax_t fileSize = 0;
    getFileStatSignature(path, mtimeSec, mtimeNsec, fileSize);
    QPointer<PreviewPane> self(this);

    Core::Tasks::TaskManager::instance().poolForKind(QStringLiteral("preview"))->start(
        [self, path, key, video, mtimeSec, mtimeNsec, fileSize]() {
        QImage image = Core::Thumbnails::loadCached(path, video ? 0 : 1);
        QString info;
        if (image.isNull()) {
            if (video) {
                auto rendered = Core::Thumbnails::renderVideo(path, 480);
                image = rendered.image;
                info = rendered.info;
            } else {
                auto rendered = Core::Thumbnails::renderPdfPage(path, 1, kPdfPageRenderWidth);
                image = rendered.image;
                info = rendered.info;
            }
        }
        QMetaObject::invokeMethod(qApp,
            [self, path, key, video, mtimeSec, mtimeNsec, fileSize,
             image = std::move(image), info]() mutable {
            if (!self) return;
            self->m_prefetchInFlight.remove(key);
            if (!video || image.isNull()) return;
            for (const CachedImageEntry& entry : self->m_imageCache) {
                if (entry.path == path) return;
            }
            self->storeInImageCache(
                {path, mtimeSec, mtimeNsec, fileSize, image, image.size(),
                 info.isEmpty() ? QStringLiteral("Video") : info});
        }, Qt::QueuedConnection);
    });
}

bool PreviewPane::archivePageActive() const {
    return m_stackedWidget && m_stackedWidget->currentIndex() == PageArchive;
}

void PreviewPane::clearArchiveSelection() {
    if (m_archiveTree) m_archiveTree->clearEntrySelection();
}

void PreviewPane::selectAllArchiveEntries() {
    if (archivePageActive() && m_archiveTree) m_archiveTree->selectAllEntries();
}

void PreviewPane::selectFirstArchiveEntry() {
    if (archivePageActive() && m_archiveTree) m_archiveTree->selectFirstEntry();
}

void PreviewPane::selectLastArchiveEntry() {
    if (archivePageActive() && m_archiveTree) m_archiveTree->selectLastEntry();
}

void PreviewPane::copyArchiveSelection() {
    if (archivePageActive() && m_archiveTree) m_archiveTree->copySelectedEntries();
}

void PreviewPane::clearPreview() {
    if (m_stackedWidget) {
        int pg = m_stackedWidget->currentIndex();
        if (pg == PageConflictResolver) {
            return;
        }
    }
    // не сбрасываем синхронно в PageEmpty: при пересканировании каталога (watcher / rename / delete)
    // выделение на миг очищается и восстанавливается, отложенная очистка убирает мигание пустотой
    m_hasPending = false;
    m_pendingPath.clear();
    m_pendingClear = true;
    if (m_previewDebounce) {
        m_previewDebounce->start();
    } else {
        performClearNow();
    }
}

void PreviewPane::resetDocumentPreview() {
    ++m_documentGeneration;
    if (m_documentCancel) m_documentCancel->store(true);
    m_documentCancel.reset();
    m_documentPath.clear();
    m_documentPages = -1;
    if (m_documentViewer) m_documentViewer->reset();
}

void PreviewPane::startDocumentPreview(const std::filesystem::path& path, int pageCount) {
    resetDocumentPreview();
    m_documentPath = path;
    m_documentPages = pageCount;
    m_documentCancel = std::make_shared<std::atomic_bool>(false);
    if (!m_documentViewer) return;
    m_documentViewer->setDocument(path, pageCount);
    m_stackedWidget->setCurrentIndex(PageDocument);
    m_documentViewer->setFocus(Qt::OtherFocusReason);
}

void PreviewPane::requestDocumentPage(int page) {
    if (m_documentPath.empty() || m_activePath != m_documentPath ||
        !m_documentCancel || page < 1) {
        return;
    }

    const int generation = m_documentGeneration;
    const std::filesystem::path path = m_documentPath;
    auto cancelToken = m_documentCancel;
    QPointer<PreviewPane> self(this);

    Core::Tasks::TaskManager::instance().poolForKind(QStringLiteral("preview"))->start(
        [self, generation, path, page, cancelToken]() {
        QImage image = Core::Thumbnails::loadCached(path, page);
        QString error;
        if (image.isNull()) {
            auto rendered = Core::Thumbnails::renderPdfPage(path, page, kPdfPageRenderWidth, cancelToken);
            if (rendered.cancelled || cancelToken->load()) return;
            image = rendered.image;
            error = rendered.error;
        }
        QMetaObject::invokeMethod(qApp, [self, generation, page, image = std::move(image), error]() mutable {
            if (!self || generation != self->m_documentGeneration) return;
            if (!image.isNull()) {
                self->m_documentViewer->addPage(page, image);
            } else {
                self->m_documentViewer->pageFailed(page, error, page > 1);
            }
        }, Qt::QueuedConnection);
    });
}

void PreviewPane::displayImageImmediate(const std::filesystem::path& path,
                                        int64_t mtimeSec, int64_t mtimeNsec, uintmax_t fileSize,
                                        const QImage& img, const QSize& originalSize,
                                        const QString& info) {
    setUpdatesEnabled(false);
    m_activePath = path;
    m_activeMtimeSec = mtimeSec;
    m_activeMtimeNsec = mtimeNsec;
    m_activeFileSize = fileSize;
    m_currentImage = img;
    if (m_stackedWidget->currentIndex() != PageImage) {
        m_stackedWidget->setCurrentIndex(PageImage);
        if (m_imageContainer && m_imageContainer->layout()) {
            m_imageContainer->layout()->activate();
        }
    }
    m_imageViewer->setImage(m_currentImage, originalSize, info, path);
    setUpdatesEnabled(true);
    update();
}

void PreviewPane::storeInImageCache(CachedImageEntry entry) {
    for (auto it = m_imageCache.begin(); it != m_imageCache.end(); ++it) {
        if (it->path == entry.path) {
            m_imageCache.erase(it);
            break;
        }
    }
    m_imageCache.push_back(std::move(entry));

    qint64 totalBytes = 0;
    for (const auto& cached : m_imageCache) {
        totalBytes += cached.image.sizeInBytes();
    }
    // Старые записи вытесняются по общему бюджету, а не только по числу
    // файлов: снимки полного разрешения легко превращались в сотни мегабайт
    while (m_imageCache.size() > 1 &&
           (m_imageCache.size() > kImageCacheMaxEntries ||
            totalBytes > kImageCacheBudgetBytes)) {
        totalBytes -= m_imageCache.front().image.sizeInBytes();
        m_imageCache.erase(m_imageCache.begin());
    }
}

void PreviewPane::previewPath(const std::filesystem::path& path) {
    if (m_stackedWidget->currentIndex() == PageConflictResolver) {
        return;
    }
    if (path.empty()) {
        clearPreview();
        return;
    }

    m_pendingClear = false;

    // Каталоги показываем сразу, потому что листинг грузится асинхронно
    // и умеет отменяться. Задержка только замораживала превью, пока
    // пользователь ведёт фокус стрелками.
    std::error_code dirEc;
    if (std::filesystem::is_directory(path, dirEc)) {
        if (m_previewDebounce) m_previewDebounce->stop();
        m_hasPending = false;
        m_pendingPath.clear();
        loadPathNow(path);
        return;
    }

    int64_t mtimeSec = 0, mtimeNsec = 0;
    uintmax_t fileSize = 0;
    bool hasStat = getFileStatSignature(path, mtimeSec, mtimeNsec, fileSize);

    // 1. если тот же файл с теми же mtime и размером уже показан, ничего не делаем
    if (m_activePath == path && m_stackedWidget->currentIndex() != PageEmpty &&
        (!hasStat || (mtimeSec == m_activeMtimeSec && mtimeNsec == m_activeMtimeNsec && fileSize == m_activeFileSize))) {
        if (m_previewDebounce) m_previewDebounce->stop();
        m_hasPending = false;
        m_pendingPath.clear();
        return;
    }

    // 2. попадание в LRU кэш в стиле Nautilus: если изображение уже декодировано и mtime/размер совпадают,
    // подменяем его на месте за 0 мс без задержки debounce и обращений к диску
    if (hasStat) {
        for (auto it = m_imageCache.begin(); it != m_imageCache.end(); ++it) {
            if (it->path == path && it->mtimeSec == mtimeSec &&
                it->mtimeNsec == mtimeNsec && it->fileSize == fileSize) {
                CachedImageEntry hit = *it;
                m_imageCache.erase(it);
                m_imageCache.push_back(hit);
                if (m_previewDebounce) m_previewDebounce->stop();
                m_hasPending = false;
                m_pendingPath.clear();
                (*m_currentJobId)++;
                displayImageImmediate(path, mtimeSec, mtimeNsec, fileSize,
                                      hit.image, hit.originalSize, hit.info);
                return;
            }
        }
    }

    if (m_hasPending && m_pendingPath == path) {
        return;
    }
    m_pendingPath = path;
    m_hasPending = true;
    if (m_previewDebounce) m_previewDebounce->start();
}

void PreviewPane::loadPathNow(const std::filesystem::path& path, bool skipPlugin) {
    if (m_stackedWidget->currentIndex() == PageConflictResolver) {
        return;
    }
    if (path.empty()) {
        performClearNow();
        return;
    }

    if (!Core::Search::isPdfFile(path)) resetDocumentPreview();

    int64_t mtimeSec = 0, mtimeNsec = 0;
    uintmax_t fileSize = 0;
    bool hasStat = getFileStatSignature(path, mtimeSec, mtimeNsec, fileSize);

    if (m_activePath == path && m_stackedWidget->currentIndex() != PageEmpty &&
        (!hasStat || (mtimeSec == m_activeMtimeSec && mtimeNsec == m_activeMtimeNsec && fileSize == m_activeFileSize))) {
        return;
    }

    m_activePath = path;
    m_activeMtimeSec = mtimeSec;
    m_activeMtimeNsec = mtimeNsec;
    m_activeFileSize = fileSize;
    uint64_t jobId = ++(*m_currentJobId);

    std::error_code ec;
    bool isDir = std::filesystem::is_directory(path, ec);

    // случай 1: превью каталога
    if (isDir) {
        m_dirModel->setDirectory(path);
        m_stackedWidget->setCurrentIndex(PageDir);
        return;
    }

    // случай 1.5: архив, интерактивный листинг содержимого (асинхронная
    // загрузка внутри модели, страница показывается сразу с «reading…»
    if (Core::XdgUtils::isArchive(path)) {
        if (m_archiveTree) m_archiveTree->setArchivePath(path);
        m_archiveModel->setArchive(path);
        m_stackedWidget->setCurrentIndex(PageArchive);
        return;
    }

    // нативные плагины превью: рендер вызывается в пуле потоков,
    // внутри доступен только write, пустой вывод откат к встроенному превью
    if (!skipPlugin && NativePluginHost::instance().hasPreview(path)) {
        QPointer<PreviewPane> self(this);
        auto jobCounter = m_currentJobId;
        Core::Tasks::TaskManager::instance().poolForKind(QStringLiteral("preview"))->start([self, jobCounter, path, jobId]() {
            QByteArray buf;
            NativePluginHost::instance().renderPreview(
                path, [&buf](const char* data, size_t len) { buf.append(data, len); });
            const QString text = QString::fromUtf8(buf);
            if (!self || jobCounter->load() != jobId) return;
            if (!text.trimmed().isEmpty()) {
                QMetaObject::invokeMethod(self.data(), [self, jobId, jobCounter, text]() {
                    if (!self || jobCounter->load() != jobId) return;
                    self->m_codeViewer->setPlainText(text);
                    self->m_stackedWidget->setCurrentIndex(PageCode);
                }, Qt::QueuedConnection);
                return;
            }
            QMetaObject::invokeMethod(self.data(), [self, path, jobId, jobCounter]() {
                if (!self || jobCounter->load() != jobId) return;
                self->m_activePath.clear();
                self->loadPathNow(path, true);
            }, Qt::QueuedConnection);
        });
        return;
    }

    if (!skipPlugin) {
        const Core::Plugins::Plugin prevPlugin = Core::Plugins::previewPluginFor(path);
        if (!prevPlugin.id.isEmpty()) {
            const QString qPath = QString::fromUtf8(path.c_str());
            const std::filesystem::path parentPath = path.parent_path();
            const QString qParent = parentPath.empty() ? QString() : QString::fromUtf8(parentPath.c_str());
            QPointer<PreviewPane> self(this);
            auto jobCounter = m_currentJobId;
            Core::Tasks::TaskManager::instance().poolForKind(QStringLiteral("preview"))->start([self, jobCounter, path, jobId, prevPlugin, qPath, qParent]() {
                QString text;
                const QStringList parts = QProcess::splitCommand(prevPlugin.exec);
                if (!parts.isEmpty()) {
                    QProcess proc;
                    proc.setProcessChannelMode(QProcess::MergedChannels);
                    proc.setProgram(parts.first());
                    proc.setArguments(parts.mid(1) << qPath);
                    if (!qParent.isEmpty()) proc.setWorkingDirectory(qParent);
                    proc.setProcessEnvironment(Core::Plugins::pluginEnvironment(
                        qParent, {qPath}, QStringLiteral("preview")));
                    proc.start();
                    if (proc.waitForFinished(3000)) {
                        text = QString::fromUtf8(proc.readAllStandardOutput());
                    } else {
                        proc.kill();
                        proc.waitForFinished(1000);
                    }
                }
                if (!self || jobCounter->load() != jobId) return;
                if (!text.trimmed().isEmpty()) {
                    QMetaObject::invokeMethod(self.data(), [self, jobId, jobCounter, text]() {
                        if (!self || jobCounter->load() != jobId) return;
                        self->m_codeViewer->setPlainText(text);
                        self->m_stackedWidget->setCurrentIndex(PageCode);
                    }, Qt::QueuedConnection);
                    return;
                }
                // плагин не дал вывода, откат к встроенному превью
                QMetaObject::invokeMethod(self.data(), [self, path, jobId, jobCounter]() {
                    if (!self || jobCounter->load() != jobId) return;
                    self->m_activePath.clear();
                    self->loadPathNow(path, true);
                }, Qt::QueuedConnection);
            });
            return;
        }
    }

    QPointer<PreviewPane> self(this);
    auto jobCounter = m_currentJobId;

    // асинхронное выполнение в пуле потоков, чтобы не подвешивать интерфейс
    Core::Tasks::TaskManager::instance().poolForKind(QStringLiteral("preview"))->start([self, jobCounter, path, jobId, mtimeSec, mtimeNsec, fileSize]() {
        if (!self || jobCounter->load() != jobId) return;

        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            return;
        }

        // случай 1.9: PDF, страницы рендерит pdftoppm в фоновом пуле
        if (Core::Search::isPdfFile(path)) {
            const int pages = Core::Thumbnails::pdfPageCount(path);
            if (!self || jobCounter->load() != jobId) return;
            QMetaObject::invokeMethod(self.data(), [self, jobCounter, jobId, path, pages]() {
                if (!self || jobCounter->load() != jobId) return;
                self->startDocumentPreview(path, pages);
            }, Qt::QueuedConnection);
            return;
        }

        // случай 2: изображение или SVG файл
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });

        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp" ||
            ext == ".avif" || ext == ".bmp" || ext == ".gif" || ext == ".ico" || ext == ".svg") {

            auto storeAndDisplay = [self, jobCounter, jobId, path, mtimeSec, mtimeNsec, fileSize](
                                       QImage img, const QSize& originalSize, const QString& info) {
                if (!self) return;
                QMetaObject::invokeMethod(self.data(), [self, jobCounter, jobId, path, mtimeSec, mtimeNsec,
                                                       fileSize, img = std::move(img), originalSize, info]() {
                    if (!self || jobCounter->load() != jobId) return;
                    self->storeInImageCache({path, mtimeSec, mtimeNsec, fileSize, img, originalSize, info});
                    self->displayImageImmediate(path, mtimeSec, mtimeNsec, fileSize, img, originalSize, info);
                }, Qt::QueuedConnection);
                // полный декод уже отмасштабирован и освобождён: возвращаем его страницы ядру
                releaseDecodeBuffers();
            };

            if (ext == ".svg") {
                QSvgRenderer renderer(QString::fromUtf8(path.c_str()));
                if (renderer.isValid()) {
                    QSize defSize = renderer.defaultSize();
                    if (defSize.isEmpty()) defSize = QSize(400, 400);
                    // Decompression bomb: width/height="100000" в SVG дали бы
                    // QImage на десятки ГБ ещё до показа, не рендерим
                    static constexpr qint64 kMaxImagePixels = 16384LL * 16384LL;
                    if (qint64(defSize.width()) * qint64(defSize.height()) > kMaxImagePixels) {
                        const QString msg = QString("Image too large to preview: %1 x %2")
                                                .arg(defSize.width()).arg(defSize.height());
                        if (!self) return;
                        QMetaObject::invokeMethod(self.data(), [self, jobCounter, jobId, msg]() {
                            if (self && jobCounter->load() == jobId) {
                                self->m_mediaViewer->setPlainText(msg);
                                self->m_stackedWidget->setCurrentIndex(PageMedia);
                            }
                        }, Qt::QueuedConnection);
                        return;
                    }
                    const QSize renderSize = boundedSize(defSize, kPreviewMaxSide);
                    QImage img(renderSize, QImage::Format_ARGB32_Premultiplied);
                    img.fill(Qt::transparent);
                    QPainter p(&img);
                    renderer.render(&p, QRectF(QPointF(0, 0), QSizeF(renderSize)));
                    p.end();

                    uintmax_t sizeBytes = std::filesystem::file_size(path, ec);
                    if (ec) sizeBytes = 0;
                    QString info = QString("SVG Vector Image | %1 \u00d7 %2 | %3")
                                       .arg(defSize.width()).arg(defSize.height())
                                       .arg(Core::XdgUtils::formatSize(sizeBytes));

                    storeAndDisplay(std::move(img), defSize, info);
                    return;
                }
            } else {
                QImageReader reader(QString::fromUtf8(path.c_str()));
                reader.setAutoTransform(true);
                QSize origSize = reader.size();
                // Decompression bomb (сжатый PNG/JPEG может декодироваться в
                // гигабайты ARGB): ограничиваем число пикселей до decode
                static constexpr qint64 kMaxImagePixels = 16384LL * 16384LL;
                if (origSize.isValid() &&
                    qint64(origSize.width()) * qint64(origSize.height()) > kMaxImagePixels) {
                    const QString msg = QString("Image too large to preview: %1 x %2")
                                            .arg(origSize.width()).arg(origSize.height());
                    if (!self) return;
                    QMetaObject::invokeMethod(self.data(), [self, jobCounter, jobId, msg]() {
                        if (self && jobCounter->load() == jobId) {
                            self->m_mediaViewer->setPlainText(msg);
                            self->m_stackedWidget->setCurrentIndex(PageMedia);
                        }
                    }, Qt::QueuedConnection);
                    return;
                }
                const QSize decodeSize = boundedSize(origSize, kPreviewMaxSide);
                if (decodeSize.isValid() && decodeSize != origSize) {
                    reader.setScaledSize(decodeSize);
                }
                QImage img = reader.read();

                if (!img.isNull()) {
                    const QSize originalSize = origSize.isValid() ? origSize : img.size();
                    // некоторые плагины игнорируют setScaledSize для подстраховки
                    if (img.width() > kPreviewMaxSide || img.height() > kPreviewMaxSide) {
                        img = img.scaled(boundedSize(img.size(), kPreviewMaxSide),
                                         Qt::KeepAspectRatio, Qt::SmoothTransformation);
                    }
                    uintmax_t sizeBytes = std::filesystem::file_size(path, ec);
                    if (ec) sizeBytes = 0;
                    QString info = QString("%1 \u00d7 %2 | %3 | %4")
                                       .arg(originalSize.width())
                                       .arg(originalSize.height())
                                       .arg(QString::fromStdString(reader.format().toUpper().toStdString()))
                                       .arg(Core::XdgUtils::formatSize(sizeBytes));

                    storeAndDisplay(std::move(img), originalSize, info);
                    return;
                }
            }
        }

        // случай 2.5: видео, кадр через ffmpegthumbnailer/ffmpeg и дисковый кэш
        if (Core::Search::isVideoFile(path)) {
            std::error_code sizeEc;
            const uintmax_t sizeBytes = std::filesystem::file_size(path, sizeEc);
            QImage thumb = Core::Thumbnails::loadCached(path);
            QString info;
            QString error;
            if (thumb.isNull()) {
                auto rendered = Core::Thumbnails::renderVideo(path, 640);
                thumb = rendered.image;
                info = rendered.info;
                error = rendered.error;
            } else {
                info = QStringLiteral("Video · cached");
            }

            if (thumb.isNull()) {
                const QString fallback = QString(
                    "\n  [VIDEO PREVIEW UNAVAILABLE]\n\n"
                    "  Name: %1\n"
                    "  Size: %2\n\n"
                    "  %3\n\n"
                    "  Install ffmpegthumbnailer or ffmpeg for a thumbnail.\n"
                    "  Double-click or Enter to play via default player.")
                    .arg(QString::fromUtf8(path.filename().c_str()))
                    .arg(Core::XdgUtils::formatSize(sizeEc ? 0 : sizeBytes))
                    .arg(error.isEmpty() ? QStringLiteral("No frame extracted") : error);
                if (!self) return;
                QMetaObject::invokeMethod(self.data(), [self, jobCounter, jobId, fallback]() {
                    if (self && jobCounter->load() == jobId) {
                        self->m_mediaViewer->setPlainText(fallback);
                        self->m_stackedWidget->setCurrentIndex(PageMedia);
                    }
                }, Qt::QueuedConnection);
                return;
            }

            if (info.isEmpty()) info = QStringLiteral("Video");
            if (!self) return;
            QMetaObject::invokeMethod(self.data(),
                [self, jobCounter, jobId, path, mtimeSec, mtimeNsec, fileSize,
                 thumb = std::move(thumb), info]() mutable {
                if (!self || jobCounter->load() != jobId) return;
                self->storeInImageCache(
                    {path, mtimeSec, mtimeNsec, fileSize, thumb, thumb.size(), info});
                self->displayImageImmediate(path, mtimeSec, mtimeNsec, fileSize,
                                            thumb, thumb.size(), info);
            }, Qt::QueuedConnection);
            return;
        }

        // случай 3: медиафайл (аудио / видео)
        if (ext == ".mp3" || ext == ".flac" || ext == ".wav" || ext == ".ogg" || ext == ".m4a" ||
            ext == ".mp4" || ext == ".mkv" || ext == ".avi" || ext == ".webm" || ext == ".mov") {

            uintmax_t sizeBytes = std::filesystem::file_size(path, ec);
            if (ec) sizeBytes = 0;
            QString info = QString("\n  [MEDIA FILE INFORMATION]\n\n"
                                   "  Name: %1\n"
                                   "  Format: %2\n"
                                   "  Size: %3 (%4 bytes)\n\n"
                                   "  Double-click or Enter to play via default system player.")
                               .arg(QString::fromUtf8(path.filename().c_str()))
                               .arg(QString::fromStdString(ext.substr(ext.empty() ? 0 : 1)).toUpper())
                               .arg(Core::XdgUtils::formatSize(sizeBytes))
                               .arg(sizeBytes);

            if (!self) return;
            QMetaObject::invokeMethod(self.data(), [self, jobCounter, jobId, info]() {
                if (self && jobCounter->load() == jobId) {
                    self->m_mediaViewer->setPlainText(info);
                    self->m_stackedWidget->setCurrentIndex(PageMedia);
                }
            }, Qt::QueuedConnection);
            return;
        }

        // случай 4: превью текста / кода (лимит 512 KB)
        uintmax_t sizeBytes = std::filesystem::file_size(path, ec);
        if (ec) sizeBytes = 0;
        if (sizeBytes <= 512 * 1024) {
            QByteArray data;
            bool readOk = false;
            QFile file(QString::fromUtf8(path.c_str()));
            if (file.open(QIODevice::ReadOnly)) {
                data = file.read(64 * 1024);
                readOk = true;
            } else {
                // откат через sudo для файлов конфигов и текста, доступных только root (например limine.conf в /efi)
                QProcess proc;
                const QStringList headArgv = Core::XdgUtils::rootArgs(
                    {"head", "-c", "65536", QString::fromUtf8(path.c_str())});
                proc.start(headArgv.first(), headArgv.mid(1));
                if (proc.waitForFinished(1000) && proc.exitCode() == 0) {
                    data = proc.readAllStandardOutput();
                    readOk = true;
                }
            }

            if (readOk && !data.isEmpty()) {
                // простая эвристика на бинарность
                bool isBinary = false;
                for (int i = 0; i < std::min<int>(data.size(), 1024); ++i) {
                    if (data[i] == '\0') {
                        isBinary = true;
                        break;
                    }
                }

                if (!isBinary) {
                    QString content = QString::fromUtf8(data);
                    if (!self) return;
                    QMetaObject::invokeMethod(self.data(), [self, jobCounter, jobId, path, content]() {
                        if (self && jobCounter->load() == jobId) {
                            self->m_codeViewer->setSourceCode(content, QString::fromUtf8(path.extension().c_str()));
                            self->m_stackedWidget->setCurrentIndex(PageCode);
                        }
                    }, Qt::QueuedConnection);
                    return;
                }
            }
        }

        // случай 5: hex дамп бинарного файла
        QByteArray hexData;
        bool hexReadOk = false;
        QFile file(QString::fromUtf8(path.c_str()));
        if (file.open(QIODevice::ReadOnly)) {
            hexData = file.read(1024);
            hexReadOk = true;
        } else {
            QProcess proc;
            const QStringList headArgv = Core::XdgUtils::rootArgs(
                {"head", "-c", "1024", QString::fromUtf8(path.c_str())});
            proc.start(headArgv.first(), headArgv.mid(1));
            if (proc.waitForFinished(1000) && proc.exitCode() == 0) {
                hexData = proc.readAllStandardOutput();
                hexReadOk = true;
            }
        }

        if (hexReadOk) {
            std::ostringstream ss;
            for (int i = 0; i < hexData.size(); i += 16) {
                ss << std::hex << std::setw(8) << std::setfill('0') << i << "  ";
                for (int j = 0; j < 16; ++j) {
                    if (i + j < hexData.size()) {
                        ss << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(static_cast<unsigned char>(hexData[i + j])) << " ";
                    } else {
                        ss << "   ";
                    }
                }
                ss << " |";
                for (int j = 0; j < 16 && (i + j) < hexData.size(); ++j) {
                    unsigned char uc = static_cast<unsigned char>(hexData[i + j]);
                    ss << (std::isprint(uc) ? static_cast<char>(uc) : '.');
                }
                ss << "|\n";
            }

            QString hexDump = QString::fromStdString(ss.str());
            if (!self) return;
            QMetaObject::invokeMethod(self.data(), [self, jobCounter, jobId, hexDump]() {
                if (self && jobCounter->load() == jobId) {
                    self->m_hexViewer->setPlainText(hexDump);
                    self->m_stackedWidget->setCurrentIndex(PageHex);
                }
            }, Qt::QueuedConnection);
        }
    });
}

void PreviewPane::showConflictResolver(const std::vector<ConflictItem>& items,
                                       std::function<void(const std::vector<ConflictItem>&)> onApply,
                                       std::function<void()> onCancel) {
    if (m_previewDebounce) m_previewDebounce->stop();
    m_hasPending = false;
    m_pendingPath.clear();
    (*m_currentJobId)++;
    m_activePath.clear();
    m_conflictResolver->setupConflicts(items,
        [this, onApply](const std::vector<ConflictItem>& res) {
            m_stackedWidget->setCurrentIndex(PageEmpty);
            if (onApply) onApply(res);
        },
        [this, onCancel]() {
            m_stackedWidget->setCurrentIndex(PageEmpty);
            if (onCancel) onCancel();
        }
    );
    m_stackedWidget->setCurrentIndex(PageConflictResolver);
}


}

#include "PreviewPane.moc"
