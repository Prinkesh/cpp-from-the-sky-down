// Copyright 2018 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// limitations under the License.

#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

namespace polymorphic {
namespace detail {

template <typename T> using ptr = T *;

template <typename T> struct type {};

using index_type = std::uint8_t;

using vtable_fun = ptr<void()>;

template <size_t I, typename Signature> struct vtable_entry;

template <size_t I, typename Method, typename Return, typename... Parameters>
struct vtable_entry<I, Return(Method, Parameters...)> {
  template <typename T> static auto get_entry(type<T>) {
    return reinterpret_cast<vtable_fun>(
        +[](void *t, Parameters... parameters) -> Return {
          return poly_extend(static_cast<Method *>(nullptr),
                             *static_cast<T *>(t), parameters...);
        });
  }

  static decltype(auto) call_imp(const vtable_fun *vt,
                                 const index_type *permutation, Method *,
                                 void *t, Parameters... parameters) {
    return reinterpret_cast<fun_ptr>(vt[permutation[I]])(t, parameters...);
  }

  static auto get_index(type<Return(Method, Parameters...)>) { return I; }

  using is_const = std::false_type;
  using fun_ptr = ptr<Return(void *, Parameters...)>;
};

template <size_t I, typename Method, typename Return, typename... Parameters>
struct vtable_entry<I, Return(Method, Parameters...) const> {
  template <typename T> static auto get_entry(type<T>) {
    return reinterpret_cast<vtable_fun>(
        +[](const void *t, Parameters... parameters) -> Return {
          return poly_extend(static_cast<Method *>(nullptr),
                             *static_cast<const T *>(t), parameters...);
        });
  }

  static decltype(auto) call_imp(const vtable_fun *vt,
                                 const index_type *permutation, Method *,
                                 const void *t, Parameters... parameters) {
    return reinterpret_cast<fun_ptr>(vt[permutation[I]])(t, parameters...);
  }
  static auto get_index(type<Return(Method, Parameters...) const>) { return I; }

