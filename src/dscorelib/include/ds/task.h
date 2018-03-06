#ifndef TASK_H
#define TASK_H

#include <functional>

#include <QRunnable>
#include <QDebug>
#include <QThreadPool>

namespace ds {
namespace core {

class Task : public ::QRunnable {
public:
    using fn_t = std::function<void ()>;

    Task(fn_t fn) : fn_{std::move(fn)} {}

    void run() override {
        try {
            fn_();
        } catch(const std::exception& ex) {
            qWarning() << "Caught exception from task: " << ex.what();
        }
    }

    static void schedule(fn_t fn) {
        auto task = new Task{std::move(fn)};
        QThreadPool::globalInstance()->start(task);
    }

private:
    fn_t fn_;
};

}} //namespaces

#endif // TASK_H
