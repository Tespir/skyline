// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <unordered_set>
#include <boost/functional/hash.hpp>
#include <nce.h>
#include "memory_manager.h"

namespace skyline::gpu {
    using GuestBuffer = span<u8>; //!< The CPU mapping for the guest buffer, multiple mappings for buffers aren't supported since overlaps cannot be reconciled

    struct BufferView;
    class BufferManager;
    class MegaBuffer;

    /**
     * @brief A buffer which is backed by host constructs while being synchronized with the underlying guest buffer
     * @note This class conforms to the Lockable and BasicLockable C++ named requirements
     */
    class Buffer : public std::enable_shared_from_this<Buffer>, public FenceCycleDependency {
      private:
        GPU &gpu;
        std::mutex mutex; //!< Synchronizes any mutations to the buffer or its backing
        memory::Buffer backing;
        std::optional<GuestBuffer> guest;

        span<u8> mirror{}; //!< A contiguous mirror of all the guest mappings to allow linear access on the CPU
        span<u8> alignedMirror{}; //!< The mirror mapping aligned to page size to reflect the full mapping
        std::optional<nce::NCE::TrapHandle> trapHandle{}; //!< The handle of the traps for the guest mappings
        enum class DirtyState {
            Clean, //!< The CPU mappings are in sync with the GPU buffer
            CpuDirty, //!< The CPU mappings have been modified but the GPU buffer is not up to date
            GpuDirty, //!< The GPU buffer has been modified but the CPU mappings have not been updated
        } dirtyState{DirtyState::CpuDirty}; //!< The state of the CPU mappings with respect to the GPU buffer

        bool everHadInlineUpdate{}; //!< Whether the buffer has ever had an inline update since it was created, if this is set then megabuffering will be attempted by views to avoid the cost of inline GPU updates

        std::shared_ptr<FenceCycle> hostImmutableCycle; //!< The cycle for when the buffer was last immutable, if this is signalled the buffer is no longer immutable

        /**
         * @return If the buffer should be treated as host immutable
         */
        bool CheckHostImmutable();

      public:
        /**
         * @brief Storage for all metadata about a specific view into the buffer, used to prevent redundant view creation and duplication of VkBufferView(s)
         */
        struct BufferViewStorage {
            vk::DeviceSize offset;
            vk::DeviceSize size;
            vk::Format format;

            // These are not accounted for in hash nor operator== since they are not an inherent property of the view, but they are required nonetheless for megabuffering on a per-view basis
            mutable u64 lastAcquiredSequence{}; //!< The last sequence number for the attached buffer that the megabuffer copy of this view was acquired from, if this is equal to the current sequence of the attached buffer then the copy at `megabufferOffset` is still valid
            mutable vk::DeviceSize megabufferOffset{}; //!< Offset of the current copy of the view in the megabuffer (if any), 0 if no copy exists and this is only valid if `lastAcquiredSequence` is equal to the current sequence of the attached buffer

            BufferViewStorage(vk::DeviceSize offset, vk::DeviceSize size, vk::Format format);

            bool operator==(const BufferViewStorage &other) const {
                return other.offset == offset && other.size == size && other.format == format;
            }
        };

        static constexpr u64 InitialSequenceNumber{1}; //!< Sequence number that all buffers start off with

      private:
        /**
         * @brief Hash function for BufferViewStorage to be used in the views set
         */
        struct BufferViewStorageHash {
            size_t operator()(const BufferViewStorage &entry) const noexcept {
                size_t seed{};
                boost::hash_combine(seed, entry.offset);
                boost::hash_combine(seed, entry.size);
                boost::hash_combine(seed, entry.format);

                // The mutable fields {lastAcquiredSequence, megabufferOffset} are deliberately ignored
                return seed;
            }
        };

        std::unordered_set<BufferViewStorage, BufferViewStorageHash> views; //!< BufferViewStorage(s) that are backed by this Buffer, used for storage and repointing to a new Buffer on deletion

        u64 sequenceNumber{InitialSequenceNumber}; //!< Sequence number that is incremented after all modifications to the host side `backing` buffer, used to prevent redundant copies of the buffer being stored in the megabuffer by views

