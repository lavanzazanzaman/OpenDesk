#pragma once
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
namespace od {
inline QString resource_dir(const char * kind, const char * development) {
    const QString override = qEnvironmentVariable((QByteArray("OPENDESK_") + QByteArray(kind).toUpper() + "_DIR").constData());
    if (!override.isEmpty()) return override;
#ifdef _WIN32
    // installed: <install dir>\runtime, <install dir>\xrizer, ... next to OpenDesk.exe
    const QString packaged = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QString::fromUtf8(kind));
#else
    const QString packaged = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QString("../lib/opendesk/%1").arg(kind));
#endif
    if (QFileInfo::exists(packaged)) return QDir::cleanPath(packaged);
    return QString::fromUtf8(development);
}
}
