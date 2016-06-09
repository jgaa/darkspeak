#pragma once

#ifndef RESTC_CPP_IO_TIMER_H_
#define RESTC_CPP_IO_TIMER_H_

#include <boost/asio.hpp>
#include <functional>
#include <memory>

#include "log/WarLog.h"

namespace darkspeak {
namespace impl {

class IoTimer : public std::enable_shared_from_this<IoTimer>
{
public:
    using ptr_t = std::shared_ptr<IoTimer>;
    using close_t = std::function<void ()>;

    ~IoTimer() {
        if (is_active_) {
            is_active_ = false;
        }
    }

    void Handler(const boost::system::error_code& error) {
        if (is_active_) {
            is_active_ = false;
            is_expiered_ = true;
            close_();
        }
    }

    void Cancel() {
        is_active_ = false;
    }

    bool IsExcpiered() const noexcept { return is_expiered_; }

    static ptr_t Create(size_t milliseconds_timeout,
        boost::asio::io_service& io_service,
        close_t close) {

        ptr_t timer;
        timer.reset(new IoTimer(io_service, close));
        timer->Start(milliseconds_timeout);
        return timer;
    }

    /*! Factory for Connection timers
     *
     * The wrapper must have a GetSocket() method that
     * returns a asio socket.
     */
    static ptr_t Create(size_t milliseconds_timeout,
            const std::shared_ptr<Connection>& ptr) {

        WAR_ASSERT(ptr);
        std::weak_ptr<Connection> weak_ptr = ptr;

        return Create(
            milliseconds_timeout,
            ptr->GetSocket().GetSocket().get_io_service(),
            [weak_ptr]() {
                if (auto ptr = weak_ptr.lock()) {
                    if (ptr->GetSocket().GetSocket().is_open()) {
                        LOG_WARN << "Socket "
                            << ptr->GetSocket().GetSocket()
                            << " timed out.";
                        ptr->GetSocket().GetSocket().close();
                    }
                }
            });
    }


private:
    IoTimer(boost::asio::io_service& io_service,
            close_t close)
    : close_{close}, timer_{io_service}
    {}

    void Start(size_t millisecondsTimeOut)
    {
        timer_.expires_from_now(
            boost::posix_time::milliseconds(millisecondsTimeOut));
        is_active_ = true;
        try {
            timer_.async_wait(std::bind(
                &IoTimer::Handler,
                shared_from_this(),
                std::placeholders::_1));
        } catch (const std::exception&) {
            is_active_ = false;
        }
    }

private:
    bool is_active_ = false;
    bool is_expiered_ = false;
    close_t close_;
    boost::asio::deadline_timer timer_;
};

} // impl
} // darkspeak

#endif // RESTC_CPP_IO_TIMER_H_

