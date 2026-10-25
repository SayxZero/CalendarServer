#pragma once

#include <QColor>
#include <QList>
#include <QString>
#include <QStringList>
#include <QWidget>

/*!
 * The charts of the statistics page: lines over days (active accounts) or
 * columns (exchanges a day), drawn by hand - no Qt Charts, which is GPL and
 * absent from the Qt of many distributions.
 *
 * The rules are those of a quiet chart: one y-axis, hairline grid, 2 px lines
 * with a ringed end dot and the last value beside it, columns of at most 24 px
 * with a rounded data end and a gap between them, a legend for more than one
 * series. Hovering (or the arrow keys, once the chart has focus) shows every
 * series at that point; the same numbers are in the table next to the chart.
 */
class Chart : public QWidget
{
    Q_OBJECT

public:
    enum class Form { Lines, Columns };

    struct Series
    {
        QString name;
        QColor color;
        QList<double> values;
    };

    explicit Chart(Form form, QWidget *parent = nullptr);

    /*!
     * The color of categorical slot @p slot (0, 1, 2): blue, orange, aqua. The
     * order is the validated one; a chart needs no more than three here.
     */
    static QColor seriesColor(int slot);

    /*!
     * @p ticks label the x positions under the axis (empty strings are not
     * shown), @p titles head the tooltip of each position.
     */
    void setData(const QStringList &ticks, const QStringList &titles, const QList<Series> &series);

    /*! The text in place of the chart when every value is zero or there are none. */
    void setEmptyText(const QString &text);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

private:
    struct Frame
    {
        QRectF plot;
        double top = 1;          //!< the value of the highest gridline
        QList<double> gridValues;
    };

    Frame frame() const;
    double xOf(const Frame &frame, int index) const;
    double yOf(const Frame &frame, double value) const;
    int indexAt(const Frame &frame, double x) const;
    bool isEmpty() const;
    void paintTooltip(QPainter &painter, const Frame &frame, int index);

    Form m_form;
    QStringList m_ticks;
    QStringList m_titles;
    QList<Series> m_series;
    QString m_emptyText;
    int m_hover = -1;
};
