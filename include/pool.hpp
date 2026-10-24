#pragma once
#include <cstddef>
#include <new>
#include <stdexcept>

// HW4 — a high-performance fixed-size object pool (O(1) alloc/free, placement new).
//
// One buffer is allocated up front and cut into `capacity` equal slots. Free
// slots form an intrusive singly linked list: the first bytes of a free slot
// hold the pointer to the next free slot, so the list costs no extra memory.
//   alloc(): pop the head of the list   -> O(1), nullptr when empty
//   free():  push the slot back on top  -> O(1), LIFO (the hottest slot is reused first)
// Single-threaded. Not copyable (it owns the buffer), but movable.
struct Pool {
    Pool(std::size_t obj_size, std::size_t capacity)
        : slot_size_(round_up(obj_size < sizeof(void*) ? sizeof(void*) : obj_size, kSlotAlign)),
          capacity_(capacity) {
        if (capacity_ != 0 && slot_size_ > static_cast<std::size_t>(-1) / capacity_)
            throw std::length_error("Pool: obj_size * capacity overflows");
        buf_ = static_cast<char*>(::operator new(slot_size_ * capacity_, std::align_val_t{kBufAlign}));
        // Thread every slot onto the free list in address order: slot i -> slot i+1.
        for (std::size_t i = 0; i < capacity_; ++i) {
            char* slot = buf_ + i * slot_size_;
            *reinterpret_cast<void**>(slot) = (i + 1 < capacity_) ? slot + slot_size_ : nullptr;
        }
        head_ = capacity_ ? buf_ : nullptr;
    }

    ~Pool() { ::operator delete(buf_, std::align_val_t{kBufAlign}); }

    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;
    Pool(Pool&& o) noexcept
        : buf_(o.buf_), head_(o.head_), slot_size_(o.slot_size_), capacity_(o.capacity_) {
        o.buf_ = nullptr; o.head_ = nullptr; o.capacity_ = 0;
    }
    Pool& operator=(Pool&&) = delete;

    void* alloc() noexcept {
        void* p = head_;
        if (p) head_ = *static_cast<void**>(p);
        return p;
    }

    // Precondition: p came from this pool's alloc() and is not already free.
    // free(nullptr) is a no-op, like std::free.
    void free(void* p) noexcept {
        if (!p) return;
        *static_cast<void**>(p) = head_;
        head_ = p;
    }

    bool owns(const void* p) const noexcept {
        const char* c = static_cast<const char*>(p);
        return c >= buf_ && c < buf_ + slot_size_ * capacity_ &&
               static_cast<std::size_t>(c - buf_) % slot_size_ == 0;
    }
    std::size_t slot_size() const noexcept { return slot_size_; }
    std::size_t capacity() const noexcept { return capacity_; }

private:
    // Every slot satisfies the same alignment malloc guarantees, so any object
    // that fits can be placement-new'd into it. The buffer starts on a cache line.
    static constexpr std::size_t kSlotAlign = alignof(std::max_align_t);
    static constexpr std::size_t kBufAlign  = 64;
    static constexpr std::size_t round_up(std::size_t n, std::size_t a) { return (n + a - 1) / a * a; }

    char*       buf_  = nullptr;
    void*       head_ = nullptr;
    std::size_t slot_size_;
    std::size_t capacity_;
};
