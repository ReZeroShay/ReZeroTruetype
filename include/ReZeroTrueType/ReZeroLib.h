#pragma once
#include <memory>
#include <cassert>
#include <concepts>
#include <limits>
#include <ReZeroTrueType/Allocator.h>

#if defined(_MSC_VER)
#    pragma warning(push)
#    pragma warning(disable : 4141)
#endif

#if defined(_MSC_VER)
#    define REZERO_COMPILER_MSVC 1
#    define REZERO_BREAK() __debugbreak()
#    define REZERO_INLINE inline __forceinline
#    define REZERO_NOINLINE __declspec(noinline)
#elif defined(__clang__)
#    define REZERO_COMPILER_CLANG 1
#    define REZERO_BREAK() __builtin_debugtrap()
#    define REZERO_INLINE inline __attribute__((always_inline))
#    define REZERO_NOINLINE __attribute__((noinline))
#elif defined(__GNUC__)
#    define REZERO_COMPILER_GCC 1
#    define REZERO_BREAK() __builtin_trap()
#    define REZERO_INLINE inline __attribute__((always_inline))
#    define REZERO_NOINLINE __attribute__((noinline))
#else
#    error "ReZeroTrueType: unsupported compiler"
#endif

#if !defined(REZERO_INLINE)
#    define REZERO_INLINE inline
#endif
#if !defined(REZERO_NOINLINE)
#    define REZERO_NOINLINE
#endif
#if !defined(REZERO_BREAK)
#    define REZERO_BREAK() ((void)0)
#endif

#define REZERO_NODISCARD [[nodiscard]]
#define REZERO_LIKELY(x) (!!(x))
#define REZERO_UNLIKELY(x) (!!(x))

#if !defined(REZERO_ENABLE_ASSERT)
#    if defined(NDEBUG)
#        define REZERO_ENABLE_ASSERT 0
#    else
#        define REZERO_ENABLE_ASSERT 1
#    endif
#endif

#if REZERO_ENABLE_ASSERT
#    define REZERO_ASSERT(expr) assert(expr)
#    define REZERO_ASSERT_MSG(expr, msg) assert((expr) && (msg))
#else
#    define REZERO_ASSERT(expr) ((void)0)
#    define REZERO_ASSERT_MSG(expr, msg) ((void)0)
#endif

#define REZERO_CHECK(expr)                                                                                             \
    do {                                                                                                               \
        auto &&checked_ = (expr);                                                                                      \
        if (!checked_)                                                                                                 \
            return Error{checked_.error()};                                                                            \
    } while (false)

