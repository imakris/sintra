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

#ifndef SINTRA_IPC_RINGS_H
#define SINTRA_IPC_RINGS_H

#include "config.h"

#include <atomic>
#include <cstdint>
#include <iomanip>
#include <string>
#include <thread>
#include <experimental/filesystem>

#include <omp.h>

#include <boost/interprocess/detail/os_file_functions.hpp>
#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/sync/interprocess_condition.hpp>
#include <boost/interprocess/sync/scoped_lock.hpp>

#undef max


namespace sintra {


using std::atomic;
using std::error_code;
using std::string;
using std::stringstream;
using std::thread;
namespace fs  = std::experimental::filesystem;
namespace ipc = boost::interprocess;


/*
A note on the types of the sequence counters:

The sequence counters are currently of type uint64_t. Iterating over INT_MAX in an empty loop,
takes about 1 second in a desktop system assembled in 2013. Assuming similar performance for
64-bit counters, this would take about 70 years, and the loop of a message ring is orders of
magnitude slower than an empty loop.

Thus the overflow of uint64_t is practically unreachable and will probably not be an issue
to consider during the lifetime of this code.
*/

using sequence_counter_type = uint64_t;


//
// counts an integer's number of one bits (source: http://aggregate.org/MAGIC/)
//
inline
uint32_t ones32(uint32_t x)
{
    x -= ((x >> 1) & 0x55555555);
    x = (((x >> 2) & 0x33333333) + (x & 0x33333333));
    x = (((x >> 4) + x) & 0x0f0f0f0f);
    x += (x >> 8);
    x += (x >> 16);
    return(x & 0x0000003f);
}

//
// (source: http://aggregate.org/MAGIC/)
//
inline
uint32_t floor_log2(uint32_t x)
{
    x |= (x >>  1);
    x |= (x >>  2);
    x |= (x >>  4);
    x |= (x >>  8);
    x |= (x >> 16);

    return(ones32(x >> 1));
}



// helpers
inline
bool check_or_create_directory(const string& dir_name)
{
    bool c = true;
    error_code ec;
    fs::path ps(dir_name);
    if (!fs::exists(ps)) {
        c &= fs::create_directory(ps, ec);
    }
    else
    if (fs::is_regular_file(ps)) {
        c &= fs::remove(ps, ec);
        c &= fs::create_directory(ps, ec);
    }
    return c;
}


inline bool remove_directory(const string& dir_name)
{
    uintmax_t c = 0;
    error_code ec;
    fs::path ps(dir_name);
    auto rv = fs::remove_all(dir_name.c_str(), ec);

    // [TESTING] please do not enable
    //if (!rv) {
    //    string msg = ec.message();
    //}
    return !!rv;
}


 //////////////////////////////////////////////////////////////////////////
///// BEGIN Ring ///////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////
//////   \//////   \//////   \//////   \//////   \//////   \//////   \//////
 ////     \////     \////     \////     \////     \////     \////     \////
  //       \//       \//       \//       \//       \//       \//       \//

// Ring sizes should be powers of 2 and multiple of the page size
template <int NUM_ELEMENTS, typename T>
struct Ring
{
    struct Control
    {
        // This struct is always instantiated in a memory region which is shared among processes.
        // If my understanding is correct, and the implementation is also correct, the use of
        // atomics should be fine for that purpose. [see N3337 29.4]

        atomic<size_t>                  num_attached;
        atomic<size_t>                  read_access[2];

        //the index of the nth element written to the ringbuffer.
        atomic<sequence_counter_type>   leading_sequence;

        // Used to avoid accidentaly having multiple writers on the same ring
        ipc::interprocess_mutex         ownership_mutex;


        ipc::interprocess_mutex         condition_mutex;
        ipc::interprocess_condition     dirty_condition;

#if !defined(SINTRA_RING_READING_POLICY_ALWAYS_SPIN)
        atomic<int>                     sleeping_readers;
#endif

        Control():
            num_attached(0)
        ,   leading_sequence(0)
#if !defined(SINTRA_RING_READING_POLICY_ALWAYS_SPIN)
        ,   sleeping_readers(0)
#endif
        {
            read_access[0] = 0;
            read_access[1] = 0;

            // See the 'Note' in N4713 32.5 [Lock-free property], Par. 4.
            // The program is only valid if the conditions below are true.
            assert(num_attached.is_lock_free());        // size_t
            assert(leading_sequence.is_lock_free());    // sequence_counter_type
            assert(sleeping_readers.is_lock_free());    // int
        }
    };

