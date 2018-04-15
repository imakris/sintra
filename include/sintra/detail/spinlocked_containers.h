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



        template <template <typename...> typename CT, typename... Args>
        struct spinlocked
        {
            using iterator       = typename CT<Args...>::iterator;
            using const_iterator = typename CT<Args...>::const_iterator;

            auto back() noexcept                {spinlock::locker l(m_sl); return m_c.back();            }
            auto back() const noexcept          {spinlock::locker l(m_sl); return m_c.back();            }
            auto begin() noexcept               {spinlock::locker l(m_sl); return m_c.begin();           }
            auto begin() const noexcept         {spinlock::locker l(m_sl); return m_c.begin();           }
            auto clear() noexcept               {spinlock::locker l(m_sl); return m_c.clear();           }
            auto empty() const noexcept         {spinlock::locker l(m_sl); return m_c.empty();           }
            auto end()   noexcept               {spinlock::locker l(m_sl); return m_c.end();             }
            auto end()   const noexcept         {spinlock::locker l(m_sl); return m_c.end();             }

            template <typename... FArgs>
            auto erase(const FArgs&... v)       {spinlock::locker l(m_sl); return m_c.erase(v...);       }
            template <typename... FArgs>
            auto erase(FArgs&&... v)            {spinlock::locker l(m_sl); return m_c.erase(v...);       }

            template <typename... FArgs>
            auto find(const FArgs&... v)        {spinlock::locker l(m_sl); return m_c.find(v...);        }
            template <typename... FArgs>
            auto find(FArgs&&... v)             {spinlock::locker l(m_sl); return m_c.find(v...);        }

            auto front() noexcept               {spinlock::locker l(m_sl); return m_c.front();           }
            auto front() const noexcept         {spinlock::locker l(m_sl); return m_c.front();           }

            template <typename... FArgs>
            auto insert(const FArgs&... v)      {spinlock::locker l(m_sl); return m_c.insert(v...);      }
            template <typename... FArgs>
            auto insert(FArgs&&... v)           {spinlock::locker l(m_sl); return m_c.insert(v...);      }

            auto pop_front()                    {spinlock::locker l(m_sl); return m_c.pop_front();       }

            template <typename... FArgs>
            auto push_back(const FArgs&... v)   {spinlock::locker l(m_sl); return m_c.push_back(v...);   }

            auto size()  const noexcept         {spinlock::locker l(m_sl); return m_c.size();            }

            operator CT<Args...>() const        {spinlock::locker l(m_sl); return m_c;                   }
            auto operator=(const CT<Args...>& x){spinlock::locker l(m_sl); return m_c.operator=(x.m_c);  }
            auto operator=(CT<Args...>&& x)     {spinlock::locker l(m_sl); return m_c.operator=(x.m_c);  }

        protected:
            CT<Args...>         m_c;
            mutable spinlock    m_sl;
        };



        template <typename Key, typename T>
        struct spinlocked_umap: spinlocked<unordered_map, Key, T>
        {
            T& operator[] (const Key& k)        {spinlock::locker l(this->m_sl); return this->m_c[k];    }
            T& operator[] (Key&& k)             {spinlock::locker l(this->m_sl); return this->m_c[k];    }
        };



    } // namespace detail


    using detail::spinlocked_umap;

    template <typename T>
    using spinlocked_set = detail::spinlocked<unordered_set, T>;

    template <typename T>
    using spinlocked_deque = detail::spinlocked<deque, T>;


} // namespace sintra


#endif
