#pragma once

#include <QDialog>
#include <QLabel>
#include <QCheckBox>
#include <QPushButton>
#include <QHash>
#include <QSet>
#include <QList>
#include <QElapsedTimer>
#include <QByteArray>
#include <filesystem>
#include <atomic>
#include <cstdint>

class QTextEdit;
class QProcess;
class QLineEdit;

namespace Widgets {

class PropertiesDialog : public QDialog {
    Q_OBJECT

public:
    explicit PropertiesDialog(const std::filesystem::path& path, QWidget* parent = nullptr);
    ~PropertiesDialog() override;

    // встроенный режим: виджет живёт страницей внутри PreviewPane, без
    // отдельного окна; Close/Esc → сигнал closeRequested
    void setEmbedded(bool embedded);
    void setPath(const std::filesystem::path& p);

    // компактные свойства (по умолчанию): без консоли Disk Usage, для
    // для каталогов в поле size считается только вес папки через du
    // для корня диска/ФС (точки монтирования, напр. /, /home, /mnt/usb) и
    // только для него показывается консоль Disk Usage с разбивкой по
    // подпапкам и заполнением ФС автоматически, без отдельных клавиш
    bool diskUsageVisible() const { return m_diskUsageShown; }

    int exec() override;

signals:
    void closeRequested();

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    void setupUi();
    void loadFileInfo();
    void applyPermissions();

    // компактные свойства: вес каталога одним числом через du без консоли
    // при отказе молчаливая проба sudo при живом тикете иначе знак вопроса
    // диалогов и паролей здесь нет
    void startPathWeightScan();
    void runPathWeightProc(bool viaSudo);
    void stopPathWeightProc();

    // Disk Usage классическая консоль внутри диалога (только каталоги):
    // шапка файловой системы и таблица видны мгновенно а скан du идет в фоне
    // течёт асинхронно (readyRead в поток GUI), таблица и полоса прогресса
    // наполняются по ходу скана. Закрытые подпапки показываются «?» и
    // измеряются последними ровно одним привилегированным вызовом на весь
    // скан: живой тикет sudo молча иначе один батч pkexec на все пути
    // без создания лишних диалогов пароля
    // sudoers не создаются. Если pkexec недоступен или диалог отклонён, чип
    // инлайн ввод пароля sudo проверяет пароль и делает тикет
    // а дальше продолжается скан без повторного запроса
    // полного скана после авторизации нет
    void startDiskUsageScan();
    void startSudoDiskUsage();
    void startSudoBatch();
    void startSudoPhaseProc(const QStringList& argv, bool batch);
    void startParallelDiskScan();
    void startDiskChildWave(const QStringList& names);
    void onParallelDuError(QProcess* proc);
    void spawnParallelDu();
    void onParallelDuOutput(QProcess* proc);
    void onParallelDuDone(QProcess* proc);
    void killDiskPhaseProcs();
    void armDiskScanCap(uint64_t job, bool sudoPhase);
    void rearmDiskScanCapIfActive();
    void stopDiskUsageProc();
    void onDiskDuOutput();
    void collectDiskDuErrors();
    bool parseDiskDuRecord(const QByteArray& rec);
    void onDiskDuFinished(bool sudoPhase, int exitCode);
    void noteDiskSudoNeeded();
    void onDiskPasswordEntered();
    void startRestrictedMeasurement();
    void fallbackToInlinePassword(const QString& reason);
    void declineSudoAuthorization(const QString& reason);
    void updateDiskStatusRow();
    void recomputeDiskUnknown();
    void finalizeDiskUsage();
    void scheduleDiskRender();
    void renderDiskUsage();

    std::filesystem::path m_path;

    QLabel* m_nameLabel{nullptr};
    QLabel* m_locationLabel{nullptr};
    QLabel* m_typeLabel{nullptr};
    QLabel* m_sizeLabel{nullptr};
    QLabel* m_modifiedLabel{nullptr};
    QLabel* m_inodeLabel{nullptr};
    QLabel* m_ownerLabel{nullptr};

    // компактные свойства: вес папки через du а счетчик заданий отсекает устаревшие ответы
    // устаревшие сигналы при повторных setPath
    bool m_diskUsageShown{false};         // путь корень диска/ФС: консоль видна
    std::atomic<uint64_t> m_weightJob{0};
    QProcess* m_weightProc{nullptr};

    // Disk Usage: счётчик job отсекает устаревшие сигналы при повторных
    // setPath/закрытии, процессы du/sudo убиваются при рестарте и разрушении
    QWidget* m_diskUsageGroup{nullptr};
    QTextEdit* m_diskConsole{nullptr};

