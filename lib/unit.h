#ifndef _CUTES_UNIT_HPP_
#define _CUTES_UNIT_HPP_
/**
 * @file unit.hpp
 * @brief Vault unit option parsing and file export/import support
 * @author Denis Zalevskiy <denis.zalevskiy@jolla.com>
 * @copyright (C) 2014 Jolla Ltd.
 * @par License: LGPL 2.1 http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html
 */

#include <QString>
#include <QVariantMap>

namespace vault { namespace unit {

int execute(QVariantMap const &info);

int runProcess(const QString &program, const QStringList &args);

QString optValue(const QString &arg);
}}

#endif // _CUTES_UNIT_HPP_
