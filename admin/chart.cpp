#include "chart.h"

#include "look.h"

#include <QFontMetricsF>
#include <QKeyEvent>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace {

const double kLegendHeight = 24;
const double kTickGap = 6;
const double kBarMax = 24;
const double kBarGap = 2;
const double kDotRadius = 4;

/*!
 * The step between gridlines: 1, 2 or 5 times a power of ten, whole numbers
 * only unless @p smallest allows less.
 */
double niceStep(double top, int lines, double smallest)
{
    const double raw = qMax(smallest, top / lines);
    const double power = std::pow(10.0, std::floor(std::log10(raw)));
    for (const double factor : { 1.0, 2.0, 5.0, 10.0 }) {
        if (factor * power >= raw)
            return qMax(smallest, factor * power);
    }
    return 10 * power;
}

} // namespace

Chart::Chart(Form form, QWidget *parent)
    : QWidget(parent)
    , m_form(form)
    , m_emptyText(QStringLiteral("За этот период данных нет"))
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
}

QColor Chart::seriesColor(int slot)
{
    static const QColor colors[] = { QColor(0x2a, 0x78, 0xd6), QColor(0xeb, 0x68, 0x34),
                                     QColor(0x1b, 0xaf, 0x7a) };
    return colors[qBound(0, slot, 2)];
}

void Chart::setData(const QStringList &ticks, const QStringList &titles,
                    const QList<Series> &series)
{
    m_ticks = ticks;
    m_titles = titles;
    m_series = series;
    m_hover = -1;
    update();
}

void Chart::setEmptyText(const QString &text)
{
    m_emptyText = text;
    update();
}

void Chart::setValueFormat(int decimals, const QString &suffix)
{
    m_decimals = qBound(0, decimals, 3);
    m_suffix = suffix;
    update();
}

QString Chart::axisText(double value, double step) const
{
    // The gridlines step in whole numbers or in fractions: their labels all
    // carry the same number of decimals, "50" next to "100", not "50,0".
    if (m_decimals == 0 || step >= 1)
        return Look::number(qint64(std::llround(value))) + m_suffix;
    const int decimals = qMin(m_decimals, int(std::ceil(-std::log10(step) - 1e-9)));
    return QLocale(QLocale::Russian).toString(value, 'f', decimals) + m_suffix;
}

QString Chart::valueText(double value) const
{
    if (std::isnan(value))
        return QStringLiteral("—");
    if (m_decimals == 0)
        return Look::number(qint64(std::llround(value))) + m_suffix;
    // Small values keep their fraction, big ones do not need it.
    const int decimals = std::abs(value) >= 100 ? 0 : m_decimals;
    return QLocale(QLocale::Russian).toString(value, 'f', decimals) + m_suffix;
}

QSize Chart::sizeHint() const
{
    return QSize(640, 240);
}

QSize Chart::minimumSizeHint() const
{
    return QSize(320, 180);
}

bool Chart::isEmpty() const
{
    for (const Series &series : m_series) {
        for (const double value : series.values) {
            if (value > 0)   // a gap is NaN, and NaN is not above zero
                return false;
        }
    }
    return true;
}

Chart::Frame Chart::frame() const
{
    Frame result;
    double top = 0;
    for (const Series &series : m_series) {
        for (const double value : series.values) {
            if (!std::isnan(value))
                top = qMax(top, value);
        }
    }
    const double smallest = m_decimals > 0 ? std::pow(10.0, -m_decimals) : 1.0;
    const double floor = m_decimals > 0 ? 4 * smallest : 4.0;
    const double step = niceStep(qMax(top, floor), 4, smallest);
    result.top = std::ceil(qMax(top, smallest) / step - 1e-9) * step;
    for (double value = 0; value <= result.top + step / 2; value += step)
        result.gridValues << value;

    const QFontMetricsF metrics(font());
    double labelWidth = 0;
    for (const double value : result.gridValues)
        labelWidth = qMax(labelWidth, metrics.horizontalAdvance(axisText(value, step)));

    const double legend = m_series.size() > 1 ? kLegendHeight : 4;
    // The lines carry their last value at the right end.
    double endLabels = 8;
    if (m_form == Form::Lines) {
        for (const Series &series : m_series) {
            for (int i = int(series.values.size()) - 1; i >= 0; --i) {
                if (std::isnan(series.values.at(i)))
                    continue;
                endLabels = qMax(endLabels, kDotRadius * 2 + 6
                                                + metrics.horizontalAdvance(valueText(series.values.at(i))));
                break;
            }
        }
    }
    result.plot = QRectF(labelWidth + 10, legend + metrics.height() / 2,
                         qMax(10.0, width() - labelWidth - 10 - endLabels),
                         qMax(10.0, height() - legend - metrics.height() / 2 - metrics.height() - kTickGap - 2));
    return result;
}

