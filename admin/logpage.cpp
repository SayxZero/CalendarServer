#include "logpage.h"

#include "adminsession.h"
#include "look.h"

#include <QCheckBox>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QScrollBar>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace {

/*! How much of the end of the file is shown: the last days of a quiet server. */
const qint64 kTail = 512 * 1024;

} // namespace

LogPage::LogPage(AdminSession *session, QWidget *parent)
    : Page(QStringLiteral("Журнал"), session, false, parent)
    , m_timer(new QTimer(this))
{
    m_path = Look::caption(QString());
    m_path->setTextInteractionFlags(Qt::TextSelectableByMouse);
    body()->addWidget(m_path);

    auto *row = new QHBoxLayout;
    m_follow = new QCheckBox(QStringLiteral("Следить за новыми строками"));
    m_follow->setChecked(true);
    m_problems = new QCheckBox(QStringLiteral("Только предупреждения и ошибки"));
    m_open = new QPushButton(QStringLiteral("Открыть в редакторе"));
    row->addWidget(m_follow);
    row->addWidget(m_problems);
    row->addStretch(1);
    row->addWidget(m_open);
    body()->addLayout(row);

    m_text = new QPlainTextEdit;
    m_text->setReadOnly(true);
    m_text->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_text->setMaximumBlockCount(20000);
    body()->addWidget(m_text, 1);

    connect(m_timer, &QTimer::timeout, this, [this] { reload(); });
    connect(m_follow, &QCheckBox::toggled, this, [this](bool on) {
        if (on)
            reload(true);
    });
    connect(m_problems, &QCheckBox::toggled, this, [this] { reload(true); });
    connect(m_open, &QPushButton::clicked, this, [this] {
        const QString file = this->session()->logPath();
#ifdef Q_OS_WIN
        QProcess::startDetached(QStringLiteral("notepad.exe"), { QDir::toNativeSeparators(file) });
#else
        QDesktopServices::openUrl(QUrl::fromLocalFile(file));
#endif
    });
}

void LogPage::activate()
{
    reload(true);
    m_timer->start(1500);
}

void LogPage::hideEvent(QHideEvent *event)
{
    m_timer->stop();
    Page::hideEvent(event);
}

void LogPage::reload(bool force)
{
    const QString path = session()->logPath();
    m_open->setEnabled(!path.isEmpty() && QFileInfo::exists(path));
    if (path.isEmpty()) {
        m_path->setText(QStringLiteral("Сервер пишет журнал только в консоль: в файл его пишет служба "
                                       "или сервер с параметром log_file в настройках."));
        m_text->clear();
        return;
    }
    const QFileInfo info(path);
    m_path->setText(QStringLiteral("%1%2").arg(QDir::toNativeSeparators(path),
                                                info.exists() ? QStringLiteral(" (%1)").arg(Look::bytes(info.size()))
                                                              : QStringLiteral(" — файла ещё нет")));
    if (!force && (!m_follow->isChecked()
                   || (info.size() == m_size && info.lastModified() == m_modified))) {
        return;
    }
    m_size = info.size();
    m_modified = info.lastModified();

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        m_text->setPlainText(info.exists() ? QStringLiteral("Файл не читается: %1").arg(file.errorString())
                                           : QString());
        return;
    }
    const bool cut = file.size() > kTail;
    if (cut)
        file.seek(file.size() - kTail);
    QString text = QString::fromUtf8(file.readAll());
    if (cut)
        text = text.mid(text.indexOf(QLatin1Char('\n')) + 1);   // the first line is cut
    if (m_problems->isChecked()) {
        QStringList kept;
        for (const QString &line : text.split(QLatin1Char('\n'))) {
            if (line.contains(QLatin1String("[!]")) || line.contains(QLatin1String("[!!]")))
                kept << line;
        }
        text = kept.join(QLatin1Char('\n'));
    }

    QScrollBar *bar = m_text->verticalScrollBar();
    const bool atEnd = bar->value() >= bar->maximum() - 4;
    m_text->setPlainText(text);
    if (atEnd || force)
        bar->setValue(bar->maximum());
}
