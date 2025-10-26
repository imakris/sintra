// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include <type_traits>

#include "message_args.h"


namespace sintra {

    
using std::enable_if_t;
using detail::get;
using detail::message_args_size;


 //////////////////////////////////////////////////////////////////////////
///// BEGIN simple function backend ////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////
//////    //////    //////    //////    //////    //////    //////    //////
 ////      ////      ////      ////      ////      ////      ////      ////
  //        //        //        //        //        //        //        //


template <
    typename TFunction,
    typename TVector,
    typename... Args,
    typename = enable_if_t<
        detail::message_args_size<TVector>::value == sizeof...(Args)
    >
>
auto call_function_with_fusion_vector_args_sfb(
    const TFunction& f, const TVector&,
    const Args&... args) -> decltype(f(args...))
{
    return f(args...);
}


template <
    typename TFunction,
    typename TVector,
    typename... Args,
    typename = enable_if_t<
        detail::message_args_size<TVector>::value != sizeof...(Args)
    >
>
auto call_function_with_fusion_vector_args_sfb(
    const TFunction& f,
    const TVector& t,
    const Args&... args)
{
    return call_function_with_fusion_vector_args_sfb(
        f, t, args..., get< sizeof...(Args) >(t));
}


template <
    typename TFunction,
    typename TVector,
    typename = enable_if_t<
        detail::message_args_size<TVector>::value != 0
    >
>
auto call_function_with_fusion_vector_args(const TFunction& f, const TVector& t)
{
    // TODO: implement a static assertion for the statement that follows, to provide the message
    // in the compiler output.

    // if the compiler complains here, make sure that the function you are trying to call has
    // no non-const references
    return call_function_with_fusion_vector_args_sfb(f, t, get<0>(t));
}


template <
    typename TFunction,
    typename TVector,
    typename = enable_if_t<
        detail::message_args_size<TVector>::value == 0
    >
>
auto call_function_with_fusion_vector_args(const TFunction& f, const TVector&) -> decltype(f())
{
    // the function is called without arguments
    return f();
}



  //        //        //        //        //        //        //        //
 ////      ////      ////      ////      ////      ////      ////      ////
//////    //////    //////    //////    //////    //////    //////    //////
////////////////////////////////////////////////////////////////////////////
///// END simple function backend //////////////////////////////////////////
 //////////////////////////////////////////////////////////////////////////


 //////////////////////////////////////////////////////////////////////////
///// BEGIN member function backend ////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////
//////    //////    //////    //////    //////    //////    //////    //////
 ////      ////      ////      ////      ////      ////      ////      ////
  //        //        //        //        //        //        //        //



template <
    typename TObj,
    typename TFunction,
    typename TVector,
    typename... Args,
    typename = enable_if_t<
        detail::message_args_size<TVector>::value == sizeof...(Args)
    >
>
auto call_function_with_fusion_vector_args_mfb(
    TObj& obj,
    const TFunction& f,
    const TVector& /* t */,
    const Args&... args) -> decltype((obj.*f)(args...))
{
    return (obj.*f)(args...);
}


template <
    typename TObj,
    typename TFunction,
    typename TVector,
    typename... Args,
    typename = enable_if_t<
        detail::message_args_size<TVector>::value != sizeof...(Args)
    >
>
auto call_function_with_fusion_vector_args_mfb(
    TObj& obj,
    const TFunction& f,
    const TVector& t,
    const Args&... args)
{
    return call_function_with_fusion_vector_args_mfb(
        obj, f, t, args..., get< sizeof...(Args) >(t));
}


template <
    typename TObj,
    typename TFunction,
    typename TVector,
    typename = enable_if_t<
        detail::message_args_size<TVector>::value != 0
    >
>
auto call_function_with_fusion_vector_args(TObj& obj, const TFunction& f, const TVector& t)
{
    // TODO: implement a static assertion for the statement that follows, to provide the message
    // in the compiler output.

    // if the compiler complains here, make sure that none of the function arguments is either
    // - a non-const reference
    // - a non-POD and non STL container object
    return call_function_with_fusion_vector_args_mfb(obj, f, t, get<0>(t));
}


template <
    typename TObj,
    typename TFunction,
    typename TVector,
    typename = enable_if_t<
        detail::message_args_size<TVector>::value == 0
    >
>
auto call_function_with_fusion_vector_args(
    TObj& obj,
    TFunction f,
    const TVector&) -> decltype((obj.*f)())
{
    // the function is called without arguments
    return (obj.*f)();
}



  //        //        //        //        //        //        //        //
 ////      ////      ////      ////      ////      ////      ////      ////
//////    //////    //////    //////    //////    //////    //////    //////
////////////////////////////////////////////////////////////////////////////
///// END member function backend //////////////////////////////////////////
 //////////////////////////////////////////////////////////////////////////


} // namespace sintra;



/*

// This is a test

#include <iostream>
#include "message_args.h"


using namespace sintra;


int func0()
{
    std::cout << "func0" << '\n';
    return 5;
}


void func1(int a)
{
    std::cout << a << '\n';
}


float func2(int a, float b)
{
    std::cout << a << ' ' << b << '\n';
    return 0.2f;
}

struct B
{
    int mfunc0()
    {
        std::cout << "mfunc0" << '\n';
        return 5;
    }

    void mfunc1(int a)
    {
        std::cout << a << '\n';
    }

    float mfunc2(int a, float b)
    {
        std::cout << a << ' ' << b << '\n';
        return 0.2f;
    }
};



int main(void)
{
    detail::message_args<> t0;
    detail::message_args<int> t1{1};
    detail::message_args<int, float> t2{1, 1.5f};

    auto rv0 = call_function_with_fusion_vector_args(func0, t0);
    call_function_with_fusion_vector_args(func1, t1);
    auto rv2 = call_function_with_fusion_vector_args(func2, t2);

    std::cout << rv0 << ' ' << rv2 << '\n';

    B b;
    auto rv00 = call_function_with_fusion_vector_args(b, &B::mfunc0, t0);
    call_function_with_fusion_vector_args(b, &B::mfunc1, t1);
    auto rv22 = call_function_with_fusion_vector_args(b, &B::mfunc2, t2);

    std::cout << rv00 << ' ' << rv22 << '\n';

    return 0;
}

*/