    // инлайн строка статуса над консолью: чип «scanning» вплотную к
    // полосе прогресса; пока ждём пароль, плашка «needed sudo» встаёт на место
    // «scanning» (та же ширина), бар не сбрасывается, а замирает;
    // поле пароля настоящее (QLineEdit), без попапов
    QWidget* m_diskStatusRow{nullptr};
    QLabel* m_diskScanChip{nullptr};      // scanning (плашка 13 знаков)
    QLabel* m_diskBarLabel{nullptr};      // [████░░] NN% | … (замирает на needed sudo)
    QLabel* m_diskNeededChip{nullptr};    // needed sudo встаёт на место scanning
    QLabel* m_diskPasswordCaption{nullptr};
    QLineEdit* m_diskPasswordEdit{nullptr};
    QLabel* m_diskSudoErrorLabel{nullptr};
    int m_diskBarShownPct{-1};            // что реально показал бар (последний кадр)
    int m_diskFrozenPct{-2};              // минус два не заморожен а минус один точки или процент
    int m_diskQueueBasePct{0};            // стартовая точка очереди (без отката)

    std::atomic<uint64_t> m_diskScanJob{0};
    QProcess* m_diskProc{nullptr};
    QString m_diskBaseHtml;              // Filesystem/Mount/Capacity/Available (мгновенно)
    QString m_diskRootClean;             // cleanPath текущего пути (сверка с выводом du)
    QSet<QString> m_diskChildren;        // подкаталоги первого уровня (для строк «?»)
    QHash<QString, qint64> m_diskSizes;  // имя подкаталога → байты
    QSet<QString> m_diskResolved;        // имена, для которых размер получен
    QSet<QString> m_diskRestricted;      // ошибки доступа в текущей фазе (неполный размер)
    QSet<QString> m_diskMountChildren;   // дети точки монтирования на другой файловой системе которых du пропускает
    bool m_diskMountWaveDone{false};     // их поштучная волна уже запускалась
    QSet<QString> m_diskUnknown;         // финальные строки «?»
    QSet<QString> m_diskProgressChildren; // дети из m_diskTotalDirs (одна ФС) только их считаем в прогрессе
    QByteArray m_diskOutBuf;             // недособранные записи NUL du
    QString m_diskErrBuf;                // недособранные строки stderr du
    int m_diskDoneDirs{0};
    int m_diskTotalDirs{0};
    bool m_diskScanning{false};
    bool m_diskSudoPhase{false};
    bool m_diskRootRestricted{false};    // недоступен сам каталог свойств
    bool m_diskRenderScheduled{false};
    QElapsedTimer m_diskLastRender;
    QElapsedTimer m_diskCapClock;        // кап по простою: тишина du без вывода
    uint64_t m_diskHardCapJob{0};        // абсолютный кап один раз на фазу
    bool m_diskHardCapPhase{false};

    // параллельная волна для медленных файловых систем ntfs или fuse
    // последовательный du упирается в latency отдельных запросов stat, поэтому
    // подпапки первого уровня сканируются одновременно через du
    bool m_diskParallelMode{false};
    QStringList m_diskParallelQueue;                 // оставшиеся подпапки
    QList<QProcess*> m_diskParallelProcs;            // активные du по подпапкам
    QHash<QProcess*, QByteArray> m_diskParallelBufs; // раздельные буферы записей NUL
    QHash<QProcess*, QString> m_diskParallelErrBufs; // stderr волны (Permission denied)

    // вторая волна (закрытые папки последними). Авторизация: живой
    // тикет sudo молча иначе один батч pkexec со всеми путями
    // один системный диалог; отказ или нет полкита → поле инлайн пароля
    QStringList m_diskSudoQueue;
    int m_diskSudoQueueTotal{0};
    bool m_diskSudoQueueRunning{false};
    bool m_diskSudoReady{false};         // авторизация уже получена (тикет/полкит)
    bool m_diskSudoNeeded{false};        // видна строка инлайн, ждём пароль
    bool m_diskSudoPkexec{false};        // привилегированный вызов идёт через pkexec
    bool m_diskPkexecAuthFailed{false};  // pkexec отказал/отменён (stderr маркеры)
    bool m_diskPkexecWaiting{false};     // открыт системный диалог полкита: бар заморожен
    bool m_diskSudoDeclined{false};      // авторизацию отклонили второй раз не спрашиваем
    bool m_diskSudoBatchRunning{false};  // идёт батч pkexec/sudo закрытых папок
    bool m_diskNormalDone{false};        // первая (обычная) фаза завершена

    QCheckBox* m_userRead{nullptr};
    QCheckBox* m_userWrite{nullptr};
    QCheckBox* m_userExec{nullptr};
    QCheckBox* m_groupRead{nullptr};
    QCheckBox* m_groupWrite{nullptr};
    QCheckBox* m_groupExec{nullptr};
    QCheckBox* m_otherRead{nullptr};
    QCheckBox* m_otherWrite{nullptr};
    QCheckBox* m_otherExec{nullptr};

    QLabel* m_octalLabel{nullptr};
    QPushButton* m_applyBtn{nullptr};
    QPushButton* m_closeBtn{nullptr};

    bool m_embedded{false};
};

}
