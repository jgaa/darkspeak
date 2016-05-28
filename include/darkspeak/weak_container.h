
#pragma once

namespace darkspeak { namespace impl {

template<typename T, typename listT>
std::vector<std::shared_ptr<T>> GetValidObjects(listT& data)
{
    std::vector<std::shared_ptr<T>> list;

    for(auto it = data.begin(); it != data.end();) {
        auto ptr = it->lock();
        if (!ptr) {
            // dead object
            it = data.erase(it);
        } else {
            list.push_back(move(ptr));
            ++it;
        }
    }

    return list;
}

}} // namespaces
