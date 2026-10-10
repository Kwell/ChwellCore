#pragma once

#include <typeindex>
#include <unordered_map>

namespace chwell { namespace service {

// Explicit exact-type bindings; the component/provider owns the pointed-to object.
// Registration, lookup and owner removal must run on the application owner thread.
class InterfaceRegistry {
public:
    template <typename Interface>
    bool add(Interface* implementation, const void* owner) {
        if (!implementation || !owner) return false;
        return entries_.emplace(std::type_index(typeid(Interface)), Entry{implementation, owner}).second;
    }
    template <typename Interface>
    Interface* get() const {
        auto it = entries_.find(std::type_index(typeid(Interface)));
        return it == entries_.end() ? nullptr : static_cast<Interface*>(it->second.implementation);
    }
    void remove_owner(const void* owner) {
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->second.owner == owner) it = entries_.erase(it);
            else ++it;
        }
    }
private:
    struct Entry { void* implementation; const void* owner; };
    std::unordered_map<std::type_index, Entry> entries_;
};

}} // namespace chwell::service