      public:
        /**
         * @brief A delegate for a strong reference to a Buffer by a BufferView which can be changed to another Buffer transparently
         * @note This class conforms to the Lockable and BasicLockable C++ named requirements
         */
        struct BufferDelegate : public FenceCycleDependency {
            std::shared_ptr<Buffer> buffer;
            const Buffer::BufferViewStorage *view;
            std::function<void(const BufferViewStorage &, const std::shared_ptr<Buffer> &)> usageCallback;
            std::list<BufferDelegate *>::iterator iterator;

            BufferDelegate(std::shared_ptr<Buffer> buffer, const Buffer::BufferViewStorage *view);

            ~BufferDelegate();

            void lock();

            void unlock();

            bool try_lock();
        };

      private:
        std::list<BufferDelegate *> delegates; //!< The reference delegates for this buffer, used to prevent the buffer from being deleted while it is still in use

        friend BufferView;
        friend BufferManager;

        /**
         * @brief Sets up mirror mappings for the guest mappings
         */
        void SetupGuestMappings();

      public:
        std::weak_ptr<FenceCycle> cycle; //!< A fence cycle for when any host operation mutating the buffer has completed, it must be waited on prior to any mutations to the backing

        constexpr vk::Buffer GetBacking() {
            return backing.vkBuffer;
        }

        /**
         * @return A span of the backing of this buffer
         * @note This operation **must** be performed only on host-only buffers since synchronization is handled internally for guest-backed buffers
         */
        span<u8> GetBackingSpan() {
            if (guest)
                throw exception("Attempted to get a span of a guest-backed buffer");
            return span<u8>(backing);
        }

        Buffer(GPU &gpu, GuestBuffer guest);

        /**
         * @brief Creates a Buffer that is pre-synchronised with the contents of the input buffers
         * @param pCycle The FenceCycle associated with the current workload, utilised for synchronising GPU dirty buffers
         * @param srcBuffers Span of overlapping source buffers
         */
        Buffer(GPU &gpu, const std::shared_ptr<FenceCycle> &pCycle, GuestBuffer guest, span<std::shared_ptr<Buffer>> srcBuffers);

        /**
         * @brief Creates a host-only Buffer which isn't backed by any guest buffer
         * @note The created buffer won't have a mirror so any operations cannot depend on a mirror existing
         */
        Buffer(GPU &gpu, vk::DeviceSize size);

        ~Buffer();

        /**
         * @brief Acquires an exclusive lock on the buffer for the calling thread
         * @note Naming is in accordance to the BasicLockable named requirement
         */
        void lock() {
            mutex.lock();
        }

        /**
         * @brief Relinquishes an existing lock on the buffer by the calling thread
         * @note Naming is in accordance to the BasicLockable named requirement
         */
        void unlock() {
            mutex.unlock();
        }

        /**
         * @brief Attempts to acquire an exclusive lock but returns immediately if it's captured by another thread
         * @note Naming is in accordance to the Lockable named requirement
         */
        bool try_lock() {
            return mutex.try_lock();
        }

        /**
         * @brief Marks the buffer as dirty on the GPU, it will be synced on the next call to SynchronizeGuest
         * @note This **must** be called after syncing the buffer to the GPU not before
         * @note The buffer **must** be locked prior to calling this
         */
        void MarkGpuDirty();

        /**
         * @brief Waits on a fence cycle if it exists till it's signalled and resets it after
         * @note The buffer **must** be locked prior to calling this
         */
        void WaitOnFence();

        /**
         * @brief Polls a fence cycle if it exists and resets it if signalled
         * @return Whether the fence cycle was signalled
         * @note The buffer **must** be locked prior to calling this
         */
        bool PollFence();

        /**
         * @brief Synchronizes the host buffer with the guest
         * @param rwTrap If true, the guest buffer will be read/write trapped rather than only being write trapped which is more efficient than calling MarkGpuDirty directly after
         * @note The buffer **must** be locked prior to calling this
         */
        void SynchronizeHost(bool rwTrap = false);

        /**
         * @brief Synchronizes the host buffer with the guest
         * @param cycle A FenceCycle that is checked against the held one to skip waiting on it when equal
         * @param rwTrap If true, the guest buffer will be read/write trapped rather than only being write trapped which is more efficient than calling MarkGpuDirty directly after
         * @note The buffer **must** be locked prior to calling this
         */
        void SynchronizeHostWithCycle(const std::shared_ptr<FenceCycle> &cycle, bool rwTrap = false);

