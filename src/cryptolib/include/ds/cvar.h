#ifndef CVAR_H
#define CVAR_H

#include <stdexcept>

namespace ds {
namespace crypto {

template <typename T, typename F>
class Cvar
{
public:
    struct Error : public std::runtime_error
    {
        Error(const char *what) : std::runtime_error(what) {}
    };

    Cvar(T * v, F fn, const char *errmsg)
        : v_{v}, fn_{fn}
    {
        if (!v) {
            throw Error(errmsg);
        }
    }

    ~Cvar() {
        if (v_) {
            fn_(v_);
        }
    }

    operator T * () { return v_; }
    T * operator -> () { return v_; }


    T * take() {
        auto v = v_;
        v_ = {};
        return v;
    }

private:
    T * v_;
    F fn_;
};

/*! Hold a C pointer until the instance goes out of scope,
 * then call the destructor functor.
 *
 * Convenience function when dealing with resources in the openssl library
 *
 * \exception Error if the pointer is nullptr
 */
template <typename T, typename F>
Cvar<T, F> make_cvar(T * v, F fn, const char *errmsg) {
    return Cvar<T, F>(v, fn, errmsg);
}

}} // namepaces

#endif // CVAR_H