    class acquisition_failure_exception {};

    enum { capacity         = NUM_ELEMENTS           };
    enum { data_region_size = NUM_ELEMENTS/sizeof(T) };


    Ring(const string& directory, const string& prefix, uint64_t id);
    ~Ring();

    const uint64_t      m_id;

protected:
    using region_ptr_type = ipc::mapped_region*;

    bool create();
    bool destroy();
    bool attach();
    bool detach();

    region_ptr_type                     m_data_region_0     = nullptr;
    region_ptr_type                     m_data_region_1     = nullptr;
    region_ptr_type                     m_control_region    = nullptr;

    T*                                  m_data              = nullptr;
    Control*                            m_control           = nullptr;

    string                              m_directory;
    string                              m_data_filename;
    string                              m_control_filename;

    const uint32_t                      m_semiring_shift    = floor_log2(NUM_ELEMENTS) - 1;

    friend struct Managed_process;
};



template <int NUM_ELEMENTS, typename T>
Ring<NUM_ELEMENTS, T>::Ring(const string& directory, const string& prefix, uint64_t id):
    m_id(id)
{
    stringstream stream;
    stream << std::hex << id;

    m_directory = directory + "/";
    m_data_filename = m_directory + prefix + stream.str();
    m_control_filename = m_data_filename + "_control";

    fs::path pr(m_data_filename);
    fs::path pc(m_control_filename);

    bool c1 = fs::exists(pr) && fs::is_regular_file(pr) && fs::file_size(pr) &&
              fs::exists(pc) && fs::is_regular_file(pc) && fs::file_size(pc);
    bool c2 = c1 || create();

    if (!c2 || !attach()) {
        throw acquisition_failure_exception();
    }

    if (!c1) {
        try {
            new (m_control) Control;
        }
        catch (...) {
            throw acquisition_failure_exception();
        }
    }
    
    m_control->num_attached++;
}


template <int NUM_ELEMENTS, typename T>
Ring<NUM_ELEMENTS, T>::~Ring()
{
    if (m_control->num_attached-- == 1) {
        detach();
        destroy();
    }
    else {
        detach();
    }
}


template <int NUM_ELEMENTS, typename T>
bool Ring<NUM_ELEMENTS, T>::create()
{
    try {
        if (!check_or_create_directory(m_directory))
            return false;

        ipc::file_handle_t fh_data =
            ipc::ipcdetail::create_new_file(m_data_filename.c_str(),    ipc::read_write);
        if (fh_data == ipc::ipcdetail::invalid_file())
            return false;

        ipc::file_handle_t fh_control =
            ipc::ipcdetail::create_new_file(m_control_filename.c_str(), ipc::read_write);
        if (fh_control == ipc::ipcdetail::invalid_file())
            return false;

#ifdef NDEBUG
        if (!ipc::ipcdetail::truncate_file(fh_data,    data_region_size    ))
            return false;
        if (!ipc::ipcdetail::truncate_file(fh_control, sizeof(Control)))
            return false;
#else
        auto ustring = "UNINITIALIZED";
        auto dv = strlen(ustring);
        char* u_data    = new char[data_region_size];
        char* u_control = new char[sizeof(Control)];
        for (size_t i=0; i<data_region_size; i++)
            u_data[i]    = ustring[i%dv];
        for (size_t i=0; i<sizeof(Control);  i++)
            u_control[i] = ustring[i%dv];

        ipc::ipcdetail::write_file(fh_data,  u_data, data_region_size);
        ipc::ipcdetail::write_file(fh_control, u_control,sizeof(Control));

        delete [] u_data;
        delete [] u_control;
#endif

        bool success = false;
        success |= ipc::ipcdetail::close_file(fh_data);
        success |= ipc::ipcdetail::close_file(fh_control);

        return success;
    }
    catch (...) {
    }
    return false;
}


template <int NUM_ELEMENTS, typename T>
bool Ring<NUM_ELEMENTS, T>::destroy()
{
    try {
        fs::path pr(m_data_filename);
        fs::path pc(m_control_filename);
        return remove(pr) && remove(pc);
    }
    catch (...) {
    }
    return false;
}


template <int NUM_ELEMENTS, typename T>
bool Ring<NUM_ELEMENTS, T>::attach()
{
    assert(
        m_data_region_0  == nullptr &&
        m_data_region_1  == nullptr &&
        m_control_region == nullptr &&
        m_data           == nullptr);

    try {
        if (fs::file_size(m_data_filename)      != data_region_size ||
            fs::file_size(m_control_filename)   != sizeof(Control))
        {
            return false;
        }

        size_t page_size = ipc::mapped_region::get_page_size();
        assert(data_region_size % page_size == 0);

        // WARNING: This might eventually require system specific implementations.
        // [translation: it has not failed so far, thus it's still here. If it fails, reimplement]
        void *mem = malloc(data_region_size * 2 + page_size);
        char *ptr = (char*)(ptrdiff_t((char *)mem + page_size) & ~(page_size - 1));

        ipc::file_mapping file(m_data_filename.c_str(), ipc::read_write);

        ipc::map_options_t map_extra_options = 0;

#ifdef _WIN32
        free(mem); // here we make the assumption that that pages are put back into the free list.
#else
        // on Linux however, we do not free.
    #ifdef MAP_FIXED
        map_extra_options |= MAP_FIXED;
    #endif

    #ifdef MAP_NOSYNC
        map_extra_options |= MAP_NOSYNC
    #endif
#endif

        m_data_region_0 = new ipc::mapped_region(
            file, ipc::read_write, 0, data_region_size, ptr, map_extra_options);
        m_data_region_1 = new ipc::mapped_region(
            file, ipc::read_write, 0, 0,
            ((char*)m_data_region_0->get_address()) + data_region_size, map_extra_options);
        m_data = (T*)m_data_region_0->get_address();

        assert(m_data_region_0->get_address() == ptr);
        assert(m_data_region_1->get_address() == ptr + data_region_size);
        assert(m_data_region_0->get_size() == data_region_size);
        assert(m_data_region_1->get_size() == data_region_size);

        ipc::file_mapping fm_control(m_control_filename.c_str(), ipc::read_write);
        m_control_region = new ipc::mapped_region(fm_control, ipc::read_write, 0, 0);
        m_control = (Control*)m_control_region->get_address();

        return true;
    }
    catch (...) {
        return false;
    }
}


template <int NUM_ELEMENTS, typename T>
bool Ring<NUM_ELEMENTS, T>::detach()
{
    delete m_data_region_0;
    delete m_data_region_1;
    delete m_control_region;

    m_data_region_0 = nullptr;
    m_data_region_1 = nullptr;
    m_control_region = nullptr;
    m_data = nullptr;

    return true;
}