        /**
         * @brief Synchronizes the guest buffer with the host buffer
         * @param skipTrap If true, setting up a CPU trap will be skipped and the dirty state will be Clean/CpuDirty
         * @param nonBlocking If true, the call will return immediately if the fence is not signalled, skipping the sync
         * @note The buffer **must** be locked prior to calling this
         */
        void SynchronizeGuest(bool skipTrap = false, bool nonBlocking = false);

        /**
         * @brief Synchronizes the guest buffer with the host buffer when the FenceCycle is signalled
         * @note The buffer **must** be locked prior to calling this
         * @note The guest buffer should not be null prior to calling this
         */
        void SynchronizeGuestWithCycle(const std::shared_ptr<FenceCycle> &cycle);

        /**
         * @brief Synchronizes the guest buffer with the host buffer immediately, flushing GPU work if necessary
         * @note The buffer **must** be locked prior to calling this
         * @param pCycle The FenceCycle associated with the current workload, utilised for waiting and flushing semantics
         * @param flushHostCallback Callback to flush and execute all pending GPU work to allow for synchronisation of GPU dirty buffers
         */
        void SynchronizeGuestImmediate(const std::shared_ptr<FenceCycle> &pCycle, const std::function<void()> &flushHostCallback);

        /**
         * @brief Reads data at the specified offset in the buffer
         * @param pCycle The FenceCycle associated with the current workload, utilised for waiting and flushing semantics
         * @param flushHostCallback Callback to flush and execute all pending GPU work to allow for synchronisation of GPU dirty buffers
         */
        void Read(const std::shared_ptr<FenceCycle> &pCycle, const std::function<void()> &flushHostCallback, span<u8> data, vk::DeviceSize offset);

        /**
         * @brief Writes data at the specified offset in the buffer, falling back to GPU side copies if the buffer is host immutable
         * @param pCycle The FenceCycle associated with the current workload, utilised for waiting and flushing semantics
         * @param flushHostCallback Callback to flush and execute all pending GPU work to allow for synchronisation of GPU dirty buffers
         * @param gpuCopyCallback Callback to perform a GPU-side copy for this Write
         */
        void Write(const std::shared_ptr<FenceCycle> &pCycle, const std::function<void()> &flushHostCallback, const std::function<void()> &gpuCopyCallback, span<u8> data, vk::DeviceSize offset);

        /**
         * @return A cached or newly created view into this buffer with the supplied attributes
         * @note The buffer **must** be locked prior to calling this
         */
        BufferView GetView(vk::DeviceSize offset, vk::DeviceSize size, vk::Format format = {});

        /**
         * @brief Attempts to return the current sequence number and prepare the buffer for read accesses from the returned span
         * @return The current sequence number and a span of the buffers guest mirror given that the buffer is not GPU dirty, if it is then a zero sequence number is returned
         * @note The contents of the returned span can be cached safely given the sequence number is unchanged
         * @note The buffer **must** be locked prior to calling this
         * @note An implicit CPU -> GPU sync will be performed when calling this, an immediate GPU -> CPU sync will also be attempted if the buffer is GPU dirty
         */
        std::pair<u64, span<u8>> AcquireCurrentSequence();

        /**
         * @brief Increments the sequence number of the buffer, any futher calls to AcquireCurrentSequence will return this new sequence number. See the comment for `sequenceNumber`
         * @note The buffer **must** be locked prior to calling this
         * @note This **must** be called after any modifications of the backing buffer data (but not mirror)
         */
        void AdvanceSequence();

        /**
         * @param pCycle The FenceCycle associated with the current workload, utilised for waiting and flushing semantics
         * @param flushHostCallback Callback to flush and execute all pending GPU work to allow for synchronisation of GPU dirty buffers
         * @return A span of the backing buffer contents
         * @note The returned span **must** not be written to
         * @note The buffer **must** be kept locked until the span is no longer in use
         */
        span<u8> GetReadOnlyBackingSpan(const std::shared_ptr<FenceCycle> &pCycle, const std::function<void()> &flushHostCallback);

