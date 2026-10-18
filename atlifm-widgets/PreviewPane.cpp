#include "PreviewPane.hpp"
#include "NativePlugins.hpp"
#include "../atlifm-core/Plugins.hpp"
#include "MillerColumns.hpp"
#include "FileItemDelegate.hpp"
#include "ContextMenu.hpp"
#include "TuiInputPopup.hpp"
#include "../atlifm-theme/Theme.hpp"
#include "../atlifm-core/XdgUtils.hpp"
#include "../atlifm-core/Icons.hpp"
#include "../atlifm-models/ArchiveModel.hpp"
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
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QApplication>
#include <QClipboard>
#include <QThreadPool>
#include <QPointer>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QScrollArea>
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
#include <sstream>
#include <iomanip>
#include <chrono>
#include <ctime>



namespace Widgets {

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
    }

    void setImage(const QImage& img, const QString& baseInfo, const std::filesystem::path& filePath = {}) {
        bool sameFileAndSize = (m_filePath == filePath && !m_image.isNull() && m_image.size() == img.size());
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
        m_image = QImage();
        m_scaledPixmap = QPixmap();
        m_scaledTargetSize = QSize();
        m_baseInfo.clear();
        m_filePath.clear();
        m_isInitialFit = true;
        update();
    }

    void resetToFit() {
        if (m_image.isNull() || width() <= 0 || height() <= 0) return;
        double sx = static_cast<double>(width()) / m_image.width();
        double sy = static_cast<double>(height()) / m_image.height();
        m_scale = std::min(sx, sy);
        if (m_scale > 1.0) m_scale = 1.0; // мелкие иконки по умолчанию не растягиваем выше 100%
        m_pan = QPointF((width() - m_image.width() * m_scale) / 2.0,
                        (height() - m_image.height() * m_scale) / 2.0);
        emit zoomChanged(currentZoomText());
    }

    QString currentZoomText() const {
        if (m_image.isNull()) return "";
        int pct = qRound(m_scale * 100.0);
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

        if (m_scale <= 2.0) {
            if (m_scaledPixmap.isNull() || m_scaledTargetSize != targetSize) {
                m_scaledTargetSize = targetSize;
                m_scaledPixmap = QPixmap::fromImage(
                    m_image.scaled(targetSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
            }
            p.drawPixmap(targetRect.topLeft(), m_scaledPixmap);
        } else {
            p.drawImage(targetRect, m_image);
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
            m_isInitialFit = true;
            resetToFit();
            update();
            event->accept();
            return;
        }
        QWidget::mouseDoubleClickEvent(event);
    }

private:
    QImage m_image;
    QPixmap m_scaledPixmap;
    QSize m_scaledTargetSize;
    QString m_baseInfo;
    std::filesystem::path m_filePath;
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

// интерактивный листинг архива (страница PageArchive)
// MillerListView поверх Models::ArchiveModel, при перетаскивании выбранные записи
// сначала распаковываются в кэш, наружу отдаются реальные файлы (Copy)

namespace {
constexpr char16_t kArchivePathSep = u'\x1F';

// кэш распакованных записей уникален для архива (путь + mtime + размер),
// повторные драги мгновенны, старые каталоги чистятся по возрасту
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
}

class ArchiveListView : public MillerListView {
    Q_OBJECT

public:
    explicit ArchiveListView(QWidget* parent = nullptr)
        : MillerListView(false, parent) {
        setObjectName("archiveList");
        connect(this, &MillerListView::contextMenuRequested, this, &ArchiveListView::showArchiveMenu);
    }

    // после загрузки или навигации курсор на первой рабочей строке
    void focusFirstRow() {
        if (!model() || model()->rowCount() == 0 || !selectionModel()) return;
        auto* am = qobject_cast<Models::ArchiveModel*>(model());
        const int r = (am && am->isParentRow(0)) ? 1 : 0;
        if (r >= model()->rowCount()) return;
        const QModelIndex idx = model()->index(r, 0);
        selectionModel()->setCurrentIndex(idx, QItemSelectionModel::NoUpdate);
        selectionModel()->select(idx, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        scrollTo(idx, QAbstractItemView::PositionAtTop);
    }

    // распаковка записей в кэш, возвращает реальные пути (для QDrag и открытия)
    // abortOnRelease: ожидание прерывается, если кнопку отпустили до готовности,
    // но фоновая распаковка продолжается и прогревает кэш
    bool extractToCache(const QStringList& members, const QStringList& normalizedPaths,
                        bool abortOnRelease,
                        std::vector<std::filesystem::path>& outRealPaths, QString& outError);

protected:
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    std::vector<std::filesystem::path> dragOutPaths(const QModelIndex& startIndex) const override;
    void startDragOut(const std::vector<std::filesystem::path>& paths,
                      Qt::KeyboardModifiers modifiers,
                      Qt::DropAction defaultAction) override;

private:
    void activateIndex(const QModelIndex& idx);
    void openEntry(int row);
    void showArchiveMenu(const QPoint& globalPos, const QModelIndex& idx);

    // кнопка жеста: пока нажата ожидание продолжается,
    // отпускание означает «не драг» и отменяет старт QDrag
    bool m_gestureButtonDown{false};
};

bool ArchiveListView::extractToCache(const QStringList& members, const QStringList& normalizedPaths,
                                     bool abortOnRelease,
                                     std::vector<std::filesystem::path>& outRealPaths,
                                     QString& outError) {
    outRealPaths.clear();
    outError.clear();
    auto* am = qobject_cast<Models::ArchiveModel*>(model());
    if (!am) return false;
    const std::filesystem::path archive = am->archivePath();
    if (archive.empty() || normalizedPaths.isEmpty()) return false;

    cleanupOldArchiveCacheOnce();
    const std::filesystem::path cacheDir = archiveCacheDir(archive);
    std::error_code ec;
    std::filesystem::create_directories(cacheDir, ec);
    if (ec) {
        outError = QStringLiteral("Cannot create cache folder: %1")
                       .arg(QString::fromUtf8(ec.message().c_str()));
        return false;
    }

    const bool singleStream = Core::XdgUtils::isSingleStreamArchive(archive);

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
        QThreadPool::globalInstance()->start([state, archive, extractMembers, cacheDir]() {
            QString err;
            bool ok = false;
            // QThreadPool не ловит исключения воркера, а state->done обязан быть
            // выставлен, иначе вложенный event loop ждёт его вечно
            try {
                ok = extractMembers.isEmpty()
                    ? Core::XdgUtils::extractArchive(archive, cacheDir, err)
                    : Core::XdgUtils::extractArchiveEntries(archive, extractMembers, cacheDir, err);
            } catch (const std::exception& e) {
                err = QString::fromUtf8(e.what());
            } catch (...) {
                err = QStringLiteral("Archive extraction failed");
            }
            QMutexLocker lock(&state->mutex);
            state->done = true;
            state->ok = ok;
            state->error = err;
        });

        // вложенный цикл событий держит GUI живым, при abortOnRelease ожидание
        // прекращается по отпусканию кнопки
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

        if (aborted) return false;
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

void ArchiveListView::activateIndex(const QModelIndex& idx) {
    auto* am = qobject_cast<Models::ArchiveModel*>(model());
    if (!am || !idx.isValid()) return;
    const int row = idx.row();
    if (am->isParentRow(row)) {
        if (am->navigateUp()) focusFirstRow();
        return;
    }
    if (am->isDirAt(row)) {
        am->navigateInto(am->entryPathAt(row));
        focusFirstRow();
        return;
    }
    openEntry(row);
}

void ArchiveListView::openEntry(int row) {
    auto* am = qobject_cast<Models::ArchiveModel*>(model());
    if (!am || row < 0) return;
    const QString norm = am->entryPathAt(row);
    const QString member = am->memberPathAt(row);
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

void ArchiveListView::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) m_gestureButtonDown = true;
    MillerListView::mousePressEvent(event);
}

void ArchiveListView::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) m_gestureButtonDown = false;
    MillerListView::mouseReleaseEvent(event);
}

void ArchiveListView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        const QModelIndex idx = indexAt(event->pos());
        if (idx.isValid()) {
            activateIndex(idx);
            event->accept();
            return;
        }
    }
    QListView::mouseDoubleClickEvent(event);
}