  //\       //\       //\       //\       //\       //\       //\       //
 ////\     ////\     ////\     ////\     ////\     ////\     ////\     ////
//////\   //////\   //////\   //////\   //////\   //////\   //////\   //////
////////////////////////////////////////////////////////////////////////////
///// END Ring /////////////////////////////////////////////////////////////
 //////////////////////////////////////////////////////////////////////////


 //////////////////////////////////////////////////////////////////////////
///// BEGIN Ring_R /////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////
//////   \//////   \//////   \//////   \//////   \//////   \//////   \//////
 ////     \////     \////     \////     \////     \////     \////     \////
  //       \//       \//       \//       \//       \//       \//       \//


template <int NUM_ELEMENTS, typename T>
struct Ring_R: Ring<NUM_ELEMENTS, T>
{
    Ring_R(const string& directory, const string& prefix, uint64_t id):
        Ring<NUM_ELEMENTS, char>::Ring(directory, prefix, id),
        m_unblocked(false)
    {
        m_reading_sequence = this->m_control->leading_sequence.load();
        m_semiring = (m_reading_sequence >> this->m_semiring_shift) & 1;
        this->m_control->read_access[m_semiring]++;
    }

    ~Ring_R()
    {
        this->m_control->read_access[m_semiring]--;
    }