        /**
         * @brief Prevents any further writes to the `backing` host side buffer for the duration of the current cycle, forcing slower inline GPU updates instead
         * @note The buffer **must** be locked prior to calling this
         */
        void MarkHostImmutable(const std::shared_ptr<FenceCycle> &pCycle);

        bool EverHadInlineUpdate() const { return everHadInlineUpdate; }
    };

    /**
     * @brief A contiguous view into a Vulkan Buffer that represents a single guest buffer (as opposed to Buffer objects which contain multiple)
     * @note The object **must** be locked prior to accessing any members as values will be mutated
     * @note This class conforms to the Lockable and BasicLockable C++ named requirements
     */
    struct BufferView {
        constexpr static vk::DeviceSize MegaBufferingDisableThreshold{1024 * 128}; //!< The threshold at which the view is considered to be too large to be megabuffered (128KiB)

        std::shared_ptr<Buffer::BufferDelegate> bufferDelegate;

        BufferView(std::shared_ptr<Buffer> buffer, const Buffer::BufferViewStorage *view);

        constexpr BufferView(nullptr_t = nullptr) : bufferDelegate(nullptr) {}

        /**
         * @brief Acquires an exclusive lock on the buffer for the calling thread
         * @note Naming is in accordance to the BasicLockable named requirement
         */
        void lock() const {
            bufferDelegate->lock();
        }

        /**
         * @brief Relinquishes an existing lock on the buffer by the calling thread
         * @note Naming is in accordance to the BasicLockable named requirement
         */
        void unlock() const {
            bufferDelegate->unlock();
        }

        /**
         * @brief Attempts to acquire an exclusive lock but returns immediately if it's captured by another thread
         * @note Naming is in accordance to the Lockable named requirement
         */
        bool try_lock() const {
            return bufferDelegate->try_lock();
        }

        constexpr operator bool() const {
            return bufferDelegate != nullptr;
        }

        /**
         * @note The buffer **must** be locked prior to calling this
         */
        Buffer::BufferDelegate *operator->() const {
            return bufferDelegate.get();
        }

        /**
         * @brief Attaches a fence cycle to the underlying buffer in a way that it will be synchronized with the latest backing buffer
         * @note The view **must** be locked prior to calling this
         */
        void AttachCycle(const std::shared_ptr<FenceCycle> &cycle);

        /**
         * @brief Registers a callback for a usage of this view, it may be called multiple times due to the view being recreated with different backings
         * @note This will force the buffer to be host immutable for the current cycle, preventing megabuffering and requiring slower GPU inline writes instead
         * @note The callback will be automatically called the first time after registration
         * @note The view **must** be locked prior to calling this
         */
        void RegisterUsage(const std::shared_ptr<FenceCycle> &pCycle, const std::function<void(const Buffer::BufferViewStorage &, const std::shared_ptr<Buffer> &)> &usageCallback);

        /**
         * @brief Reads data at the specified offset in the view
         * @note The view **must** be locked prior to calling this
         * @note See Buffer::Read
         */
        void Read(const std::shared_ptr<FenceCycle> &pCycle, const std::function<void()> &flushHostCallback, span<u8> data, vk::DeviceSize offset) const;

        /**
         * @brief Writes data at the specified offset in the view
         * @note The view **must** be locked prior to calling this
         * @note See Buffer::Write
         */
        void Write(const std::shared_ptr<FenceCycle> &pCycle, const std::function<void()> &flushHostCallback, const std::function<void()> &gpuCopyCallback, span<u8> data, vk::DeviceSize offset) const;

        /**
         * @brief If megabuffering is beneficial for the current buffer, pushes its contents into the megabuffer and returns the offset of the pushed data
         * @return The offset of the pushed buffer contents in the megabuffer, or 0 if megabuffering is not to be used
         * @note The view **must** be locked prior to calling this
         */
        vk::DeviceSize AcquireMegaBuffer(MegaBuffer &megaBuffer) const;

        /**
         * @return A span of the backing buffer contents
         * @note The returned span **must** not be written to
         * @note The view **must** be kept locked until the span is no longer in use
         * @note See Buffer::GetReadOnlyBackingSpan
         */
        span<u8> GetReadOnlyBackingSpan(const std::shared_ptr<FenceCycle> &pCycle, const std::function<void()> &flushHostCallback);
    };
}
