#include "page.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>

Page::Page(const QString &title, AdminSession *session, bool scrolls, QWidget *parent)
    : QWidget(parent)
    , m_title(title)
    , m_session(session)
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto *header = new QWidget;
    auto *headerLayout = new QVBoxLayout(header);
    headerLayout->setContentsMargins(28, 22, 28, 12);
    headerLayout->setSpacing(2);
    auto *heading = new QLabel(title);
    heading->setProperty("role", "pageTitle");
    m_subtitle = new QLabel;
    m_subtitle->setProperty("role", "pageSubtitle");
    m_subtitle->setWordWrap(true);
    m_subtitle->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_subtitle->hide();
    headerLayout->addWidget(heading);
    headerLayout->addWidget(m_subtitle);
    outer->addWidget(header);

    auto *content = new QWidget;
    content->setObjectName(QStringLiteral("plane"));
    m_body = new QVBoxLayout(content);
    m_body->setContentsMargins(28, 4, 28, 24);
    m_body->setSpacing(14);

    if (scrolls) {
        auto *scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(content);
        outer->addWidget(scroll, 1);
        m_scrolled = content;
    } else {
        outer->addWidget(content, 1);
    }
}

void Page::setSubtitle(const QString &text)
{
    m_subtitle->setText(text);
    m_subtitle->setVisible(!text.isEmpty());
}

QWidget *Page::buttonRow(const QList<QWidget *> &buttons, QWidget *parent)
{
    auto *row = new QWidget(parent);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    for (QWidget *button : buttons)
        layout->addWidget(button);
    layout->addStretch(1);
    return row;
}
