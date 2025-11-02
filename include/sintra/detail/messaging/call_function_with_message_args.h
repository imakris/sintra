// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include <type_traits>

#include "../messaging/message_args.h"


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
        message_args_size<TVector>::value == sizeof...(Args)
    >
>
auto call_function_with_message_args_sfb(
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
        message_args_size<TVector>::value != sizeof...(Args)
    >
>
auto call_function_with_message_args_sfb(
    const TFunction& f,
    const TVector& t,
    const Args&... args)
{
    return call_function_with_message_args_sfb(
        f, t, args..., get< sizeof...(Args) >(t));
}


template <
    typename TFunction,
    typename TVector,
    typename = enable_if_t<
        message_args_size<TVector>::value != 0
    >
>
auto call_function_with_message_args(const TFunction& f, const TVector& t)
{
    // TODO: implement a static assertion for the statement that follows, to provide the message
    // in the compiler output.

    // if the compiler complains here, make sure that the function you are trying to call has
    // no non-const references
    return call_function_with_message_args_sfb(f, t, get<0>(t));
}


template <
    typename TFunction,
    typename TVector,
    typename = enable_if_t<
        message_args_size<TVector>::value == 0
    >
>
auto call_function_with_message_args(const TFunction& f, const TVector&) -> decltype(f())
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
        message_args_size<TVector>::value == sizeof...(Args)
    >
>
auto call_function_with_message_args_mfb(
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
        message_args_size<TVector>::value != sizeof...(Args)
    >
>
auto call_function_with_message_args_mfb(
    TObj& obj,
    const TFunction& f,
    const TVector& t,
    const Args&... args)
{
    return call_function_with_message_args_mfb(
        obj, f, t, args..., get< sizeof...(Args) >(t));
}


template <
    typename TObj,
    typename TFunction,
    typename TVector,
    typename = enable_if_t<
        message_args_size<TVector>::value != 0
    >
>
auto call_function_with_message_args(TObj& obj, const TFunction& f, const TVector& t)
{
    // TODO: implement a static assertion for the statement that follows, to provide the message
    // in the compiler output.

    // if the compiler complains here, make sure that none of the function arguments is either
    // - a non-const reference
    // - a non-POD and non STL container object
    return call_function_with_message_args_mfb(obj, f, t, get<0>(t));
}


template <
    typename TObj,
    typename TFunction,
    typename TVector,
    typename = enable_if_t<
        message_args_size<TVector>::value == 0
    >
>
auto call_function_with_message_args(
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



