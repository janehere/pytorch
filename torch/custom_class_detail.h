#pragma once

#include <ATen/core/boxing/kernel_functor.h>
#include <ATen/core/function.h>
#include <c10/util/Metaprogramming.h>
#include <c10/util/TypeTraits.h>

namespace torch {
namespace jit {

namespace detail {

// Argument type utilities
template <class R, class...>
struct types {
  using type = types;
};

template <typename Method>
struct WrapMethod;

template <typename R, typename CurrClass, typename... Args>
struct WrapMethod<R (CurrClass::*)(Args...)> {
  WrapMethod(R (CurrClass::*m)(Args...)) : m(std::move(m)) {}

  R operator()(c10::intrusive_ptr<CurrClass> cur, Args... args) {
    return c10::guts::invoke(m, *cur, args...);
  }

  R (CurrClass::*m)(Args...);
};

template <typename R, typename CurrClass, typename... Args>
struct WrapMethod<R (CurrClass::*)(Args...) const> {
  WrapMethod(R (CurrClass::*m)(Args...) const) : m(std::move(m)) {}

  R operator()(c10::intrusive_ptr<CurrClass> cur, Args... args) {
    return c10::guts::invoke(m, *cur, args...);
  }

  R (CurrClass::*m)(Args...) const;
};

// Adapter for different callable types
template <
    typename CurClass,
    typename Func,
    std::enable_if_t<
        std::is_member_function_pointer<std::decay_t<Func>>::value,
        bool> = false>
WrapMethod<Func> wrap_func(Func f) {
  return WrapMethod<Func>(std::move(f));
}

template <
    typename CurClass,
    typename Func,
    std::enable_if_t<
        !std::is_member_function_pointer<std::decay_t<Func>>::value,
        bool> = false>
Func wrap_func(Func f) {
  return f;
}

template <
    class Functor,
    bool AllowDeprecatedTypes,
    size_t... ivalue_arg_indices>
typename c10::guts::infer_function_traits_t<Functor>::return_type
call_torchbind_method_from_stack(
    Functor& functor,
    Stack& stack,
    std::index_sequence<ivalue_arg_indices...>) {
  (void)(stack); // when sizeof...(ivalue_arg_indices) == 0, this argument would
                 // be unused and we have to silence the compiler warning.

  constexpr size_t num_ivalue_args = sizeof...(ivalue_arg_indices);

  using IValueArgTypes =
      typename c10::guts::infer_function_traits_t<Functor>::parameter_types;
  return (functor)(c10::detail::ivalue_to_arg<
                   std::remove_cv_t<std::remove_reference_t<
                       c10::guts::typelist::
                           element_t<ivalue_arg_indices, IValueArgTypes>>>,
                   AllowDeprecatedTypes>(std::move(
      torch::jit::peek(stack, ivalue_arg_indices, num_ivalue_args)))...);
}

template <class Functor, bool AllowDeprecatedTypes>
typename c10::guts::infer_function_traits_t<Functor>::return_type
call_torchbind_method_from_stack(Functor& functor, Stack& stack) {
  constexpr size_t num_ivalue_args =
      c10::guts::infer_function_traits_t<Functor>::number_of_parameters;
  return call_torchbind_method_from_stack<Functor, AllowDeprecatedTypes>(
      functor, stack, std::make_index_sequence<num_ivalue_args>());
}

template <class RetType, class Func>
struct BoxedProxy;

template <class RetType, class Func>
struct BoxedProxy {
  void operator()(Stack& stack, Func& func) {
    auto retval = call_torchbind_method_from_stack<Func, false>(func, stack);
    constexpr size_t num_ivalue_args =
        c10::guts::infer_function_traits_t<Func>::number_of_parameters;
    torch::jit::drop(stack, num_ivalue_args);
    stack.emplace_back(c10::ivalue::from(std::move(retval)));
  }
};

template <class Func>
struct BoxedProxy<void, Func> {
  void operator()(Stack& stack, Func& func) {
    call_torchbind_method_from_stack<Func, false>(func, stack);
    constexpr size_t num_ivalue_args =
        c10::guts::infer_function_traits_t<Func>::number_of_parameters;
    torch::jit::drop(stack, num_ivalue_args);
    stack.emplace_back(IValue());
  }
};

} // namespace detail

TORCH_API void registerCustomClass(at::ClassTypePtr class_type);
TORCH_API void registerCustomClassMethod(std::shared_ptr<Function> method);

} // namespace jit
} // namespace torch