void ArchiveListView::keyPressEvent(QKeyEvent* event) {
    const int k = event->key();
    if (k == Qt::Key_Return || k == Qt::Key_Enter || k == Qt::Key_Right) {
        if (currentIndex().isValid()) activateIndex(currentIndex());
        return;
    }
    if (k == Qt::Key_Left || k == Qt::Key_Backspace) {
        auto* am = qobject_cast<Models::ArchiveModel*>(model());
        if (am && am->navigateUp()) focusFirstRow();
        return;
    }
    if (k == Qt::Key_Space) {
        const QModelIndex cur = currentIndex();
        if (cur.isValid() && selectionModel() && model()) {
            selectionModel()->select(cur, QItemSelectionModel::Toggle | QItemSelectionModel::Rows);
            const int nextRow = std::min(cur.row() + 1, model()->rowCount() - 1);
            const QModelIndex next = model()->index(nextRow, 0);
            if (next.isValid()) {
                selectionModel()->setCurrentIndex(next, QItemSelectionModel::NoUpdate);
                scrollTo(next, QAbstractItemView::EnsureVisible);
            }
        }
        return;
    }
    if (k == Qt::Key_A && (event->modifiers() & Qt::ControlModifier)) {
        selectAll();
        return;
    }
    MillerListView::keyPressEvent(event);
}

