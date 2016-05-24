
#pragma once

namespace darkspeak { namespace impl {

template<typename T, typename listT>
std::vector<std::shared_ptr<T>> GetValidObjects(listT& data)
{
    std::vector<std::shared_ptr<T>> list;

    auto prev = data.end();
    for(auto it = data.begin(); it != data.end();) {
        auto ptr = it->lock();
        if (!ptr) {
            // dead object
            data.erase(it);
            it = prev;
            if (it == data.end()) {
                it = data.begin();
            } else {
                ++it;
            }
        } else {
            list.push_back(move(ptr));
            prev = it;
            ++it;
        }
    }

    return list;
}

}} // namespaces