double Chart::xOf(const Frame &frame, int index) const
{
    const int count = int(m_ticks.size());
    if (m_form == Form::Columns) {
        const double slot = frame.plot.width() / qMax(1, count);
        return frame.plot.left() + slot * (index + 0.5);
    }
    if (count <= 1)
        return frame.plot.center().x();
    return frame.plot.left() + frame.plot.width() * index / (count - 1);
}

double Chart::yOf(const Frame &frame, double value) const
{
    return frame.plot.bottom() - frame.plot.height() * value / frame.top;
}

int Chart::indexAt(const Frame &frame, double x) const
{
    const int count = int(m_ticks.size());
    if (count == 0)
        return -1;
    int best = 0;
    double distance = std::abs(xOf(frame, 0) - x);
    for (int i = 1; i < count; ++i) {
        const double d = std::abs(xOf(frame, i) - x);
        if (d < distance) {
            distance = d;
            best = i;
        }
    }
    return best;
}

void Chart::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), Look::surface());

    const QFontMetricsF metrics(font());
    if (m_ticks.isEmpty() || isEmpty()) {
        painter.setPen(Look::muted());
        painter.drawText(rect(), Qt::AlignCenter, m_emptyText);
        return;
    }

    const Frame f = frame();

    // Legend: a line key or a swatch, the way the marks look, in text ink.
    if (m_series.size() > 1) {
        double x = f.plot.left();
        const double y = kLegendHeight / 2;
        for (const Series &series : m_series) {
            if (m_form == Form::Lines) {
                painter.setPen(QPen(series.color, 2, Qt::SolidLine, Qt::RoundCap));
                painter.drawLine(QPointF(x, y), QPointF(x + 14, y));
            } else {
                painter.fillRect(QRectF(x, y - 5, 10, 10), series.color);
            }
            painter.setPen(Look::secondary());
            const double textX = x + 20;
            painter.drawText(QPointF(textX, y + metrics.ascent() / 2 - 1), series.name);
            x = textX + metrics.horizontalAdvance(series.name) + 18;
        }
    }

    // Gridlines, hairline and solid; the zero line is the baseline.
    for (const double value : f.gridValues) {
        const double y = std::round(yOf(f, value)) + 0.5;
        painter.setPen(QPen(value == 0 ? Look::baseline() : Look::grid(), 1));
        painter.drawLine(QPointF(f.plot.left(), y), QPointF(f.plot.right(), y));
        painter.setPen(Look::muted());
        const double step = f.gridValues.size() > 1 ? f.gridValues.at(1) - f.gridValues.at(0) : 1;
        const QString text = axisText(value, step);
        painter.drawText(QRectF(0, y - metrics.height() / 2, f.plot.left() - 8, metrics.height()),
                         Qt::AlignRight | Qt::AlignVCenter, text);
    }

    // Labels of the x axis, wherever the caller put one and there is room.
    painter.setPen(Look::muted());
    double lastRight = -1e9;
    for (int i = int(m_ticks.size()) - 1; i >= 0; --i) {
        if (m_ticks.at(i).isEmpty())
            continue;
        const double width = metrics.horizontalAdvance(m_ticks.at(i));
        double x = xOf(f, i) - width / 2;
        x = qBound(0.0, x, double(this->width()) - width);
        // Going from the right end leftwards: a label that would touch the
        // one already drawn is skipped.
        if (lastRight > -1e9 && x + width + 8 > lastRight)
            continue;
        painter.drawText(QPointF(x, f.plot.bottom() + kTickGap + metrics.ascent()), m_ticks.at(i));
        lastRight = x;
    }

    if (m_form == Form::Columns) {
        const int count = int(m_ticks.size());
        const double slot = f.plot.width() / qMax(1, count);
        const double barWidth = slot >= 3 ? qMin(kBarMax, slot - kBarGap) : qMax(1.0, slot);
        const Series &series = m_series.first();
        for (int i = 0; i < count && i < series.values.size(); ++i) {
            const double value = series.values.at(i);
            if (std::isnan(value) || value <= 0)
                continue;
            const double x = xOf(f, i) - barWidth / 2;
            const double y = yOf(f, value);
            const QRectF bar(x, y, barWidth, f.plot.bottom() - y);
            QColor color = series.color;
            if (i == m_hover)
                color = color.lighter(118);
            // Rounded at the data end, square at the baseline.
            const double radius = qMin(4.0, qMin(barWidth / 2, bar.height()));
            QPainterPath path;
            path.moveTo(bar.bottomLeft());
            path.lineTo(bar.left(), bar.top() + radius);
            path.quadTo(bar.topLeft(), QPointF(bar.left() + radius, bar.top()));
            path.lineTo(bar.right() - radius, bar.top());
            path.quadTo(bar.topRight(), QPointF(bar.right(), bar.top() + radius));
            path.lineTo(bar.bottomRight());
            path.closeSubpath();
            painter.fillPath(path, color);
        }
    } else {
        // The crosshair first, under the lines.
        if (m_hover >= 0) {
            const double x = std::round(xOf(f, m_hover)) + 0.5;
            painter.setPen(QPen(Look::baseline(), 1));
            painter.drawLine(QPointF(x, f.plot.top()), QPointF(x, f.plot.bottom()));
        }
        QList<QPair<double, QString>> endLabels;
        for (const Series &series : m_series) {
            if (series.values.isEmpty())
                continue;
            // A gap breaks the line; a point alone between two gaps gets a dot.
            QPainterPath path;
            QList<QPointF> lonely;
            bool drawing = false;
            for (int i = 0; i < series.values.size(); ++i) {
                const double value = series.values.at(i);
                if (std::isnan(value)) {
                    drawing = false;
                    continue;
                }
                const QPointF point(xOf(f, i), yOf(f, value));
                if (drawing) {
                    path.lineTo(point);
                } else {
                    path.moveTo(point);
                    const bool nextGap = i + 1 >= series.values.size()
                                         || std::isnan(series.values.at(i + 1));
                    if (nextGap)
                        lonely << point;
                }
                drawing = true;
            }
            painter.setPen(QPen(series.color, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(path);
            painter.setPen(Qt::NoPen);
            painter.setBrush(series.color);
            for (const QPointF &point : std::as_const(lonely))
                painter.drawEllipse(point, 1.5, 1.5);

            int last = int(series.values.size()) - 1;
            while (last >= 0 && std::isnan(series.values.at(last)))
                --last;
            if (last < 0)
                continue;
            const QPointF end(xOf(f, last), yOf(f, series.values.at(last)));
            painter.setPen(QPen(Look::surface(), 2));
            painter.setBrush(series.color);
            painter.drawEllipse(end, kDotRadius + 1, kDotRadius + 1);
            endLabels.append(qMakePair(end.y(), valueText(series.values.at(last))));
            if (m_hover >= 0 && m_hover < series.values.size()
                && !std::isnan(series.values.at(m_hover))) {
                const QPointF at(xOf(f, m_hover), yOf(f, series.values.at(m_hover)));
                painter.drawEllipse(at, kDotRadius + 1, kDotRadius + 1);
            }
        }
        // The last values beside the end dots - only those that do not run
        // into each other: converging labels are left to the legend and the
        // tooltip rather than pushed apart from their lines.
        painter.setPen(Look::secondary());
        QList<double> placed;
        for (const auto &label : endLabels) {
            const bool clash = std::any_of(placed.cbegin(), placed.cend(), [&](double y) {
                return std::abs(y - label.first) < metrics.height();
            });
            if (clash)
                continue;
            placed << label.first;
            painter.drawText(QPointF(f.plot.right() + kDotRadius * 2 + 4,
                                     label.first + metrics.ascent() / 2 - 1),
                             label.second);
        }
    }

    if (m_hover >= 0)
        paintTooltip(painter, f, m_hover);
}

void Chart::paintTooltip(QPainter &painter, const Frame &frame, int index)
{
    QFont bold = font();
    bold.setWeight(QFont::DemiBold);
    const QFontMetricsF metrics(font());
    const QFontMetricsF boldMetrics(bold);

    const QString title = index < m_titles.size() ? m_titles.at(index) : m_ticks.value(index);
    double valueWidth = 0;
    double nameWidth = 0;
    for (const Series &series : m_series) {
        valueWidth = qMax(valueWidth, boldMetrics.horizontalAdvance(valueText(series.values.value(index))));
        nameWidth = qMax(nameWidth, metrics.horizontalAdvance(series.name));
    }
    const double line = metrics.height() + 4;
    const double width = qMax(metrics.horizontalAdvance(title), 18 + valueWidth + 8 + nameWidth) + 20;
    const double height = line * (m_series.size() + 1) + 10;

    const double x0 = xOf(frame, index);
    double x = x0 + 12;
    if (x + width > this->width() - 2)
        x = x0 - 12 - width;
    const QRectF box(qMax(2.0, x), frame.plot.top(), width, height);

    painter.setPen(QPen(Look::grid(), 1));
    painter.setBrush(Look::surface());
    painter.drawRoundedRect(box, 6, 6);

    double y = box.top() + 6 + metrics.ascent();
    painter.setPen(Look::secondary());
    painter.drawText(QPointF(box.left() + 10, y), title);
    for (const Series &series : m_series) {
        y += line;
        const double keyY = y - metrics.ascent() / 2 + 1;
        if (m_form == Form::Lines) {
            painter.setPen(QPen(series.color, 2, Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(QPointF(box.left() + 10, keyY), QPointF(box.left() + 22, keyY));
        } else {
            painter.fillRect(QRectF(box.left() + 10, keyY - 4, 8, 8), series.color);
        }
        // The value leads, the name follows.
        painter.setFont(bold);
        painter.setPen(Look::ink());
        const QString value = valueText(series.values.value(index));
        painter.drawText(QPointF(box.left() + 28, y), value);
        painter.setFont(font());
        painter.setPen(Look::secondary());
        painter.drawText(QPointF(box.left() + 28 + valueWidth + 8, y), series.name);
    }
}

void Chart::mouseMoveEvent(QMouseEvent *event)
{
    if (m_ticks.isEmpty() || isEmpty())
        return;
    const Frame f = frame();
    const int index = f.plot.adjusted(-12, -12, 12, 12).contains(event->position())
                          ? indexAt(f, event->position().x())
                          : -1;
    if (index != m_hover) {
        m_hover = index;
        update();
    }
}

void Chart::leaveEvent(QEvent *)
{
    if (m_hover >= 0 && !hasFocus()) {
        m_hover = -1;
        update();
    }
}

void Chart::keyPressEvent(QKeyEvent *event)
{
    const int count = int(m_ticks.size());
    if (count == 0 || (event->key() != Qt::Key_Left && event->key() != Qt::Key_Right)) {
        QWidget::keyPressEvent(event);
        return;
    }
    if (m_hover < 0)
        m_hover = count - 1;
    else
        m_hover = qBound(0, m_hover + (event->key() == Qt::Key_Left ? -1 : 1), count - 1);
    update();
}

void Chart::focusOutEvent(QFocusEvent *event)
{
    m_hover = -1;
    update();
    QWidget::focusOutEvent(event);
}
