#ifndef TORMGR_H
#define TORMGR_H

#include <QObject>

#include "torconfig.h"

namespace ds {
namespace tor {

/*! Manages the access to a Tor server
 *
 */
class TorMgr : public QObject
{
    Q_OBJECT

public:

    explicit TorMgr(const TorConfig& config, QObject *parent = nullptr);

signals:

public slots:
    void start();

private:
    void startUseSystemInstance();

    TorConfig config_;
};

}} // namespaces

#endif // TORMGR_H
