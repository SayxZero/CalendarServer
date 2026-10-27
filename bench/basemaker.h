#pragma once

#include <QString>

#include <atomic>
#include <functional>

/*!
 * A base of made-up accounts for the benchmark: @p accounts accounts with
 * @p records records each (events, tasks, completions of the size of real
 * ones, three calendars), every tenth account sharing its first calendar
 * with the next three, one real password hash for all of them, and the
 * plain session tokens in tokens.json beside the base. Nothing in it comes
 * from a real base.
 *
 * A folder that holds a base made with the same numbers is kept as it is
 * (base.json says how it was made): on a hard disk making it takes a minute.
 */
struct BaseRequest
{
    QString dir;
    int accounts = 200;
    int records = 500;
};

using BaseProgress = std::function<void(double fraction, const QString &message)>;

bool baseIsReady(const BaseRequest &request);

bool prepareBase(const BaseRequest &request, const std::atomic<bool> *cancel,
                 const BaseProgress &progress, QString *error);
