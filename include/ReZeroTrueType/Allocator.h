#pragma once
#include <concepts>
#include <cstddef>
#include <memory>
#include <type_traits>

namespace ReZeroTrueType {

    template <class A>
    concept Allocator = requires(A &a, std::size_t size, std::size_t alignment) {
        { a.allocate(size, alignment) } noexcept -> std::same_as<void *>;
    };

    template <class A>
    concept DeallocatingAllocator = Allocator<A> && requires(A &a, void *ptr) {
        { a.deallocate(ptr) } noexcept;
    };

    /// A non-owning, type-erased view of an allocator.
    struct AllocatorView {
      private:
        struct VTable {
            auto (*allocate)(void *self, std::size_t size, std::size_t alignment) noexcept -> void *;
            auto (*deallocate)(void *self, void *ptr) noexcept -> void;
        };
        // Type Erasure
        template <DeallocatingAllocator A> static const VTable &toVTable() noexcept {
            static constexpr VTable vt = {
                .allocate = [](void *self, std::size_t size, std::size_t alignment) noexcept -> void * {
                    return static_cast<A *>(self)->allocate(size, alignment);
                },
                .deallocate = [](void *self, void *ptr) noexcept { static_cast<A *>(self)->deallocate(ptr); }};
            return vt;
        }

      public:
        AllocatorView() = delete;

        // Do not allow AllocatorView to wrap another AllocatorView.
        template <DeallocatingAllocator A>
            requires(!std::same_as<std::remove_cvref_t<A>, AllocatorView>)
        explicit AllocatorView(A &allocator) : self(std::addressof(allocator)), vt(&toVTable<A>()) {}
        auto allocate(std::size_t size, std::size_t alignment) const noexcept -> void * {
            return vt->allocate(self, size, alignment);
        }
        auto deallocate(void *ptr) const noexcept -> void { vt->deallocate(self, ptr); }

        template <typename T, typename... Args>
            requires std::is_nothrow_constructible_v<T, Args...>
        auto construct(Args... args) noexcept -> T * {
            auto *ptr = allocate(sizeof(T), alignof(T));
            if (ptr == nullptr) {
                return nullptr;
            }
            return std::construct_at(static_cast<T *>(ptr), std::forward<Args>(args)...);
        }

        template <typename T> void destory(T *ptr) noexcept {
            if (ptr == nullptr) {
                return;
            }
            std::destroy_at(ptr);
            deallocate(ptr);
        }

      private:
        void *self;
        const VTable *vt;
    };
}; // namespace ReZeroTrueType
