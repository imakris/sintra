// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

// This file defines crippled spinlock-wrapped versions of common containers.
// The only guarantee that these containers provide is that individual
// operations on them are thread safe.
// By no means should anyone ever assume that this offers any kind of generic
// thread safety associated with their usage. Their sole purpose was to save
// some typing, in a few common scenarios within sintra, but also to simplify
// debugging, by eliminating a class of bugs associated with container
// corruption.


#include "../ipc/spinlock.h"
#include <deque>
#include <list>
#include <map>
#include <set>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>


namespace sintra {

using std::deque;
using std::list;
using std::map;
using std::set;
using std::unordered_map;
using std::unordered_set;
using std::vector;


namespace detail {


template <template <typename...> typename CT, typename... Args>
struct spinlocked
{
    using iterator          = typename CT<Args...>::iterator;
    using const_iterator    = typename CT<Args...>::const_iterator;
    using reference         = typename CT<Args...>::reference;
    using const_reference   = typename CT<Args...>::const_reference;
    using size_type         = typename CT<Args...>::size_type;
    using locker            = spinlock::locker;

    struct scoped_access
    {
        scoped_access(spinlock& sl, CT<Args...>& c)
            : m_lock(sl)
            , m_c(c)
        {}

        scoped_access(const scoped_access&) = delete;
        scoped_access& operator=(const scoped_access&) = delete;

        scoped_access(scoped_access&&) = delete;
        scoped_access& operator=(scoped_access&&) = delete;

        iterator begin() noexcept { return m_c.begin(); }
        iterator end() noexcept { return m_c.end(); }

        auto erase(iterator it) { return m_c.erase(it); }

        CT<Args...>& get() noexcept { return m_c; }

    private:
        locker         m_lock;
        CT<Args...>&   m_c;
    };

    struct const_scoped_access
    {
        const_scoped_access(spinlock& sl, const CT<Args...>& c)
            : m_lock(sl)
            , m_c(c)
        {}

        const_scoped_access(const const_scoped_access&) = delete;
        const_scoped_access& operator=(const const_scoped_access&) = delete;

        const_scoped_access(const_scoped_access&&) = delete;
        const_scoped_access& operator=(const_scoped_access&&) = delete;

        const_iterator begin() const noexcept { return m_c.begin(); }
        const_iterator end() const noexcept { return m_c.end(); }

        const CT<Args...>& get() const noexcept { return m_c; }

    private:
        locker               m_lock;
        const CT<Args...>&   m_c;
    };

    template <typename Fn>
    decltype(auto) with_lock(Fn&& fn)
    {
        locker l(m_sl);
        return fn(m_c);
    }

    void clear() noexcept                          {locker l(m_sl); m_c.clear();                  }

    bool empty() const noexcept                    {locker l(m_sl); return m_c.empty();           }

    auto pop_front()                               {locker l(m_sl); m_c.pop_front();              }

    template <typename... FArgs>
    auto push_back(FArgs&&... v)                   {locker l(m_sl); m_c.push_back(std::forward<FArgs>(v)...);   }

    auto size()  const noexcept                    {locker l(m_sl); return m_c.size();            }

    operator CT<Args...>() const                   {locker l(m_sl); return m_c;                   }
    spinlocked& operator=(const CT<Args...>& x)    {locker l(m_sl); m_c = x; return *this; }
    spinlocked& operator=(CT<Args...>&& x)         {locker l(m_sl); m_c = std::move(x); return *this; }

    scoped_access scoped() noexcept                {return scoped_access(m_sl, m_c);              }
    const_scoped_access scoped() const noexcept    {return const_scoped_access(m_sl, m_c);        }

protected:
    CT<Args...>         m_c;
    mutable spinlock    m_sl;
};



} // namespace detail



template <typename T>
using spinlocked_deque = detail::spinlocked<deque, T>;

template <typename T>
using spinlocked_list = detail::spinlocked<list, T>;

template <typename T>
using spinlocked_set = detail::spinlocked<set, T>;

template <typename T>
using spinlocked_uset = detail::spinlocked<unordered_set, T>;


template <typename Key, typename T>
struct spinlocked_map: detail::spinlocked<map, Key, T>
{
};


template <typename Key, typename T>
struct spinlocked_umap: detail::spinlocked<unordered_map, Key, T>
{
    using locker = spinlock::locker;

    template <typename U, typename = void>
    struct has_value_type : std::false_type
    {};

    template <typename U>
    struct has_value_type<U, std::void_t<typename U::value_type>> : std::true_type
    {};

    bool contains_key(const Key& key) const
    {
        locker l(this->m_sl);
        return this->m_c.find(key) != this->m_c.end();
    }

    template <typename U = T, typename = std::enable_if_t<has_value_type<U>::value>>
    bool copy_value(const Key& key, std::vector<typename U::value_type>& out) const
    {
        locker l(this->m_sl);
        auto it = this->m_c.find(key);
        if (it == this->m_c.end()) {
            return false;
        }
        out.assign(it->second.begin(), it->second.end());
        return !out.empty();
    }

    template <typename K, typename V>
    void set_value(K&& key, V&& value)
    {
        locker l(this->m_sl);
        this->m_c[std::forward<K>(key)] = std::forward<V>(value);
    }
};


template <typename T>
using spinlocked_vector = detail::spinlocked<vector, T>;

} // namespace sintra