  using is_const = std::true_type;
  using fun_ptr = ptr<Return(const void *, Parameters...)>;
};

template <typename T> using is_const_t = typename T::is_const;

template <typename... entry> struct entries : entry... {

  using entry::call_imp...;
  using entry::get_entry...;
  using entry::get_index...;

  static constexpr bool all_const() {
    return std::conjunction_v<is_const_t<entry>...>;
  }
};

template <typename Sequence, typename... Signatures> struct vtable_imp;

template <size_t... I, typename... Signatures>
struct vtable_imp<std::index_sequence<I...>, Signatures...>
    : entries<vtable_entry<I, Signatures>...> {
  static_assert(sizeof...(Signatures) <=
                std::numeric_limits<index_type>::max());

  template <typename T> static auto get_vtable(type<T> t) {
    static const vtable_fun vt[] = {
        vtable_entry<I, Signatures>::get_entry(t)...};
    return &vt[0];
  }

  template <typename T>
  vtable_imp(type<T> t) : vptr_(get_vtable(t)), permutation_{I...} {}

  const vtable_fun *vptr_;
  std::array<detail::index_type, sizeof...(Signatures)> permutation_;

  template <typename OtherSequence, typename... OtherSignatures>
  vtable_imp(const vtable_imp<OtherSequence, OtherSignatures...> &other)
      : vptr_(other.vptr_),
        permutation_{
            other.permutation_[other.get_index(type<Signatures>{})]...} {}

  template <typename VoidType, typename Method, typename... Parameters>
  decltype(auto) call(Method *method, VoidType t,
                      Parameters &&... parameters) const {
    return vtable_imp::call_imp(vptr_, permutation_.data(), method, t,
                                std::forward<Parameters>(parameters)...);
  }
};

template <typename... Signatures>
using vtable =
    vtable_imp<std::make_index_sequence<sizeof...(Signatures)>, Signatures...>;

template <typename T> std::false_type is_vtable(const T *);

template <typename Sequence, typename... Signatures>
std::true_type is_vtable(const vtable_imp<Sequence, Signatures...> &);

template <typename T, typename = std::void_t<>>
struct is_polymorphic : std::false_type {};

template <typename T>
struct is_polymorphic<
    T, std::void_t<decltype(std::declval<const T &>().get_vtable())>>
    : decltype(is_vtable(std::declval<const T &>().get_vtable())) {};

} // namespace detail

template <typename... Signatures> class ref {
  using vtable_t = detail::vtable<Signatures...>;

  vtable_t vt_;
  std::conditional_t<vtable_t::all_const(), const void *, void *> t_;

public:
  template <typename T, typename = std::enable_if_t<
                            !detail::is_polymorphic<std::decay_t<T>>::value>>
  ref(T &t) : vt_(detail::type<std::decay_t<T>>{}), t_(&t) {}

  template <typename Poly, typename = std::enable_if_t<detail::is_polymorphic<
                               std::decay_t<Poly>>::value>>
  ref(const Poly &other) : vt_(other.get_vtable()), t_(other.get_ptr()){};

  explicit operator bool() const { return t_ != nullptr; }

  const auto &get_vtable() const { return vt_; }

  auto get_ptr() const { return t_; }

  template <typename Method, typename... Parameters>
  decltype(auto) call(Parameters &&... parameters) const {
    return vt_.call(static_cast<Method *>(nullptr), t_,
                    std::forward<Parameters>(parameters)...);
  }
};

namespace detail {

struct clone {};

struct holder {
  void *ptr;
  ~holder() {}
};

template <typename T> struct holder_imp : holder {
  T t_;
  holder_imp(T t) : t_(std::move(t)) { ptr = &t_; }
};

template <typename T> std::unique_ptr<holder> poly_extend(clone *, const T &t) {
  return std::make_unique<holder_imp<T>>(t);
}

} // namespace detail

using copyable = std::unique_ptr<detail::holder>(detail::clone) const;

namespace detail {

template <bool all_const, typename... Signatures> class object_imp {
  using vtable_t = const detail::vtable<Signatures...>;

  vtable_t vt_;
  std::unique_ptr<holder> t_;

public:
  const auto &get_vtable() const { return vt_; }
  template <typename T, typename = std::enable_if_t<
                            !detail::is_polymorphic<std::decay_t<T>>::value>>
  object_imp(T t)
      : vt_(detail::type<T>{}),
        t_(std::make_unique<detail::holder_imp<T>>(std::move(t))) {}

  template <typename Poly, typename = std::enable_if_t<detail::is_polymorphic<
                               std::decay_t<Poly>>::value>>
  object_imp(const Poly &other)
      : vt_(other.get_vtable()),
        t_(other ? other.template call<detail::clone>() : nullptr) {}

  object_imp(const object_imp &other)
      : vt_{other.get_vtable()},
        t_(other ? other.call<detail::clone>() : nullptr) {}

  object_imp(object_imp &&) = default;

  object_imp &operator=(object_imp &&) = default;

  object_imp &operator=(const object_imp &other) { (*this) = ref(other); }

  explicit operator bool() const { return t_ != nullptr; }

  void *get_ptr() { return t_ ? t_->ptr : nullptr; }
  const void *get_ptr() const { return t_ ? t_->ptr : nullptr; }

  template <typename Method, typename... Parameters>
  decltype(auto) call(Parameters &&... parameters) {
    return vt_.call(static_cast<Method *>(nullptr), get_ptr(),
                    std::forward<Parameters>(parameters)...);
  }

  template <typename Method, typename... Parameters>
  decltype(auto) call(Parameters &&... parameters) const {
    return vt_.call(static_cast<Method *>(nullptr), get_ptr(),
                    std::forward<Parameters>(parameters)...);
  }
};

template <typename... Signatures> class object_imp<true, Signatures...> {
  using vtable_t = const detail::vtable<Signatures...>;

  vtable_t vt_;
  std::shared_ptr<const holder> t_;

public:
  const auto &get_vtable() const { return vt_; }
  template <typename T, typename = std::enable_if_t<
                            !detail::is_polymorphic<std::decay_t<T>>::value>>
  object_imp(T t)
      : vt_(detail::type<T>{}),
        t_(std::make_shared<holder_imp<T>>(std::move(t))) {}

  template <typename Poly, typename = std::enable_if_t<detail::is_polymorphic<
                               std::decay_t<Poly>>::value>>
  object_imp(const Poly &other)
      : vt_(other.get_vtable()),
        t_(other ? other.template call<detail::clone>() : nullptr) {}

  template <typename... OtherSignatures>
  object_imp(const object_imp<true, OtherSignatures...> &other)
      : vt_(other.get_vtable()), t_(other.t_) {}

  object_imp(const object_imp &) = default;

  object_imp(object_imp &&) = default;

  object_imp &operator=(object_imp &&) = default;

  object_imp &operator=(const object_imp &) = default;

  explicit operator bool() const { return t_ != nullptr; }

  const void *get_ptr() const { return t_ ? t_->ptr : nullptr; }

  template <typename Method, typename... Parameters>
  decltype(auto) call(Parameters &&... parameters) const {
    return vt_.call(static_cast<Method *>(nullptr), get_ptr(),
                    std::forward<Parameters>(parameters)...);
  }
};

} // namespace detail

template <typename... Signatures>
using object = detail::object_imp<detail::vtable<Signatures...>::all_const(),
                                  Signatures..., copyable>;

} // namespace polymorphic