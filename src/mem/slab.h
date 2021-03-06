#pragma once

#include "superslab.h"

namespace snmalloc
{
  class Slab
  {
  private:
    uint16_t address_to_index(address_t p)
    {
      // Get the offset from the slab for a memory location.
      return static_cast<uint16_t>(p - address_cast(this));
    }

  public:
    static Metaslab& get_meta(Slab* self)
    {
      Superslab* super = Superslab::get(self);
      return super->get_meta(self);
    }

    /**
     * Given a bumpptr and a fast_free_list head reference, builds a new free
     * list, and stores it in the fast_free_list. It will only create a page
     * worth of allocations, or one if the allocation size is larger than a
     * page.
     */
    static SNMALLOC_FAST_PATH void
    alloc_new_list(void*& bumpptr, FreeListHead& fast_free_list, size_t rsize)
    {
      auto snbumpptr = static_cast<SlabNext*>(bumpptr);
      fast_free_list.value = snbumpptr;

      void* newbumpptr = pointer_offset(bumpptr, rsize);
      void* slab_end = pointer_align_up<SLAB_SIZE>(newbumpptr);
      void* slab_end2 =
        pointer_align_up<OS_PAGE_SIZE>(pointer_offset(bumpptr, rsize * 32));
      if (slab_end2 < slab_end)
        slab_end = slab_end2;

      while (newbumpptr < slab_end)
      {
        auto newsnbumpptr = static_cast<SlabNext*>(newbumpptr);
        Metaslab::store_next(snbumpptr, newsnbumpptr);
        snbumpptr = newsnbumpptr;
        bumpptr = newbumpptr;
        newbumpptr = pointer_offset(bumpptr, rsize);
      }

      Metaslab::store_next(snbumpptr, nullptr);
      bumpptr = newbumpptr;
    }

    // Returns true, if it deallocation can proceed without changing any status
    // bits. Note that this does remove the use from the meta slab, so it
    // doesn't need doing on the slow path.
    //
    // This is pre-factored to take an explicit self parameter so that we can
    // eventually annotate that pointer with additional information.
    static SNMALLOC_FAST_PATH bool
    dealloc_fast(Slab* self, Superslab* super, void* p)
    {
      Metaslab& meta = super->get_meta(self);
#ifdef CHECK_CLIENT
      if (meta.is_unused())
        error("Detected potential double free.");
#endif

      if (unlikely(meta.return_object()))
        return false;

      // Update the head and the next pointer in the free list.
      SlabNext* head = meta.head;

      SlabNext* psn = static_cast<SlabNext*>(p);

      // Set the head to the memory being deallocated.
      meta.head = psn;
      SNMALLOC_ASSERT(meta.valid_head());

      // Set the next pointer to the previous head.
      Metaslab::store_next(psn, head);

      return true;
    }

    // If dealloc fast returns false, then call this.
    // This does not need to remove the "use" as done by the fast path.
    // Returns a complex return code for managing the superslab meta data.
    // i.e. This deallocation could make an entire superslab free.
    //
    // This is pre-factored to take an explicit self parameter so that we can
    // eventually annotate that pointer with additional information.
    static SNMALLOC_SLOW_PATH typename Superslab::Action
    dealloc_slow(Slab* self, SlabList* sl, Superslab* super, void* p)
    {
      Metaslab& meta = super->get_meta(self);
      meta.debug_slab_invariant(self);

      if (meta.is_full())
      {
        // We are not on the sizeclass list.
        if (meta.allocated == 1)
        {
          // Dealloc on the superslab.
          if (Metaslab::is_short(self))
            return super->dealloc_short_slab();

          return super->dealloc_slab(self);
        }
        SNMALLOC_ASSERT(meta.head == nullptr);
        SlabNext* psn = static_cast<SlabNext*>(p);
        meta.head = psn;
        Metaslab::store_next(psn, nullptr);
        meta.needed = meta.allocated - 1;

        // Push on the list of slabs for this sizeclass.
        sl->insert_prev(&meta);
        meta.debug_slab_invariant(self);
        return Superslab::NoSlabReturn;
      }

      // Remove from the sizeclass list and dealloc on the superslab.
      meta.remove();

      if (Metaslab::is_short(self))
        return super->dealloc_short_slab();

      return super->dealloc_slab(self);
    }
  };
} // namespace snmalloc