std::vector<std::filesystem::path> ArchiveListView::dragOutPaths(const QModelIndex& startIndex) const {
    std::vector<std::filesystem::path> out;
    auto* am = qobject_cast<Models::ArchiveModel*>(model());
    if (!am || !startIndex.isValid()) return out;

    QModelIndexList rows;
    if (selectionModel() && selectionModel()->isSelected(startIndex)) {
        rows = selectionModel()->selectedIndexes();
    } else {
        rows << startIndex;
    }
    if (rows.isEmpty()) rows << startIndex;
    std::sort(rows.begin(), rows.end(), [](const QModelIndex& a, const QModelIndex& b) {
        return a.row() < b.row();
    });

    for (const QModelIndex& i : rows) {
        if (am->isParentRow(i.row())) continue;
        const QString norm = am->entryPathAt(i.row());
        const QString member = am->memberPathAt(i.row());
        if (norm.isEmpty() || member.isEmpty()) continue;
        // формат: архив <0x1F> нормализованный путь <0x1F> имя в архиве
        const QString vp = QString::fromUtf8(am->archivePath().c_str()) + QChar(kArchivePathSep) +
                           norm + QChar(kArchivePathSep) + member;
        out.emplace_back(vp.toUtf8().toStdString());
    }
    return out;
}

void ArchiveListView::startDragOut(const std::vector<std::filesystem::path>& paths,
                                   Qt::KeyboardModifiers /*modifiers*/,
                                   Qt::DropAction /*defaultAction*/) {
    QStringList members;
    QStringList normalized;
    for (const auto& vp : paths) {
        const QString s = QString::fromUtf8(vp.c_str());
        const QStringList parts = s.split(QChar(kArchivePathSep));
        if (parts.size() != 3 || parts[1].isEmpty() || parts[2].isEmpty()) continue;
        normalized << parts[1];
        members << parts[2];
    }
    if (normalized.isEmpty()) return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    std::vector<std::filesystem::path> real;
    QString err;
    const bool ok = extractToCache(members, normalized, /*abortOnRelease=*/true, real, err);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        if (!err.isEmpty()) TuiConfirmPopup::alert(this, "Extraction Error", err);
        return;
    }

    QDrag* drag = Dnd::DragDropHandler::createDrag(real, this);
    if (drag) {
        // из архива только копирование: реальные файлы лежат во временном кэше
        drag->exec(Qt::CopyAction, Qt::CopyAction);
    }
}

