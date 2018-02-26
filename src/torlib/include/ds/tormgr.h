#ifndef TORMGR_H
#define TORMGR_H

#include <memory>

#include <QObject>

#include "ds/torconfig.h"
#include "torcontroller.h"

namespace ds {
namespace tor {

/*! Manages the access to a Tor server
 *
 */
class TorMgr : public QObject
{
    Q_OBJECT

public:
    explicit TorMgr(const TorConfig& config);

    TorController *getController() { return ctl_.get(); }

signals:

public slots:
    /*! Start / connect to the Tor service */
    void start();

private:
    void startUseSystemInstance();

    TorConfig config_;
    std::shared_ptr<TorController> ctl_;
};

}} // namespaces

#endif // TORMGR_H