    // Returns a pointer to the first element that is pending to be read, or the head of the ring,
    // if there are no elements available.
    // The number of elements in the ring available to the reader are returned in
    // num_available_elements.
    // The function will block until elements become available or it is explicitly unblocked.
    // The caller must call done_reading() once it is done accessing the ring.
    T* start_reading(size_t* num_available_elements)
    {

#if defined(SINTRA_RING_READING_POLICY_ALWAYS_SLEEP)

        ipc::scoped_lock<ipc::interprocess_mutex> lock(m_control->condition_mutex);
        while (m_reading_sequence == m_control->leading_sequence.load() && !m_unblocked) {

            m_control->sleeping_readers++;

            if (m_reading_sequence == m_control->leading_sequence.load() && !m_unblocked) {
                m_control->dirty_condition.wait(lock);
            }

            m_control->sleeping_readers--;
        }

#elif defined(SINTRA_RING_READING_POLICY_ALWAYS_SPIN)

        while (m_reading_sequence == m_control->leading_sequence.load() && !m_unblocked) {}

#elif defined(SINTRA_RING_READING_POLICY_HYBRID)

        // if there is nothing to read
        if (m_reading_sequence == this->m_control->leading_sequence.load()) {

            double time_limit = omp_get_wtime() + spin_before_sleep * 0.5;
            size_t sc = 0;
            size_t sc_limit = 40; // initial spin count limit, to reduce the calls to omp_get_wtime

            while (m_reading_sequence == this->m_control->leading_sequence.load() && !m_unblocked) {
                
                if (sc++ > sc_limit) {

                    double tmp = omp_get_wtime();
                    if (tmp < time_limit) {
                        time_limit = tmp + spin_before_sleep * 0.5;
                        sc = 0;
                        sc_limit *= 2;
                        continue;
                    }

                    // if sufficient repetitions (=time) have taken place, go to sleep

                    // the lock has to precede the 'if' block, to prevent a missed notification.
                    ipc::scoped_lock<ipc::interprocess_mutex> lock(this->m_control->condition_mutex);

                    this->m_control->sleeping_readers++;

                    if (m_reading_sequence == this->m_control->leading_sequence.load() &&
                        !m_unblocked)
                    {
                        this->m_control->dirty_condition.wait(lock);
                    }
                    
                    this->m_control->sleeping_readers--;
                }
            }
        }

#endif

        if (m_unblocked) {
            *num_available_elements = 0;
            return nullptr;
        }

        m_num_elements_being_read =
            size_t(this->m_control->leading_sequence.load() - m_reading_sequence);
        *num_available_elements = m_num_elements_being_read;

        // return the pointer to the reading location
        return this->m_data + (m_reading_sequence % NUM_ELEMENTS);
    }



    // Increments the reading sequence counter by as many sequences as it was returned in
    // num_available_elements.
    void done_reading()
    {
        m_reading_sequence += m_num_elements_being_read;
        m_num_elements_being_read = 0;

        size_t new_semiring = (m_reading_sequence >> this->m_semiring_shift) & 1;
        if (new_semiring != m_semiring) {
            this->m_control->read_access[new_semiring]++;
            this->m_control->read_access[  m_semiring]--;
            m_semiring = new_semiring;
        }
    }

    // If start_reading() is either sleeping or spinning, it will force it to return a nullptr
    // and 0 elements.
    void unblock()
    {
        // TODO: this implementation is a bit odd... it's acquiring an INTERPROCESS lock
        // "just in case" the other thread is sleeping.
        // On the other hand, it's simple enough, for a call that should only go to the very end.

        ipc::scoped_lock<ipc::interprocess_mutex> lock(this->m_control->condition_mutex);
        m_unblocked = true;
        this->m_control->dirty_condition.notify_all();
    }

private:
    sequence_counter_type m_reading_sequence;
    size_t m_semiring;

    size_t m_num_elements_being_read = 0;
    atomic<bool> m_unblocked;
};



  //\       //\       //\       //\       //\       //\       //\       //
 ////\     ////\     ////\     ////\     ////\     ////\     ////\     ////
//////\   //////\   //////\   //////\   //////\   //////\   //////\   //////
////////////////////////////////////////////////////////////////////////////
///// END Ring_R ///////////////////////////////////////////////////////////
 //////////////////////////////////////////////////////////////////////////


 //////////////////////////////////////////////////////////////////////////
///// BEGIN Ring_W /////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////
//////   \//////   \//////   \//////   \//////   \//////   \//////   \//////
 ////     \////     \////     \////     \////     \////     \////     \////
  //       \//       \//       \//       \//       \//       \//       \//


template <int NUM_ELEMENTS, typename T>
struct Ring_W: Ring<NUM_ELEMENTS, T>
{
    atomic<thread::id> m_writing_thread;