void ArchiveListView::showArchiveMenu(const QPoint& globalPos, const QModelIndex& idx) {
    auto* am = qobject_cast<Models::ArchiveModel*>(model());
    if (!am) return;

    auto menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setFont(Theme::getMonospaceFont(9));
    menu->setStyleSheet(Theme::getApplicationStyleSheet());

    if (idx.isValid() && !am->isParentRow(idx.row())) {
        auto actOpen = menu->addAction("\U000F0214 Open\tEnter");
        connect(actOpen, &QAction::triggered, this, [this, idx]() { activateIndex(idx); });
        menu->addSeparator();
    }

    auto actExtract = menu->addAction("\U000F06EB Extract In");
    connect(actExtract, &QAction::triggered, this, [this, am]() {
        const std::filesystem::path arc = am->archivePath();
        const std::filesystem::path dest =
            arc.parent_path() / Core::XdgUtils::archiveExtractDirName(arc);
        QPointer<ArchiveListView> self(this);
        QThreadPool::globalInstance()->start([self, arc, dest]() {
            QString err;
            std::filesystem::path actualDest;
            bool ok = false;
            // исключение воркера не должно убить процесс
            try {
                ok = Core::XdgUtils::extractArchive(arc, dest, err, &actualDest);
            } catch (const std::exception& e) {
                err = QString::fromUtf8(e.what());
            } catch (...) {
                err = QStringLiteral("Archive extraction failed");
            }
            QMetaObject::invokeMethod(qApp, [self, ok, err]() {
                if (!self) return;
                if (!ok) TuiConfirmPopup::alert(self.data(), "Extraction Error", err);
            }, Qt::QueuedConnection);
        });
    });

    auto actCopy = menu->addAction("\U000F0337 Copy Archive Path");
    connect(actCopy, &QAction::triggered, this, [am]() {
        QApplication::clipboard()->setText(QString::fromUtf8(am->archivePath().c_str()));
    });

    menu->popup(globalPos);
}

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
    m_dirDelegate = new FileItemDelegate(true, this);
    m_dirListView->setModel(m_dirModel);
    m_dirListView->setItemDelegate(m_dirDelegate);
    m_dirListView->setContextMenuPolicy(Qt::CustomContextMenu);

    connect(m_dirListView, &MillerListView::enterDirectoryRequested, this, &PreviewPane::enterDirectoryRequested);
    connect(m_dirListView, &MillerListView::openFileRequested, this, [](const std::filesystem::path& p) {
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

        m_archiveInfoLabel = new QLabel(m_archivePage);
        m_archiveInfoLabel->setObjectName("archiveInfo");
        m_archiveInfoLabel->setFont(Theme::getMonospaceFont(8));
        m_archiveInfoLabel->setWordWrap(false);
        m_archiveInfoLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_archiveInfoLabel->setStyleSheet(
            QString("background-color: %1; color: %2; padding: 6px 10px;")
                .arg(Theme::Colors::Base().name(), Theme::Colors::Dim().name()));
        archiveLayout->addWidget(m_archiveInfoLabel, 0);

        m_archiveModel = new Models::ArchiveModel(this);
        m_archiveList = new ArchiveListView(m_archivePage);
        m_archiveList->setModel(m_archiveModel);
        // не dimmed: листинг интерактивный, в отличие от пассивного превью папки
        m_archiveList->setItemDelegate(new FileItemDelegate(false, m_archiveList));
        archiveLayout->addWidget(m_archiveList, 1);
    }
    connect(m_archiveModel, &Models::ArchiveModel::loaded, this, [this]() {
        updateArchiveInfo();
        if (m_archiveList) m_archiveList->focusFirstRow();
    });
    connect(m_archiveModel, &Models::ArchiveModel::loadFailed, this, [this](const QString&) {
        updateArchiveInfo();
    });
    connect(m_archiveModel, &Models::ArchiveModel::prefixChanged, this, [this]() {
        updateArchiveInfo();
    });
    m_stackedWidget->addWidget(m_archivePage);

    mainLayout->addWidget(m_stackedWidget);
    m_stackedWidget->setCurrentIndex(PageEmpty);

    // дебаунс превью: коалесцирует быстрые смены выделения и предотвращает
    // ложный сброс на PageEmpty при перезагрузке модели (watcher / rename / delete)
    m_previewDebounce = new QTimer(this);
    m_previewDebounce->setSingleShot(true);
    m_previewDebounce->setInterval(55);
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
    if (Dnd::DragDropHandler::canAcceptDrop(event->mimeData())) {
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

void PreviewPane::dragMoveEvent(QDragMoveEvent* event) {
    if (Dnd::DragDropHandler::canAcceptDrop(event->mimeData())) {
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

void PreviewPane::dropEvent(QDropEvent* event) {
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
        QThreadPool::globalInstance()->start([self, arc, dest]() {
            QString err;
            std::filesystem::path actualDest;
            bool ok = false;
            // исключение воркера не должно убить процесс
            try {
                ok = Core::XdgUtils::extractArchive(arc, dest, err, &actualDest);
            } catch (const std::exception& e) {
                err = QString::fromUtf8(e.what());
            } catch (...) {
                err = QStringLiteral("Archive extraction failed");
            }
            QMetaObject::invokeMethod(qApp, [self, ok, err]() {
                if (!self) return;
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
        emit extractInRequested(sel.front());
    });
    connect(menu, &TuiContextMenu::sudoOpenRequested, this, [this, sel]() {
        std::filesystem::path target = sel.empty() ? m_activePath : sel.front();
        std::error_code ec;
        bool isDir = std::filesystem::is_directory(target, ec);
        if (!isDir) {
            QProcess checkProc;
            const QStringList checkArgv = Core::XdgUtils::rootArgs({"test", "-d", QString::fromUtf8(target.c_str())});
            checkProc.start(checkArgv.first(), checkArgv.mid(1));
            if (checkProc.waitForFinished(400) && checkProc.exitCode() == 0) {
                isDir = true;
            }
        }
        if (isDir) {
            emit enterDirectoryRequested(target);
        } else {
            QString sudoErr;
            Core::XdgUtils::openFileWithSudo(target, sudoErr);
        }
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
    auto actSudo = menu->addAction("\U000F05D7 Sudo Open");
    menu->addSeparator();
    auto actCopyPath = menu->addAction("\U000F0337 Copy Path\tCtrl+Shift+C");
    auto actCopyName = menu->addAction("\U000F018F Copy Name");
    menu->addSeparator();
    auto actProps = menu->addAction("\U000F02FD Properties\tAlt+Enter");

    connect(actOpen, &QAction::triggered, this, [this]() {
        Core::XdgUtils::openWithDefault(m_activePath);
    });
    connect(actTerm, &QAction::triggered, this, [this]() {
        std::error_code ec;
        bool isDir = std::filesystem::is_directory(m_activePath, ec);
        Core::XdgUtils::openTerminalAt(isDir ? m_activePath : m_activePath.parent_path());
    });
    connect(actSudo, &QAction::triggered, this, [this]() {
        std::error_code ec;
        bool isDir = std::filesystem::is_directory(m_activePath, ec);
        if (!isDir) {
            QProcess checkProc;
            const QStringList checkArgv = Core::XdgUtils::rootArgs({"test", "-d", QString::fromUtf8(m_activePath.c_str())});
            checkProc.start(checkArgv.first(), checkArgv.mid(1));
            if (checkProc.waitForFinished(400) && checkProc.exitCode() == 0) {
                isDir = true;
            }
        }
        if (isDir) {
            emit enterDirectoryRequested(m_activePath);
        } else {
            QString sudoErr;
            Core::XdgUtils::openFileWithSudo(m_activePath, sudoErr);
        }
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

void PreviewPane::updateArchiveInfo() {
    if (!m_archiveInfoLabel || !m_archiveModel) return;
    const QString name = QString::fromUtf8(m_archiveModel->archivePath().filename().c_str());
    QString text;
    if (m_archiveModel->isLoading()) {
        text = QString("%1: reading…").arg(name);
    } else if (!m_archiveModel->error().isEmpty()) {
        text = QString("%1: preview unavailable: %2").arg(name, m_archiveModel->error());
    } else {
        text = QString("%1: %2 files, %3 dirs | %4")
                   .arg(name)
                   .arg(m_archiveModel->fileCount())
                   .arg(m_archiveModel->dirCount())
                   .arg(Core::XdgUtils::formatSize(m_archiveModel->totalSize()));
        if (m_archiveModel->isTruncated()) text += " | list truncated";
        if (!m_archiveModel->prefix().isEmpty()) text += " / " + m_archiveModel->prefix();
    }
    m_archiveInfoLabel->setText(text);
}

void PreviewPane::displayImageImmediate(const std::filesystem::path& path,
                                        int64_t mtimeSec, int64_t mtimeNsec, uintmax_t fileSize,
                                        const QImage& img, const QString& info) {
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
    m_imageViewer->setImage(m_currentImage, info, path);
    setUpdatesEnabled(true);
    update();
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
                displayImageImmediate(path, mtimeSec, mtimeNsec, fileSize, hit.image, hit.info);
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
        m_archiveModel->setArchive(path);
        updateArchiveInfo();
        m_stackedWidget->setCurrentIndex(PageArchive);
        return;
    }

    // нативные preview-плагины (*.so): render вызывается в пуле потоков,
    // внутри доступен только write, пустой вывод откат к встроенному превью
    if (!skipPlugin && NativePluginHost::instance().hasPreview(path)) {
        QPointer<PreviewPane> self(this);
        auto jobCounter = m_currentJobId;
        QThreadPool::globalInstance()->start([self, jobCounter, path, jobId]() {
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
            QThreadPool::globalInstance()->start([self, jobCounter, path, jobId, prevPlugin, qPath, qParent]() {
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
    QThreadPool::globalInstance()->start([self, jobCounter, path, jobId, mtimeSec, mtimeNsec, fileSize]() {
        if (!self || jobCounter->load() != jobId) return;

        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            return;
        }

        // случай 2: изображение или SVG файл
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });

        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp" ||
            ext == ".avif" || ext == ".bmp" || ext == ".gif" || ext == ".ico" || ext == ".svg") {

            auto storeAndDisplay = [self, jobCounter, jobId, path, mtimeSec, mtimeNsec, fileSize](QImage img, const QString& info) {
                if (!self) return;
                QMetaObject::invokeMethod(self.data(), [self, jobCounter, jobId, path, mtimeSec, mtimeNsec, fileSize, img = std::move(img), info]() {
                    if (!self || jobCounter->load() != jobId) return;
                    // обновляем LRU кэш (максимум 32 декодированных изображения, как в nautilus-image.c)
                    for (auto it = self->m_imageCache.begin(); it != self->m_imageCache.end(); ++it) {
                        if (it->path == path) {
                            self->m_imageCache.erase(it);
                            break;
                        }
                    }
                    if (self->m_imageCache.size() >= 32) {
                        self->m_imageCache.erase(self->m_imageCache.begin());
                    }
                    self->m_imageCache.push_back({path, mtimeSec, mtimeNsec, fileSize, img, info});
                    self->displayImageImmediate(path, mtimeSec, mtimeNsec, fileSize, img, info);
                }, Qt::QueuedConnection);
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
                    QImage img(defSize, QImage::Format_ARGB32_Premultiplied);
                    img.fill(Qt::transparent);
                    QPainter p(&img);
                    renderer.render(&p);
                    p.end();

                    uintmax_t sizeBytes = std::filesystem::file_size(path, ec);
                    if (ec) sizeBytes = 0;
                    QString info = QString("SVG Vector Image | %1 \u00d7 %2 | %3")
                                       .arg(defSize.width()).arg(defSize.height())
                                       .arg(Core::XdgUtils::formatSize(sizeBytes));

                    storeAndDisplay(std::move(img), info);
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
                QImage img = reader.read();

                if (!img.isNull()) {
                    uintmax_t sizeBytes = std::filesystem::file_size(path, ec);
                    if (ec) sizeBytes = 0;
                    QString info = QString("%1 \u00d7 %2 | %3 | %4")
                                       .arg(origSize.isValid() ? origSize.width() : img.width())
                                       .arg(origSize.isValid() ? origSize.height() : img.height())
                                       .arg(QString::fromStdString(reader.format().toUpper().toStdString()))
                                       .arg(Core::XdgUtils::formatSize(sizeBytes));

                    storeAndDisplay(std::move(img), info);
                    return;
                }
            }
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
