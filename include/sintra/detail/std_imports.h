// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sintra {

using std::atomic;
using std::atomic_flag;
using std::condition_variable;
using std::deque;
using std::enable_if;
using std::enable_if_t;
using std::function;
using std::is_base_of;
using std::is_base_of_v;
using std::is_const;
using std::is_convertible;
using std::is_reference;
using std::is_same;
using std::is_same_v;
using std::is_standard_layout_v;
using std::is_trivial_v;
using std::list;
using std::lock_guard;
using std::make_pair;
using std::map;
using std::mutex;
using std::recursive_mutex;
using std::remove_cv;
using std::remove_reference;
using std::runtime_error;
using std::set;
using std::shared_ptr;
using std::string;
using std::thread;
using std::unique_lock;
using std::unordered_map;
using std::unordered_set;
using std::vector;

} // namespace sintra
