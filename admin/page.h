#pragma once

#include <QWidget>

class AdminSession;
class QLabel;
class QVBoxLayout;

/*!
 * A page of the panel: a heading with a line under it and the content below.
 * A page of cards scrolls as a whole; a page that fills the window with a
 * list of its own (the accounts) does not.
 */
class Page : public QWidget
{
    Q_OBJECT

public:
    Page(const QString &title, AdminSession *session, bool scrolls, QWidget *parent = nullptr);

    QString title() const { return m_title; }

    /*! Shown again: a page reloads what may have changed while it was hidden. */
    virtual void activate() {}

    /*! What the page scrolls, for --grab; null for a page that does not. */
    QWidget *scrolledContent() const { return m_scrolled; }

protected:
    AdminSession *session() const { return m_session; }
    QVBoxLayout *body() const { return m_body; }
    void setSubtitle(const QString &text);

    /*! A row of buttons under a card's text, aligned to the left. */
    static QWidget *buttonRow(const QList<QWidget *> &buttons, QWidget *parent = nullptr);

private:
    QString m_title;
    AdminSession *m_session;
    QLabel *m_subtitle = nullptr;
    QVBoxLayout *m_body = nullptr;
    QWidget *m_scrolled = nullptr;
};