#define REZERO_TRY(name, expr)                                                                                         \
    auto name##checked_ = (expr);                                                                                      \
    if (!name##checked_)                                                                                               \
        return Error{name##checked_.error()};                                                                          \
    auto name = *name##checked_;                                                                                       \
    (void)0

namespace ReZeroTrueType {

    template <typename T> constexpr const T &max(const T &a, const T &b) { return (a < b) ? b : a; }
    template <typename T> constexpr const T &min(const T &a, const T &b) { return (b < a) ? b : a; }

    constexpr auto memset(void *destination, std::uint8_t value, std::size_t count) noexcept -> void * {
        auto *bytes = static_cast<std::uint8_t *>(destination);

        for (std::size_t i = 0; i < count; ++i)
            bytes[i] = value;

        return destination;
    }

    constexpr auto memcpy(void *destination, const void *source, std::size_t count) noexcept -> void * {
        auto *dst = static_cast<std::uint8_t *>(destination);
        const auto *src = static_cast<const std::uint8_t *>(source);

        for (std::size_t i = 0; i < count; ++i)
            dst[i] = src[i];

        return destination;
    }

    [[nodiscard]] constexpr auto memcmp(const void *lhs, const void *rhs, std::size_t count) noexcept -> int {
        const auto *a = static_cast<const std::uint8_t *>(lhs);
        const auto *b = static_cast<const std::uint8_t *>(rhs);

        for (std::size_t i = 0; i < count; ++i) {
            if (a[i] < b[i])
                return -1;

            if (a[i] > b[i])
                return 1;
        }

        return 0;
    }

    template <typename E> struct Error {
        E error;

        constexpr explicit Error(const E &e) : error(e) {}
        constexpr explicit Error(E &&e) : error(std::move(e)) {}
    };

    template <typename E> Error(E) -> Error<E>;

    template <typename T, typename E> struct Result {
        using ValueType = T;
        using ErrorType = E;

        union Storage {
            T value;
            E error;

            constexpr Storage() noexcept {}
            constexpr ~Storage() noexcept {};
        };

        Storage storage;
        bool isOk{true};

        constexpr Result(const T &val) : storage{}, isOk(true) {
            std::construct_at(std::addressof(storage.value), val);
        }
        constexpr Result(T &&val) noexcept(std::is_nothrow_move_constructible_v<T>) : storage{}, isOk(true) {
            std::construct_at(std::addressof(storage.value), std::move(val));
        }

        constexpr Result(const Error<E> &err) : storage{}, isOk(false) {
            std::construct_at(std::addressof(storage.error), err.error);
        }

        constexpr Result(Error<E> &&err) noexcept(std::is_nothrow_move_constructible_v<E>) : storage{}, isOk(false) {
            std::construct_at(std::addressof(storage.error), std::move(err.error));
        }

        template <typename... Args>
        constexpr explicit Result(std::in_place_type_t<T>, Args &&...args) : storage{}, isOk(true) {
            std::construct_at(std::addressof(storage.value), std::forward<Args>(args)...);
        }

        template <typename... Args>
        constexpr explicit Result(std::in_place_type_t<E>, Args &&...args) : storage{}, isOk(false) {
            std::construct_at(std::addressof(storage.error), std::forward<Args>(args)...);
        }

        constexpr Result(const Result &other) : storage{}, isOk(other.isOk) {
            if (isOk) {
                std::construct_at(std::addressof(storage.value), other.storage.value);
            } else {
                std::construct_at(std::addressof(storage.error), other.storage.error);
            }
        }

        constexpr Result(Result &&other) noexcept(std::is_nothrow_move_constructible_v<T> &&
                                                  std::is_nothrow_move_constructible_v<E>)
            : storage{}, isOk(other.isOk) {
            if (isOk) {
                std::construct_at(std::addressof(storage.value), std::move(other.storage.value));
            } else {
                std::construct_at(std::addressof(storage.error), std::move(other.storage.error));
            }
        }

        constexpr ~Result() { destroy(); }

        constexpr void destroy() noexcept {
            if (isOk) {
                std::destroy_at(std::addressof(storage.value));
            } else {
                std::destroy_at(std::addressof(storage.error));
            }
        }

        constexpr Result &operator=(const Result &other) {
            if (this != std::addressof(other)) {
                destroy();
                isOk = other.isOk;
                if (isOk) {
                    std::construct_at(std::addressof(storage.value), other.storage.value);
                } else {
                    std::construct_at(std::addressof(storage.error), other.storage.error);
                }
            }
            return *this;
        }

        constexpr Result &operator=(Result &&other) noexcept(std::is_nothrow_move_constructible_v<T> &&
                                                             std::is_nothrow_move_constructible_v<E>) {
            if (this != std::addressof(other)) {
                destroy();
                isOk = other.isOk;
                if (isOk) {
                    std::construct_at(std::addressof(storage.value), std::move(other.storage.value));
                } else {
                    std::construct_at(std::addressof(storage.error), std::move(other.storage.error));
                }
            }
            return *this;
        }

        [[nodiscard]] constexpr bool is_ok() const noexcept { return isOk; }
        [[nodiscard]] constexpr bool is_err() const noexcept { return !isOk; }
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return isOk; }

        [[nodiscard]] constexpr T &value() & noexcept {
            REZERO_ASSERT(isOk);
            return storage.value;
        }
        [[nodiscard]] constexpr const T &value() const & noexcept {
            REZERO_ASSERT(isOk);
            return storage.value;
        }
        [[nodiscard]] constexpr T &&value() && noexcept {
            REZERO_ASSERT(isOk);
            return std::move(storage.value);
        }

        [[nodiscard]] constexpr T &operator*() & noexcept {
            REZERO_ASSERT(isOk);
            return storage.value;
        }
        [[nodiscard]] constexpr const T &operator*() const & noexcept {
            REZERO_ASSERT(isOk);
            return storage.value;
        }
        [[nodiscard]] constexpr T &&operator*() && noexcept {
            REZERO_ASSERT(isOk);
            return std::move(storage.value);
        }

        [[nodiscard]] constexpr T *operator->() noexcept {
            REZERO_ASSERT(isOk);
            return std::addressof(storage.value);
        }
        [[nodiscard]] constexpr const T *operator->() const noexcept {
            REZERO_ASSERT(isOk);
            return std::addressof(storage.value);
        }

        [[nodiscard]] constexpr E &error() & noexcept {
            REZERO_ASSERT(!isOk);
            return storage.error;
        }
        [[nodiscard]] constexpr const E &error() const & noexcept {
            REZERO_ASSERT(!isOk);
            return storage.error;
        }
        [[nodiscard]] constexpr E &&error() && noexcept {
            REZERO_ASSERT(!isOk);
            return std::move(storage.error);
        }

        template <typename U> constexpr T value_or(U &&default_value) const & {
            return isOk ? storage.value : static_cast<T>(std::forward<U>(default_value));
        }

        template <typename U> constexpr T value_or(U &&default_value) && {
            return isOk ? std::move(storage.value) : static_cast<T>(std::forward<U>(default_value));
        }
    };

    template <typename E> struct [[nodiscard]] Result<void, E> {
        using ValueType = void;
        using ErrorType = E;

        union Storage {
            unsigned char dummy; // 占位空字节
            E error;

            constexpr Storage() noexcept : dummy(0) {}
            constexpr ~Storage() noexcept {}
        };

        Storage storage;
        bool isOk{true};

        constexpr Result() noexcept : storage{}, isOk(true) {}

        constexpr Result(const Error<E> &err) : storage{}, isOk(false) {
            std::construct_at(std::addressof(storage.error), err.error);
        }

        constexpr Result(Error<E> &&err) noexcept(std::is_nothrow_move_constructible_v<E>) : storage{}, isOk(false) {
            std::construct_at(std::addressof(storage.error), std::move(err.error));
        }

        template <typename... Args>
        constexpr explicit Result(std::in_place_type_t<E>, Args &&...args) : storage{}, isOk(false) {
            std::construct_at(std::addressof(storage.error), std::forward<Args>(args)...);
        }

        constexpr Result(const Result &other) : storage{}, isOk(other.isOk) {
            if (!isOk) {
                std::construct_at(std::addressof(storage.error), other.storage.error);
            }
        }

        constexpr Result(Result &&other) noexcept(std::is_nothrow_move_constructible_v<E>)
            : storage{}, isOk(other.isOk) {
            if (!isOk) {
                std::construct_at(std::addressof(storage.error), std::move(other.storage.error));
            }
        }

        // 5. 析构与 Destroy
        constexpr ~Result() { destroy(); }

        constexpr void destroy() noexcept {
            if (!isOk) {
                std::destroy_at(std::addressof(storage.error));
            }
        }

        constexpr Result &operator=(const Result &other) {
            if (this != std::addressof(other)) {
                destroy();
                isOk = other.isOk;
                if (!isOk) {
                    std::construct_at(std::addressof(storage.error), other.storage.error);
                }
            }
            return *this;
        }

        constexpr Result &operator=(Result &&other) noexcept(std::is_nothrow_move_constructible_v<E>) {
            if (this != std::addressof(other)) {
                destroy();
                isOk = other.isOk;
                if (!isOk) {
                    std::construct_at(std::addressof(storage.error), std::move(other.storage.error));
                }
            }
            return *this;
        }

        [[nodiscard]] constexpr bool is_ok() const noexcept { return isOk; }
        [[nodiscard]] constexpr bool is_err() const noexcept { return !isOk; }
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return isOk; }

        constexpr void value() const noexcept { REZERO_ASSERT(isOk); }

        [[nodiscard]] constexpr E &error() & noexcept {
            REZERO_ASSERT(!isOk);
            return storage.error;
        }
        [[nodiscard]] constexpr const E &error() const & noexcept {
            REZERO_ASSERT(!isOk);
            return storage.error;
        }
        [[nodiscard]] constexpr E &&error() && noexcept {
            REZERO_ASSERT(!isOk);
            return std::move(storage.error);
        }
    };
    struct NoneType {
        explicit constexpr NoneType() noexcept = default;
    };

    inline constexpr NoneType None{};

    template <typename T> struct Option {
        union Storage {
            T value;

            constexpr Storage() noexcept {}
            ~Storage() noexcept {};
        };

        using ValueType = T;

        constexpr Option() noexcept : storage{}, hasValue(false) {}

        constexpr Option(NoneType) noexcept : storage{}, hasValue(false) {}

        template <typename... Args>
        constexpr explicit Option(std::in_place_t, Args &&...args) : storage{}, hasValue(true) {
            std::construct_at(std::addressof(storage.value), std::forward<Args>(args)...);
        }

        constexpr Option(const T &val) : storage{}, hasValue(true) {
            std::construct_at(std::addressof(storage.value), val);
        }

        constexpr Option(T &&val) noexcept(std::is_nothrow_move_constructible_v<T>) : storage{}, hasValue(true) {
            std::construct_at(std::addressof(storage.value), std::move(val));
        }
        constexpr ~Option() { reset(); }

        constexpr Option(const Option &other) : storage{}, hasValue(other.hasValue) {
            if (hasValue) {
                std::construct_at(std::addressof(storage.value), other.storage.value);
            }
        }

        constexpr Option(Option &&other) noexcept(std::is_nothrow_move_constructible_v<T>)
            : storage{}, hasValue(other.hasValue) {
            if (hasValue) {
                std::construct_at(std::addressof(storage.value), std::move(other.storage.value));
                other.reset();
            }
        }
        Option &operator=(NoneType) noexcept {
            reset();
            return *this;
        }

        constexpr Option &operator=(const Option &other) {
            if (this != std::addressof(other)) {
                if (other.hasValue) {
                    if (hasValue) {
                        storage.value = other.storage.value; // 已持有值时直接赋值
                    } else {
                        std::construct_at(std::addressof(storage.value), other.storage.value);
                        hasValue = true;
                    }
                } else {
                    reset();
                }
            }
            return *this;
        }

        constexpr Option &operator=(Option &&other) noexcept(std::is_nothrow_move_assignable_v<T> &&
                                                             std::is_nothrow_move_constructible_v<T>) {
            if (this != std::addressof(other)) {
                if (other.hasValue) {
                    if (hasValue) {
                        storage.value = std::move(other.storage.value);
                    } else {
                        std::construct_at(std::addressof(storage.value), std::move(other.storage.value));
                        hasValue = true;
                    }
                    other.reset();
                } else {
                    reset();
                }
            }
            return *this;
        }

        constexpr void reset() noexcept {
            if (hasValue) {
                std::destroy_at(std::addressof(storage.value)); // 替换显式析构调用 storage.value.~T()
                hasValue = false;
            }
        }

        [[nodiscard]] constexpr T &&value() && noexcept {
            REZERO_ASSERT(hasValue);
            return std::move(storage.value);
        }

        [[nodiscard]] constexpr T &value() & noexcept {
            REZERO_ASSERT(hasValue);
            return storage.value;
        }

        [[nodiscard]] constexpr const T &value() const & noexcept {
            REZERO_ASSERT(hasValue);
            return storage.value;
        }

        [[nodiscard]] constexpr bool has_value() const noexcept { return hasValue; }
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return hasValue; }

        [[nodiscard]] constexpr T &operator*() & noexcept {
            REZERO_ASSERT(hasValue);
            return storage.value;
        }
        [[nodiscard]] constexpr const T &operator*() const & noexcept {
            REZERO_ASSERT(hasValue);
            return storage.value;
        }
        [[nodiscard]] constexpr T &&operator*() && noexcept {
            REZERO_ASSERT(hasValue);
            return std::move(storage.value);
        }
        [[nodiscard]] constexpr T *operator->() noexcept {
            REZERO_ASSERT(hasValue);
            return std::addressof(storage.value);
        }
        [[nodiscard]] constexpr const T *operator->() const noexcept {
            REZERO_ASSERT(hasValue);
            return std::addressof(storage.value);
        }

        Storage storage;
        bool hasValue{false};
    };

    template <class Char> struct BasicStringView {
        static_assert(std::is_trivial_v<Char>);

        using value_type = Char;
        using size_type = std::size_t;
        using difference_type = std::ptrdiff_t;

        using const_pointer = const Char *;
        using const_reference = const Char &;
        using const_iterator = const Char *;

        static constexpr size_type npos = static_cast<size_type>(-1);

      public:
        constexpr BasicStringView() noexcept = default;

        constexpr BasicStringView(const Char *chars, size_type length) noexcept : first(chars), length(length) {
            REZERO_ASSERT(chars != nullptr || length == 0);
        }

        constexpr BasicStringView(const Char *chars) noexcept : first(chars), length(chars ? cstrLength(chars) : 0) {}

        template <size_type N> constexpr BasicStringView(const Char (&str)[N]) noexcept : first(str), length(N - 1) {}

      public:
        [[nodiscard]] constexpr auto data() const noexcept -> const Char * { return first; }

        [[nodiscard]] constexpr auto size() const noexcept -> size_type { return length; }

        [[nodiscard]] constexpr auto empty() const noexcept -> bool { return length == 0; }

        [[nodiscard]] constexpr auto front() const noexcept -> const Char & {
            REZERO_ASSERT(length > 0);
            return first[0];
        }

        [[nodiscard]] constexpr auto back() const noexcept -> const Char & {
            REZERO_ASSERT(length > 0);
            return first[length - 1];
        }

        [[nodiscard]] constexpr auto operator[](size_type index) const noexcept -> const Char & {
            REZERO_ASSERT(index < length);
            return first[index];
        }

        [[nodiscard]] constexpr auto at(size_type index) const noexcept -> const Char & {
            REZERO_ASSERT(index < length);
            return first[index];
        }

      public:
        [[nodiscard]] constexpr auto begin() const noexcept -> const_iterator { return first; }

        [[nodiscard]] constexpr auto end() const noexcept -> const_iterator { return first + length; }

      public:
        constexpr auto removePrefix(size_type count) noexcept {
            REZERO_ASSERT(count <= length);

            first += count;
            length -= count;
        }

        constexpr auto removeSuffix(size_type count) noexcept {
            REZERO_ASSERT(count <= length);

            length -= count;
        }

        [[nodiscard]] constexpr auto substr(size_type position, size_type count = npos) const noexcept
            -> BasicStringView {
            REZERO_ASSERT(position <= length);

            const size_type remaining = length - position;

            if (count > remaining) {
                count = remaining;
            }

            return BasicStringView(first + position, count);
        }

      public:
        [[nodiscard]] constexpr auto startsWith(BasicStringView value) const noexcept -> bool {
            if (value.length > length) {
                return false;
            }

            return equals(first, value.first, value.length);
        }

        [[nodiscard]] constexpr auto endsWith(BasicStringView value) const noexcept -> bool {
            if (value.length > length) {
                return false;
            }

            return equals(first + length - value.length, value.first, value.length);
        }

        [[nodiscard]] constexpr auto operator==(BasicStringView other) const noexcept -> bool {
            return length == other.length && equals(first, other.first, length);
        }

        [[nodiscard]] constexpr auto operator!=(BasicStringView other) const noexcept -> bool {
            return !(*this == other);
        }

      public:
        [[nodiscard]] constexpr auto find(Char value, size_type position = 0) const noexcept -> size_type {
            if (position >= length) {
                return npos;
            }

            for (size_type i = position; i < length; ++i) {
                if (first[i] == value) {
                    return i;
                }
            }

            return npos;
        }

        [[nodiscard]] constexpr auto find(BasicStringView value, size_type position = 0) const noexcept -> size_type {
            if (position > length) {
                return npos;
            }

            if (value.empty()) {
                return position;
            }

            if (value.length > length - position) {
                return npos;
            }

            const size_type last = length - value.length;

            for (size_type i = position; i <= last; ++i) {
                if (equals(first + i, value.first, value.length)) {
                    return i;
                }
            }

            return npos;
        }

        [[nodiscard]] constexpr auto findFirstOf(BasicStringView values, size_type position = 0) const noexcept
            -> size_type {
            if (position >= length) {
                return npos;
            }

            for (size_type i = position; i < length; ++i) {
                for (size_type j = 0; j < values.length; ++j) {
                    if (first[i] == values[j]) {
                        return i;
                    }
                }
            }

            return npos;
        }

      private:
        [[nodiscard]] static constexpr auto cstrLength(const Char *str) noexcept -> size_type {
            size_type length = 0;

            while (str[length] != Char{}) {
                ++length;
            }

            return length;
        }

        [[nodiscard]] static constexpr auto equals(const Char *lhs, const Char *rhs, size_type size) noexcept -> bool {
            for (size_type i = 0; i < size; ++i) {
                if (lhs[i] != rhs[i]) {
                    return false;
                }
            }

            return true;
        }

      private:
        const Char *first = nullptr;
        size_type length = 0;
    };
    using StringView = BasicStringView<char>;
    using u8StringView = BasicStringView<char8_t>;

    template <typename T> struct Span {
        using element_type = T;
        using value_type = std::remove_cv_t<T>;
        using size_type = std::size_t;
        using difference_type = std::ptrdiff_t;
        using pointer = T *;
        using const_pointer = const T *;
        using reference = T &;
        using const_reference = const T &;
        using iterator = pointer;
        using const_iterator = const_pointer;
        using reverse_iterator = std::reverse_iterator<iterator>;
        using const_reverse_iterator = std::reverse_iterator<const_iterator>;

        constexpr Span() noexcept = default;
        constexpr Span(pointer ptr, size_type length) : ptr(ptr), length(length) {}

        template <size_t N> constexpr Span(T (&array)[N]) : ptr(array), length(N) {}

        template <typename U>
            requires std::convertible_to<U (*)[], element_type (*)[]>
        constexpr Span(const Span<U> &other) noexcept : ptr(other.data()), length(other.size()) {}

        constexpr Span(const Span &) noexcept = default;
        constexpr Span &operator=(const Span &) noexcept = default;

        constexpr iterator begin() const noexcept { return ptr; }
        constexpr const_iterator cbegin() const noexcept { return ptr; }
        constexpr iterator end() const noexcept { return ptr + length; }
        constexpr const_iterator cend() const noexcept { return ptr + length; }

        constexpr reverse_iterator rbegin() const noexcept { return reverse_iterator(end()); }
        constexpr const_reverse_iterator crbegin() const noexcept { return const_reverse_iterator(cend()); }
        constexpr reverse_iterator rend() const noexcept { return reverse_iterator(begin()); }
        constexpr const_reverse_iterator crend() const noexcept { return const_reverse_iterator(cbegin()); }

        constexpr reference operator[](size_type idx) const noexcept {
            REZERO_ASSERT(idx < length);
            return ptr[idx];
        }

        constexpr reference front() const noexcept {
            REZERO_ASSERT(!empty());
            return ptr[0];
        }

        constexpr reference back() const noexcept {
            REZERO_ASSERT(!empty());
            return ptr[length - 1];
        }

        constexpr pointer data() const noexcept { return ptr; }

        constexpr size_type size() const noexcept { return length; }
        constexpr size_type size_bytes() const noexcept { return length * sizeof(element_type); }
        [[nodiscard]] constexpr bool empty() const noexcept { return length == 0; }

        constexpr Span first(size_type size) const noexcept {
            REZERO_ASSERT(size <= length);
            return {ptr, size};
        }

        constexpr Span last(size_type size) const noexcept {
            REZERO_ASSERT(size <= length);
            return {ptr + (length - size), size};
        }

        constexpr Span subspan(size_type offset, size_type size = size_type(-1)) const noexcept {
            REZERO_ASSERT(offset <= length);
            if (size == size_type(-1)) {
                return {ptr + offset, length - offset};
            }
            REZERO_ASSERT(size <= length - offset);
            return {ptr + offset, size};
        }

      private:
        pointer ptr = nullptr;
        size_type length = 0;
    };

    template <typename T, std::size_t N> Span(T (&)[N]) -> Span<T>;

    /// T must be trivially copyable.
    template <class T>
        requires std::is_trivially_copyable_v<T>
    struct ArrayList {
        using value_type = T;
        using size_type = std::size_t;
        using reference = T &;
        using const_reference = const T &;
        using pointer = T *;
        using const_pointer = const T *;
        using iterator = T *;
        using const_iterator = const T *;

        explicit ArrayList(AllocatorView allocator) noexcept : Allocator(allocator) {}

        ArrayList(AllocatorView allocator, size_type initialCapacity) noexcept : Allocator(allocator) {
            reserve(initialCapacity);
        }
        ~ArrayList() noexcept { release(); }
        ArrayList(const ArrayList &) = delete;
        ArrayList &operator=(const ArrayList &) = delete;

        ArrayList(ArrayList &&other) noexcept
            : Allocator(other.Allocator), Items(other.Items), Count(other.Count), Capacity(other.Capacity) {
            other.Items = nullptr;
            other.Count = 0;
            other.Capacity = 0;
        }

        ArrayList &operator=(ArrayList &&other) noexcept {
            if (this == &other)
                return *this;

            release();

            Allocator = other.Allocator;
            Items = other.Items;
            Count = other.Count;
            Capacity = other.Capacity;

            other.Items = nullptr;
            other.Count = 0;
            other.Capacity = 0;

            return *this;
        }

        [[nodiscard]] constexpr auto span() noexcept -> Span<T> { return {Items, Count}; }

        [[nodiscard]] constexpr auto span() const noexcept -> Span<const T> { return {Items, Count}; }
        [[nodiscard]] bool empty() const noexcept { return Count == 0; }
        [[nodiscard]] size_type size() const noexcept { return Count; }
        [[nodiscard]] T *data() noexcept { return Items; }
        [[nodiscard]] const T *data() const noexcept { return Items; }
        T &operator[](size_type index) noexcept { return Items[index]; }
        const T &operator[](size_type index) const noexcept { return Items[index]; }
        T &front() noexcept { return Items[0]; }
        const T &front() const noexcept { return Items[0]; }
        T &back() noexcept { return Items[Count - 1]; }
        const T &back() const noexcept { return Items[Count - 1]; }

        T *begin() noexcept { return Items; }

        T *end() noexcept { return Items + Count; }

        const T *begin() const noexcept { return Items; }

        const T *end() const noexcept { return Items + Count; }

        bool reserve(size_type newCapacity) noexcept {
            if (newCapacity <= Capacity)
                return true;

            return grow(newCapacity);
        }

        bool push_back(const T &value) noexcept {
            if (Count == Capacity) {
                if (!grow(nextCapacity()))
                    return false;
            }
            std::construct_at(Items + Count, value);
            Count += 1;

            return true;
        }

        bool resize(size_type newSize) noexcept {
            if (newSize > Capacity) {
                if (!grow(newSize))
                    return false;
            }

            if (newSize > Count) {
                for (size_type i = Count; i < newSize; ++i) {
                    std::construct_at(Items + i);
                }
            }

            Count = newSize;
            return true;
        }

        void pop_back() noexcept {
            if (Count != 0)
                Count -= 1;
        }

        void clear() noexcept { Count = 0; }

        [[nodiscard]] size_type nextCapacity() const noexcept {
            if (Capacity == 0)
                return 8;

            constexpr size_type Max = std::numeric_limits<size_type>::max();

            if (Capacity > Max / 2)
                return Max / sizeof(T);

            return Capacity * 2;
        }

        [[nodiscard]] bool grow(size_type newCapacity) noexcept {
            if (newCapacity <= Capacity)
                return true;

            constexpr size_type Max = std::numeric_limits<size_type>::max();

            if (newCapacity > Max / sizeof(T))
                return false;

            const size_type newSize = newCapacity * sizeof(T);

            void *newMemory = Allocator.allocate(newSize, alignof(T));

            if (!newMemory)
                return false;

            T *newItems = static_cast<T *>(newMemory);

            for (size_type i = 0; i < Count; ++i) {
                std::construct_at(newItems + i, Items[i]);
            }

            if (Items) {
                Allocator.deallocate(Items);
            }

            Items = newItems;
            Capacity = newCapacity;

            return true;
        }

        void release() noexcept {
            if (!Items)
                return;

            Allocator.deallocate(Items);

            Items = nullptr;
            Count = 0;
            Capacity = 0;
        }

        AllocatorView Allocator;
        T *Items = nullptr;
        size_type Count = 0;
        size_type Capacity = 0;
    };
    /// A byte buffer that owns its memory.
    ///
    /// The memory is allocated and freed by the allocator.
    /// The allocator must stay alive while the buffer exists.
    struct OwnedBytes {
      private:
        static constexpr std::size_t DefaultAlignment = alignof(std::max_align_t);

      public:
        OwnedBytes(AllocatorView allocator) : allocator(allocator) {}

        OwnedBytes(OwnedBytes &&other) noexcept : allocator(other.allocator), ptr(other.ptr), length(other.length) {
            other.detach();
        }

        OwnedBytes &operator=(OwnedBytes &&other) noexcept {
            if (this != &other) {
                reset();
                allocator = other.allocator;
                ptr = other.ptr;
                length = other.length;
                other.detach();
            }
            return *this;
        }

        OwnedBytes(const OwnedBytes &) = delete;
        OwnedBytes &operator=(const OwnedBytes &) = delete;

        ~OwnedBytes() noexcept { reset(); }

        /// Allocates `size` bytes with `alignment` from `allocator`.
        [[nodiscard]] static auto create(AllocatorView allocator, std::size_t size,
                                         std::size_t alignment = DefaultAlignment) noexcept -> OwnedBytes {
            void *const ptr = allocator.allocate(size, alignment);
            REZERO_ASSERT(ptr != nullptr);
            return OwnedBytes(allocator, static_cast<std::uint8_t *>(ptr), size);
        }

        void reset() noexcept {
            if (ptr != nullptr)
                allocator.deallocate(ptr);
            detach();
        }
        [[nodiscard]] std::uint8_t *data() noexcept { return ptr; }
        [[nodiscard]] const std::uint8_t *data() const noexcept { return ptr; }

        [[nodiscard]] std::size_t size() const noexcept { return length; }
        [[nodiscard]] bool valid() const noexcept { return ptr != nullptr; }

        [[nodiscard]] Span<std::uint8_t> bytes() noexcept { return Span<std::uint8_t>(ptr, length); }
        [[nodiscard]] Span<const std::uint8_t> bytes() const noexcept { return Span<const std::uint8_t>(ptr, length); }

      private:
        OwnedBytes(AllocatorView allocator, std::uint8_t *data, std::size_t size) noexcept
            : allocator(allocator), ptr(data), length(size) {}

        void detach() noexcept {
            ptr = nullptr;
            length = 0;
        }

        AllocatorView allocator;
        std::uint8_t *ptr = nullptr;
        std::size_t length = 0;
    };

    namespace unicode {

        /// 从 `offset` 处解出一个 UTF-8 码点，并把 `offset` 推到下一个字符。
        ///
        /// UTF-8 的编码方式（第一个字节的高位决定长度）：
        ///
        ///     0xxxxxxx                              1 字节，码点 7 位
        ///     110xxxxx 10xxxxxx                     2 字节，码点 11 位
        ///     1110xxxx 10xxxxxx 10xxxxxx            3 字节，码点 16 位
        ///     11110xxx 10xxxxxx 10xxxxxx 10xxxxxx   4 字节，码点 21 位
        ///
        /// 解不出（越界或续字节格式不对）就返回空的 Option，offset 不动。
        ///
        /// @brief Decode one UTF-8 code point and update the offset
        ///
        /// @param text UTF-8 string
        /// @param offset current offset
        /// @return Option<uint32_t>
        inline auto decodeUtf8(u8StringView text, std::size_t &offset) -> Option<uint32_t> {
            if (offset >= text.size())
                return None;

            uint8_t b0 = static_cast<uint8_t>(text[offset]);
            if (b0 < 0x80) {
                offset += 1;
                return static_cast<uint32_t>(b0);
            }

            std::size_t length{0};
            uint32_t codepoint{0};
            if ((b0 & 0xE0) == 0xC0) {
                length = 2;
                codepoint = b0 & 0x1Fu;
            } else if ((b0 & 0xF0) == 0xE0) {
                length = 3;
                codepoint = b0 & 0x0Fu;
            } else if ((b0 & 0xF8) == 0xF0) {
                length = 4;
                codepoint = b0 & 0x07u;
            } else {
                // invalid lead byte
                return None;
            }

            if (offset + length > text.size())
                return None;

            for (std::size_t i = 1; i < length; ++i) {
                uint8_t b = static_cast<uint8_t>(text[offset + i]);
                if ((b & 0xC0) != 0x80)
                    // invalid continuation byte
                    return None;
                codepoint = (codepoint << 6) | (b & 0x3Fu);
            }

            // Reject invalid code points(overlong encoding)
            // - 0x80: minimum value for a 2-byte UTF-8 sequence.
            // - 0x800: minimum value for a 3-byte UTF-8 sequence.
            // - 0x10000: minimum value for a 4-byte UTF-8 sequence.
            // - 0x10FFFF: maximum valid Unicode code point.
            // - 0xD800-0xDFFF: UTF-16 surrogate range.
            bool overlong = (length == 2 && codepoint < 0x80) || (length == 3 && codepoint < 0x800) ||
                            (length == 4 && codepoint < 0x10000);
            if (overlong || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF))
                // invalid code point
                return None;

            offset += length;
            return codepoint;
        }
    } // namespace unicode

    namespace misc {
        REZERO_INLINE auto loadBE16(const uint8_t *ptr) -> uint16_t {
            return static_cast<uint16_t>((static_cast<uint16_t>(ptr[0]) << 8) | static_cast<uint16_t>(ptr[1]));
        }

        REZERO_INLINE auto loadBE32(const uint8_t *ptr) -> uint32_t {
            return (static_cast<uint32_t>(ptr[0]) << 24) | (static_cast<uint32_t>(ptr[1]) << 16) |
                   (static_cast<uint32_t>(ptr[2]) << 8) | static_cast<uint32_t>(ptr[3]);
        }

        REZERO_INLINE auto loadBE64(const uint8_t *ptr) -> uint64_t {
            uint64_t hi = static_cast<uint64_t>(loadBE32(ptr));
            uint64_t lo = static_cast<uint64_t>(loadBE32(ptr + 4));
            return (hi << 32) | lo;
        }
    } // namespace misc

    template <class T, class LessThan> constexpr auto insertionSort(Span<T> items, LessThan less) noexcept -> void {
        for (std::size_t i = 1; i < items.size(); ++i) {
            T value = std::move(items[i]);

            std::size_t j = i;

            while (j > 0 && less(value, items[j - 1])) {
                items[j] = std::move(items[j - 1]);
                --j;
            }

            items[j] = std::move(value);
        }
    }

} // namespace ReZeroTrueType
