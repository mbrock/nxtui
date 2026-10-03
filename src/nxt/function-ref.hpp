#pragma once

#include <cassert>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace nxt {

/// A compile-time callable, optionally bound to an object by function_ref.
template<auto Target>
struct nontype_t
{};

template<auto Target>
inline constexpr nontype_t<Target> nontype{};

/// A non-owning callable reference, available in C++23. The callable must
/// outlive every invocation, including any task that borrows its closure.
/// Temporaries are safe only within the full expression that creates them.
///
/// Supports R(Args...), const and noexcept signatures, function pointers,
/// callable objects and compile-time binding via nxt::nontype<Target>.
/// It has no empty state and does not allocate, copy or destroy the
/// callable. Optional callbacks can use
/// std::optional<function_ref<Signature>>. This is a local callable view,
/// not a complete std::function_ref backport.
template<typename Signature>
class function_ref;

namespace detail {

template<typename T>
inline constexpr bool is_function_ref = false;

template<typename Signature>
inline constexpr bool is_function_ref<function_ref<Signature>> = true;

template<typename T>
inline constexpr bool is_function_pointer =
    std::is_pointer_v<std::remove_cvref_t<T>>
    && std::is_function_v<std::remove_pointer_t<std::remove_cvref_t<T>>>;

// Keep function and object pointers in separate union members: converting
// a function pointer to void* is not portable C++.
union function_ref_entity
{
    const volatile void * object;
    void (*function)();
};

template<typename R, bool Const, bool Noexcept, typename... Args>
class function_ref_base
{
    template<typename T>
    using qualified = std::conditional_t<Const, std::add_const_t<T>, T>;

    template<typename... T>
    static constexpr bool invocable =
        Noexcept ? std::is_nothrow_invocable_r_v<R, T..., Args...>
                 : std::is_invocable_r_v<R, T..., Args...>;

    template<typename T>
    static qualified<T> * object(function_ref_entity entity) noexcept
    {
        return static_cast<qualified<T> *>(
            const_cast<void *>(entity.object));
    }

public:
    template<typename F>
        requires std::is_function_v<F> && invocable<F *>
    function_ref_base(F * function) noexcept
        : entity_{.function = reinterpret_cast<void (*)()>(function)}
        , invoke_(
              [](function_ref_entity entity,
                 Args &&... args) noexcept(Noexcept) -> R {
                  return std::invoke_r<R>(
                      reinterpret_cast<F *>(entity.function),
                      std::forward<Args>(args)...);
              })
    {
        assert(function != nullptr);
    }

    template<typename F>
        requires(!is_function_ref<std::remove_cvref_t<F>>)
                    && (!std::is_function_v<std::remove_reference_t<F>>)
                    && (!std::is_member_pointer_v<
                        std::remove_reference_t<F>>)
                    && (!is_function_pointer<F>)
                    && invocable<qualified<std::remove_reference_t<F>> &>
    constexpr function_ref_base(F && function) noexcept
        : entity_{.object = std::addressof(function)}
        , invoke_(
              [](function_ref_entity entity,
                 Args &&... args) noexcept(Noexcept) -> R {
                  return std::invoke_r<R>(
                      *object<std::remove_reference_t<F>>(entity),
                      std::forward<Args>(args)...);
              })
    {
    }

    template<auto Target>
        requires invocable<decltype((Target))>
    constexpr function_ref_base(nontype_t<Target>) noexcept
        : entity_{.object = nullptr}
        , invoke_(
              [](function_ref_entity,
                 Args &&... args) noexcept(Noexcept) -> R {
                  return std::invoke_r<R>(
                      Target, std::forward<Args>(args)...);
              })
    {
        check_target<Target>();
    }

    template<auto Target, typename T>
        requires(!std::is_function_v<T>)
                    && invocable<decltype((Target)), qualified<T> &>
    constexpr function_ref_base(nontype_t<Target>, T & bound) noexcept
        : entity_{.object = std::addressof(bound)}
        , invoke_(
              [](function_ref_entity entity,
                 Args &&... args) noexcept(Noexcept) -> R {
                  return std::invoke_r<R>(
                      Target,
                      *object<T>(entity),
                      std::forward<Args>(args)...);
              })
    {
        check_target<Target>();
    }

    template<auto Target, typename T>
        requires(!std::is_function_v<T>)
                    && invocable<decltype((Target)), qualified<T> *>
    constexpr function_ref_base(nontype_t<Target>, T * bound) noexcept
        : entity_{.object = bound}
        , invoke_(
              [](function_ref_entity entity,
                 Args &&... args) noexcept(Noexcept) -> R {
                  return std::invoke_r<R>(
                      Target,
                      object<T>(entity),
                      std::forward<Args>(args)...);
              })
    {
        check_target<Target>();
        if constexpr (std::is_member_pointer_v<decltype(Target)>)
            assert(bound != nullptr);
    }

    // Store the function pointer value, not the caller's pointer variable.
    template<typename F>
        requires std::is_function_v<F> && invocable<F *>
    function_ref_base & operator=(F * function) noexcept
    {
        *this = function_ref_base{function};
        return *this;
    }

    // Copy another view, or assign a function pointer by value.
    // Assigning a temporary callable object would immediately dangle.
    template<typename F>
        requires(!is_function_ref<std::remove_cvref_t<F>>)
                    && (!is_function_pointer<F>)
    function_ref_base & operator=(F) = delete;

    R operator()(Args... args) const noexcept(Noexcept)
    {
        return invoke_(entity_, std::forward<Args>(args)...);
    }

private:
    template<auto Target>
    static constexpr void check_target() noexcept
    {
        if constexpr (
            std::is_pointer_v<decltype(Target)>
            || std::is_member_pointer_v<decltype(Target)>)
            static_assert(Target != nullptr);
    }

    function_ref_entity entity_;
    R (*invoke_)(function_ref_entity, Args &&...) noexcept(Noexcept);
};

} // namespace detail

template<typename R, typename... Args>
class function_ref<R(Args...)>
    : public detail::function_ref_base<R, false, false, Args...>
{
    using base = detail::function_ref_base<R, false, false, Args...>;
public:
    using base::base;
    using base::operator=;
};

template<typename R, typename... Args>
class function_ref<R(Args...) const>
    : public detail::function_ref_base<R, true, false, Args...>
{
    using base = detail::function_ref_base<R, true, false, Args...>;
public:
    using base::base;
    using base::operator=;
};

template<typename R, typename... Args>
class function_ref<R(Args...) noexcept>
    : public detail::function_ref_base<R, false, true, Args...>
{
    using base = detail::function_ref_base<R, false, true, Args...>;
public:
    using base::base;
    using base::operator=;
};

template<typename R, typename... Args>
class function_ref<R(Args...) const noexcept>
    : public detail::function_ref_base<R, true, true, Args...>
{
    using base = detail::function_ref_base<R, true, true, Args...>;
public:
    using base::base;
    using base::operator=;
};

template<typename F>
    requires std::is_function_v<F>
function_ref(F *) -> function_ref<F>;

} // namespace nxt
