#include <iostream>
#include <assert.h>
#include <array>
#include <QTextStream>
#include <QDateTime>
#include <QDebug>

#include "ds/logging.h"

namespace ds {
namespace core {

Logging::Logging(QSettings &settings)
    : settings_{settings}
{
    assert(!instance_);
    instance_ = this;
}

Logging::~Logging()
{
    assert(instance_ == this);
    instance_ = {};
}

void Logging::logMessageHandler(QtMsgType type,
                                const QMessageLogContext &context,
                                const QString &msg)
{
    if (instance_) {
        instance_->onLogMessageHandler(type, context, msg);
    }
}

void Logging::onLogMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    std::array<QString, 6> message_types = {{
        "debug", "warn", "error", "fatal", "info", "system"
    }};

    const auto now = QDateTime::currentDateTime().toString("yyyy-HH-mm HH:mm.zzz");

    if (settings_.value("log-enabled", false).toBool()) {

        if (!logFile_) {
            open();
        }

        QTextStream stream(logFile_.get());

        stream
            << now
            << ' '
            << message_types.at(type)
            << ' '
            << context.function
            << ' '
            << msg
            << '\n';
    }
#ifdef QT_DEBUG
    std::clog << now.toStdString()
         << ' '
         << message_types.at(type).toStdString()
         << ' '
         << context.function
         << ' '
         << msg.toStdString()
         << std::endl;
#endif
}

void Logging::changed()
{
    if (settings_.value("log-enabled", false).toBool()) {
        open();
    } else if (logFile_){
        qDebug() << "Closing the log-file";
        logFile_->reset();
    }
}

void Logging::open()
{
    const auto path = settings_.value("log-path", "whid.log").toString();
    QIODevice::OpenMode options = QIODevice::WriteOnly;
    QString mode = "truncate";
    if (settings_.value("log-append", false).toBool()) {
        options |= QIODevice::Append;
        mode = "append";
    } else {
        options |= QIODevice::Truncate;
    }
    qDebug() << "Opening log-file: " << path << " [" << mode << ']';
    logFile_ = std::make_unique<QFile>(path);
    logFile_->open(options);
}

}} // namepaces