    Ring_W(const string& directory, const string& prefix, uint64_t id):
        Ring<NUM_ELEMENTS, char>::Ring(directory, prefix, id)
    {
        if (!this->m_control->ownership_mutex.try_lock()) {
            using afe = typename Ring<NUM_ELEMENTS, T>::acquisition_failure_exception;
            throw afe();
        }
    }

    ~Ring_W()
    {
        this->m_control->ownership_mutex.unlock();
    }


    inline
    T* prepare_write(size_t num_elements_to_write)
    {
        // assure exclusive write access
        while (m_writing_thread != std::this_thread::get_id()) {
            auto invalid_thread = thread::id();
            m_writing_thread.compare_exchange_strong(invalid_thread, std::this_thread::get_id());
        }

        size_t index = m_pending_new_sequence % NUM_ELEMENTS;
        m_pending_new_sequence += num_elements_to_write;
        size_t new_semiring = (m_pending_new_sequence >> this->m_semiring_shift) & 1;

        // if the writing range has not been acquired
        if (m_semiring != new_semiring) {

            // if anyone is reading the new_semiring, wait (spin) to avoid an overwrite
            while (this->m_control->read_access[new_semiring].load()) {}

            m_semiring = new_semiring;
        }
        return this->m_data+index;
    }


    // The generic version, writing a buffer of ring elements
    T* write(const T* src_buffer, size_t num_src_elements)
    {
        T* write_location = prepare_write(num_src_elements);

        for (size_t i = 0; i < num_src_elements*sizeof(T); i++) {
            write_location[i] = src_buffer[i];
        }
        return write_location;
    }


    // The specialised version, writing (copying) an arbitrary type object into the ring
    template <typename T2>
    T2* write(size_t num_extra_bytes, T2&& obj)
    {
        auto num_bytes = sizeof(T2)+num_extra_bytes;
        assert(num_bytes % sizeof(T) == 0);

        T2* write_location = (T2*) prepare_write(num_bytes / sizeof(T));

        // write (copy) the message
        *(write_location) = obj;

        // [TESTING] please do not enable
        //using pod_type = typename Make_pod<T2>::type;
        //*((pod_type*)(write_location)) = (pod_type&&)obj;
        return write_location;
    }


    // The specialised version, for constructing an arbitrary type object in-place into the ring
    template <typename T2, typename... Args>
    T2* write(size_t num_extra_bytes, Args... args)
    {
        auto num_bytes = sizeof(T2) + num_extra_bytes;
        assert(num_bytes % sizeof(T) == 0);

        T2* write_location = (T2*) prepare_write(num_bytes / sizeof(T));

        // write (construct) the message
        return new (write_location) T2(args...);
    }


    void done_writing()
    {

        // update sequence
        this->m_control->leading_sequence.store(m_pending_new_sequence);

#if defined(SINTRA_RING_READING_POLICY_ALWAYS_SLEEP)

        {
            ipc::scoped_lock<ipc::interprocess_mutex> lock(m_control->condition_mutex);
            m_control->dirty_condition.notify_all();
        }

#elif defined(SINTRA_RING_READING_POLICY_ALWAYS_SPIN)

        // nothing to do...

#elif defined(SINTRA_RING_READING_POLICY_HYBRID)


        // from this point on, any comparison of the form:
        // m_reading_sequence == m_control->leading_sequence
        // on the reader will keep failing until done_reading() is called
        int expected = 0;
        if (!this->m_control->sleeping_readers.compare_exchange_weak(expected, expected))
        {
            ipc::scoped_lock<ipc::interprocess_mutex> lock(this->m_control->condition_mutex);
            this->m_control->dirty_condition.notify_all();
        }

#endif


        m_writing_thread = thread::id();
    }


private:
    // something referring to the writing range
    size_t m_semiring = 0;
    sequence_counter_type m_pending_new_sequence = 0;
};



  //\       //\       //\       //\       //\       //\       //\       //
 ////\     ////\     ////\     ////\     ////\     ////\     ////\     ////
//////\   //////\   //////\   //////\   //////\   //////\   //////\   //////
////////////////////////////////////////////////////////////////////////////
///// END Ring_W ///////////////////////////////////////////////////////////
 //////////////////////////////////////////////////////////////////////////

} // namespace sintra

#endif
