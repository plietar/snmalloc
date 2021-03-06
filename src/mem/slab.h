#pragma once

#include "superslab.h"

namespace snmalloc
{
  class Slab
  {
  private:
    uint16_t pointer_to_index(void* p)
    {
      // Get the offset from the slab for a memory location.
      return (uint16_t)((size_t)p - (size_t)this);
    }

  public:
    static Slab* get(void* p)
    {
      return (Slab*)((size_t)p & SLAB_MASK);
    }

    Metaslab& get_meta()
    {
      Superslab* super = Superslab::get(this);
      return super->get_meta(this);
    }

    SlabLink* get_link()
    {
      return get_meta().get_link(this);
    }

    template<ZeroMem zero_mem, typename MemoryProvider>
    void* alloc(SlabList* sc, size_t rsize, MemoryProvider& memory_provider)
    {
      // Read the head from the metadata stored in the superslab.
      Metaslab& meta = get_meta();
      uint16_t head = meta.head;

      assert(rsize == sizeclass_to_size(meta.sizeclass));
      meta.debug_slab_invariant(is_short(), this);
      assert(sc->get_head() == (SlabLink*)((size_t)this + meta.link));
      assert(!meta.is_full());

      meta.add_use();

      void* p;

      if ((head & 1) == 0)
      {
        p = (void*)((size_t)this + head);

        // Read the next slot from the memory that's about to be allocated.
        uint16_t next = *(uint16_t*)p;
        meta.head = next;
      }
      else
      {
        // This slab is being bump allocated.
        p = (void*)((size_t)this + head - 1);
        meta.head = (head + (uint16_t)rsize) & (SLAB_SIZE - 1);
        if (meta.head == 1)
        {
          meta.set_full();
        }
      }

      // If we're full, we're no longer the current slab for this sizeclass
      if (meta.is_full())
        sc->pop();

      meta.debug_slab_invariant(is_short(), this);

      if constexpr (zero_mem == YesZero)
      {
        if (rsize < PAGE_ALIGNED_SIZE)
          memory_provider.zero(p, rsize);
        else
          memory_provider.template zero<true>(p, rsize);
      }

      return p;
    }

    // Returns true, if it alters get_status.
    template<typename MemoryProvider>
    inline typename Superslab::Action dealloc(
      SlabList* sc, Superslab* super, void* p, MemoryProvider& memory_provider)
    {
      Metaslab& meta = super->get_meta(this);

      bool was_full = meta.is_full();
      meta.debug_slab_invariant(is_short(), this);
      meta.sub_use();

#ifndef SNMALLOC_SAFE_CLIENT
      if (!is_multiple_of_sizeclass(
            sizeclass_to_size(meta.sizeclass),
            (uintptr_t)this + SLAB_SIZE - (uintptr_t)p))
      {
        error("Not deallocating start of an object");
      }
#endif

      if (was_full)
      {
        // We are not on the sizeclass list.
        if (!meta.is_unused())
        {
          // Update the head and the sizeclass link.
          uint16_t index = pointer_to_index(p);
          meta.head = index;
          assert(meta.valid_head(is_short()));
          meta.link = index;

          // Push on the list of slabs for this sizeclass.
          sc->insert(meta.get_link(this));
          meta.debug_slab_invariant(is_short(), this);
        }
        else
        {
          // Dealloc on the superslab.
          if (is_short())
            return super->dealloc_short_slab(memory_provider);
          else
            return super->dealloc_slab(this, memory_provider);
        }
      }
      else if (meta.is_unused())
      {
        // Remove from the sizeclass list and dealloc on the superslab.
        sc->remove(meta.get_link(this));

        if (is_short())
          return super->dealloc_short_slab(memory_provider);
        else
          return super->dealloc_slab(this, memory_provider);
      }
      else
      {
#ifndef NDEBUG
        sc->debug_check_contains(meta.get_link(this));
#endif

        // Update the head and the next pointer in the free list.
        uint16_t head = meta.head;
        uint16_t current = pointer_to_index(p);

        // Set the head to the memory being deallocated.
        meta.head = current;
        assert(meta.valid_head(is_short()));

        // Set the next pointer to the previous head.
        *(uint16_t*)p = head;
        meta.debug_slab_invariant(is_short(), this);
      }
      return Superslab::NoSlabReturn;
    }

    bool is_short()
    {
      return ((size_t)this & SUPERSLAB_MASK) == (size_t)this;
    }
  };
}
