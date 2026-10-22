#pragma once

#include <QWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QStringList>
#include <filesystem>
#include <vector>

class QHBoxLayout;
class QLabel;
class QSplitter;

namespace Widgets {

class BookmarksBar;
class MillerViewContainer;
class TuiStatusBar;

// «Open Files» диалог выбора файла в полном дизайне основного окна: панель закладок + 3 колонки Miller,
// сверху в режиме Save строка имени, снизу адресная строка как в основном окне.
// Живёт оверлеем в MainWindow и отдельным окном в PortalService
class FileChooserDialog : public QWidget {
    Q_OBJECT

public:
    explicit FileChooserDialog(QWidget* parent = nullptr);

    // каталог в колонках, несуществующее или не каталог игнорируется
    void setDirectory(const std::filesystem::path& dir);
    std::filesystem::path currentDirectory() const;
    // выделенный в колонках файл (пусто, если ничего или выделен каталог)
    std::filesystem::path selectedPath() const;
    void updateTheme();

    // режимы портального backend'а XDG (системный «выбрать файл» из браузера, см. PortalService)
    void setMultipleSelectionEnabled(bool enabled);
    void setDirectoryMode(bool enabled);
    void setSaveMode(bool enabled, const QString& initialName = QString());
    // надпись главной кнопки (портал accept_label, «Extract In» задаёт Extract)
    void setAcceptLabel(const QString& label);
    // все выделенные файлы (для multiple), без выделения текущий
    QStringList selectedFilePaths() const;
    // фокус на основной элемент ввода (колонки или поле имени при сохранении)
    void focusPrimaryInput();

signals:
    void fileChosen(const QString& path);
    void filesChosen(const QStringList& paths);
    void closeRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void setupUi();
    void onDirectoryChanged(const std::filesystem::path& dir);
    // двойной клик/Enter по файлу в колонках: Save подставляет имя, Open(multiple) filesChosen, одиночный Open fileChosen
    void onFileActivated(const std::filesystem::path& path);
    void acceptCurrent();

    QSplitter* m_bodySplitter{nullptr};
    BookmarksBar* m_bookmarksBar{nullptr};
    MillerViewContainer* m_miller{nullptr};
    // нижняя адресная строка как в основном окне, тот же TuiStatusBar: путь, фильтр, счётчики, диск, бейдж sudo
    TuiStatusBar* m_statusBar{nullptr};
    // верхняя строка имени только в режиме сохранения; путь остаётся внизу в статусбаре
    QWidget* m_nameBar{nullptr};
    QWidget* m_actionBar{nullptr};
    QLabel* m_nameLabel{nullptr};
    QLineEdit* m_nameEdit{nullptr};
    QPushButton* m_cancelBtn{nullptr};
    QPushButton* m_openBtn{nullptr};
    bool m_multiple{false};
    bool m_directoryMode{false};
    bool m_saveMode{false};
    // надпись главной кнопки вне режима save (Open по умолчанию)
    QString m_acceptLabel{QStringLiteral("Open")};
};

}
