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
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>


namespace sintra {


using std::deque;
using std::list;
using std::map;
using std::memory_order_acquire;
using std::memory_order_release;
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

    reference back() noexcept                      {locker l(m_sl); return m_c.back();            }
    const_reference back() const noexcept          {locker l(m_sl); return m_c.back();            }
    iterator begin() noexcept                      {locker l(m_sl); return m_c.begin();           }
    const_iterator begin() const noexcept          {locker l(m_sl); return m_c.begin();           }
    void clear() noexcept                          {locker l(m_sl); return m_c.clear();           }


    template <class... FArgs>
    auto emplace(FArgs&&... args)
    {
        locker l(this->m_sl);
        return this->m_c.emplace(std::forward<FArgs>(args)...);
    }

    template <class... FArgs>
    auto emplace_hint(const_iterator hint, FArgs&&... args)
    {
        locker l(this->m_sl);
        return this->m_c.emplace_hint(hint, std::forward<FArgs>(args)...);
    }

    bool empty() const noexcept                    {locker l(m_sl); return m_c.empty();           }
    iterator end() noexcept                        {locker l(m_sl); return m_c.end();             }
    const_iterator end() const noexcept            {locker l(m_sl); return m_c.end();             }

    template <typename... FArgs>
    auto erase(FArgs&&... v)                       {locker l(m_sl); return m_c.erase(std::forward<FArgs>(v)...);       }

    template <typename... FArgs>
    auto find(FArgs&&... v)                        {locker l(m_sl); return m_c.find(std::forward<FArgs>(v)...);        }
    template <typename... FArgs>
    auto find(FArgs&&... v) const                  {locker l(m_sl); return m_c.find(std::forward<FArgs>(v)...);        }

    reference front() noexcept                     {locker l(m_sl); return m_c.front();           }
    const_reference front() const noexcept         {locker l(m_sl); return m_c.front();           }

    template <typename... FArgs>
    auto insert(FArgs&&... v)                      {locker l(m_sl); return m_c.insert(std::forward<FArgs>(v)...);      }

    auto pop_front()                               {locker l(m_sl); return m_c.pop_front();       }

    template <typename... FArgs>
    auto push_back(FArgs&&... v)                   {locker l(m_sl); return m_c.push_back(std::forward<FArgs>(v)...);   }

    auto size()  const noexcept                    {locker l(m_sl); return m_c.size();            }

    operator CT<Args...>() const                   {locker l(m_sl); return m_c;                   }
    auto operator=(const CT<Args...>& x)           {locker l(m_sl); return m_c.operator=(x);      }
    auto operator=(CT<Args...>&& x)                {locker l(m_sl); return m_c.operator=(std::move(x));      }


    //auto operator=(const CT<Args...>& x)           {locker l(m_sl); return m_c.operator=(x.m_c);  }
    //auto operator=(CT<Args...>&& x)                {locker l(m_sl); return m_c.operator=(x.m_c);  }

    reference operator[] (size_type p)             {locker l(m_sl); return m_c[p];                }
    const_reference operator[] (size_type p) const {locker l(m_sl); return m_c[p];                }

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
    using locker = spinlock::locker;

    template <typename... FArgs>
    auto lower_bound(FArgs&&... v) {
        locker l(this->m_sl);
        return this->m_c.lower_bound(std::forward<FArgs>(v)...);
    }

    T& operator[] (const Key& k)                   {locker l(this->m_sl); return this->m_c[k];    }
    T& operator[] (Key&& k)                        {locker l(this->m_sl); return this->m_c[std::move(k)];    }
};


template <typename Key, typename T>
struct spinlocked_umap: detail::spinlocked<unordered_map, Key, T>
{
    using locker = spinlock::locker;
    T& operator[] (const Key& k)                   {locker l(this->m_sl); return this->m_c[k];    }
    T& operator[] (Key&& k)                        {locker l(this->m_sl); return this->m_c[std::move(k)];    }
};


template <typename T>
using spinlocked_vector = detail::spinlocked<vector, T>;

} // namespace sintra


