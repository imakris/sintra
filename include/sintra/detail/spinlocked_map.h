/*
Copyright 2017 Ioannis Makris

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef __SINTRA_SPINLOCKED_MAP__
#define __SINTRA_SPINLOCKED_MAP__


#include <atomic>
#include <unordered_map>
#include <unordered_set>
#include <deque>


namespace sintra {


using std::atomic_flag;
using std::memory_order_acquire;
using std::memory_order_release;
using std::unordered_map;
using std::unordered_set;
using std::deque;


namespace detail {


struct spinlock
{
    struct locker
    {
        locker(spinlock& sl): m_sl(sl) { m_sl.lock();   }
        ~locker()                      { m_sl.unlock(); }
        spinlock& m_sl;
    };

    void lock()   { while (m_locked.test_and_set(memory_order_acquire)) {} }
    void unlock() { m_locked.clear(memory_order_release);                  }

private:
    atomic_flag m_locked = ATOMIC_FLAG_INIT;
};


template <typename Key, typename T>
struct spinlocked_map
{
    using iterator       = typename unordered_map<Key, T>::iterator;
    using const_iterator = typename unordered_map<Key, T>::const_iterator;

    T& operator[] (const Key& k)        {spinlock::locker l(m_sl); return m_map[k];                }
    T& operator[] (Key&& k)             {spinlock::locker l(m_sl); return m_map[k];                }
    auto find(const Key& k)             {spinlock::locker l(m_sl); return m_map.find(k);           }
    auto find(const Key& k) const       {spinlock::locker l(m_sl); return m_map.find(k);           }
    auto begin() noexcept               {spinlock::locker l(m_sl); return m_map.begin();           }
    auto begin() const noexcept         {spinlock::locker l(m_sl); return m_map.begin();           }
    auto end()   noexcept               {spinlock::locker l(m_sl); return m_map.end();             }
    auto end()   const noexcept         {spinlock::locker l(m_sl); return m_map.end();             }
    auto erase(const_iterator position) {spinlock::locker l(m_sl); return m_map.erase(position);   }
    auto erase(const Key& k)            {spinlock::locker l(m_sl); return m_map.erase(k);          }
    auto erase(const_iterator first, const_iterator last)
                                        {spinlock::locker l(m_sl); return m_map.erase(first, last);}
    auto clear() noexcept               {spinlock::locker l(m_sl); return m_map.clear();           }

    template <typename... Args>
    auto insert(const Args&... v)       {spinlock::locker l(m_sl); return m_map.insert(v...);      }
    template <typename... Args>
    auto insert(Args&&... v)            {spinlock::locker l(m_sl); return m_map.insert(v...);      }
    auto empty() const noexcept         {spinlock::locker l(m_sl); return m_map.empty();           }
    auto size()  const noexcept         {spinlock::locker l(m_sl); return m_map.size();            }
    auto operator= (const spinlocked_map& x)
                                        {spinlock::locker l(m_sl); return m_map.operator=(x.m_map);}
    auto operator= (spinlocked_map&& x) {spinlock::locker l(m_sl); return m_map.operator=(x.m_map);}

private:
    unordered_map<Key, T> m_map;
    mutable spinlock m_sl;
};



template <typename T>
struct spinlocked_set
{
    using iterator       = typename unordered_set<T>::iterator;
    using const_iterator = typename unordered_set<T>::const_iterator;

    auto find(const T& v)               {spinlock::locker l(m_sl); return m_set.find(v);           }
    auto find(const T& v) const         {spinlock::locker l(m_sl); return m_set.find(v);           }
    auto begin() noexcept               {spinlock::locker l(m_sl); return m_set.begin();           }
    auto begin() const noexcept         {spinlock::locker l(m_sl); return m_set.begin();           }
    auto end()   noexcept               {spinlock::locker l(m_sl); return m_set.end();             }
    auto end()   const noexcept         {spinlock::locker l(m_sl); return m_set.end();             }
    auto erase(const_iterator position) {spinlock::locker l(m_sl); return m_set.erase(position);   }
    auto erase(const T& v)              {spinlock::locker l(m_sl); return m_set.erase(v);          }
    auto erase(const_iterator first, const_iterator last)
                                        {spinlock::locker l(m_sl); return m_set.erase(first, last);}
    template <typename... Args>
    auto insert(const Args&... v)       {spinlock::locker l(m_sl); return m_set.insert(v...);      }
    template <typename... Args>
    auto insert(Args&&... v)            {spinlock::locker l(m_sl); return m_set.insert(v...);      }
    auto empty() const noexcept         {spinlock::locker l(m_sl); return m_set.empty();           }
    auto size()  const noexcept         {spinlock::locker l(m_sl); return m_set.size();            }
    auto operator= (const spinlocked_set& x)
                                        {spinlock::locker l(m_sl); return m_set.operator=(x.m_set);}
    auto operator= (spinlocked_set&& x) {spinlock::locker l(m_sl); return m_set.operator=(x.m_set);}

private:
    unordered_set<T> m_set;
    mutable spinlock m_sl;
};



template <typename T>
struct spinlocked_deque
{
    using iterator       = typename deque<T>::iterator;
    using const_iterator = typename deque<T>::const_iterator;

    auto begin() noexcept               {spinlock::locker l(m_sl); return m_deq.begin();           }
    auto begin() const noexcept         {spinlock::locker l(m_sl); return m_deq.begin();           }
    auto end()   noexcept               {spinlock::locker l(m_sl); return m_deq.end();             }
    auto end()   const noexcept         {spinlock::locker l(m_sl); return m_deq.end();             }
    auto pop_front()                    {spinlock::locker l(m_sl); return m_deq.pop_front();       }
    auto push_back(const T& v)          {spinlock::locker l(m_sl); return m_deq.push_back(v);      }
    auto empty() const noexcept         {spinlock::locker l(m_sl); return m_deq.empty();           }
    auto size()  const noexcept         {spinlock::locker l(m_sl); return m_deq.size();            }

    auto front() noexcept               {spinlock::locker l(m_sl); return m_deq.front();           }
    auto front() const noexcept         {spinlock::locker l(m_sl); return m_deq.front();           }
    auto back() noexcept                {spinlock::locker l(m_sl); return m_deq.back();            }
    auto back() const noexcept          {spinlock::locker l(m_sl); return m_deq.back();            }

    auto operator= (const spinlocked_deque& x)
                                        {spinlock::locker l(m_sl); return m_deq.operator=(x.m_deq);}
    auto operator= (spinlocked_deque&& x)
                                        {spinlock::locker l(m_sl); return m_deq.operator=(x.m_deq);}

private:
    deque<T> m_deq;
    mutable spinlock m_sl;
};


} // namespace detail


//template <typename T>
//using spinlocked_map<T> = detail::spinlocked_map<T>;
using detail::spinlocked_map;

//template <typename T>
//using spinlocked_set<T> = detail::spinlocked_set<T>;
using detail::spinlocked_set;

//template <typename T>
//using spinlocked_deque<T> = detail::spinlocked_deque<T>;
using detail::spinlocked_deque;


} // namespace sintra

#endif
