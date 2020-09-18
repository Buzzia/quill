#include "quill/detail/misc/FreeListAllocator.h"
#include "quill/QuillError.h"
#include "quill/detail/misc/Os.h"
#include "quill/detail/misc/Utilities.h"
#include <algorithm>
#include <cassert>

namespace quill
{
namespace detail
{
/***/
FreeListAllocator::FreeListAllocator(size_t initial_capacity) { reserve(initial_capacity); }

/***/
FreeListAllocator::~FreeListAllocator()
{
  for (auto* p : _requested_from_os)
  {
    aligned_free(p);
  }
}

/***/
void FreeListAllocator::reserve(size_t new_capacity)
{
  // Round up the size to max_align_t, that way we always aligned to max_align_t
  size_t const size_inc_padding = _add_padding(new_capacity);

  // Request this memory from operating system
  Block* block = _request_from_os(size_inc_padding);

  // Finally add this block to our free list
  auto insert_it = _free_list.emplace(block->header.size, std::vector<Block*>{});
  insert_it.first->second.emplace_back(block);
}

/***/
void FreeListAllocator::set_minimum_allocation(size_t minimum_alloc_size)
{
  if (!is_pow_of_two(minimum_alloc_size))
  {
    QUILL_THROW(QuillError{"minimum_alloc_size needs to be a power of 2"});
  }

  _minimum_allocation = minimum_alloc_size;
}

/***/
void* FreeListAllocator::allocate(size_t s)
{
  // Round up the size to max_align_t, that way we always aligned to max_align_t
  size_t const size_inc_padding = _add_padding(s);

  // First look for a free block in the free list
  Block* block = _find_free_block(size_inc_padding);

  if (block)
  {
    // We found a block in the free list
    return block->data;
  }

  // No block in the free list was found. We will allocate more memory
  // Round up the size to the nearest page size
  size_t requested_size = size_inc_padding;

  if (_minimum_allocation != 0)
  {
    size_t const mask = _minimum_allocation - 1;
    requested_size += mask;
    requested_size &= ~mask;

    assert((requested_size % _minimum_allocation == 0) &&
           "rounded_up_size is not multiple of a huge page size");
  }

  // Request this size from OS
  block = _request_from_os(requested_size);

  // Slice the block if it is too large, to reuse the free part.
  block = _slice(block, size_inc_padding);

  // Mark the block as used
  block->header.used = true;

  return block->data;
}

/***/
void FreeListAllocator::deallocate(void* p)
{
  // Given the pointer we know we had constructed a Block earlier if we subtract the BlockHeader
  Block* block = reinterpret_cast<Block*>(static_cast<unsigned char*>(p) - sizeof(BlockHeader));

  // Check if we can coalesce with the next block or the previous block
  block = _coalesce_with_next(block);
  block = _coalesce_with_previous(block);

  block->header.used = false;

  // Finally add this block to our free list
  auto insert_it = _free_list.emplace(block->header.size, std::vector<Block*>{});

  if (insert_it.second)
  {
    // if we are inserting for the first time reserve some space in the vector
    insert_it.first->second.reserve(32);
  }

  // If we failed to insert, we append to a new vector
  // If we inserted, we append to existing vector
  insert_it.first->second.emplace_back(block);
}

/***/
FreeListAllocator::Block* FreeListAllocator::_find_free_block(size_t size)
{
  // look for a free block that is greater or equal to the requested size
  auto search_block_it = _free_list.lower_bound(size);

  if (search_block_it != _free_list.end())
  {
    // we found a block in our free list, and we will use the last one from the vector

    assert(!search_block_it->second.empty() && "Vector can never be empty if it exists in the map.");

    Block* block = search_block_it->second.back();

    // a paranoid check that the key in the map and the header size are in sync ..
    assert((search_block_it->first == block->header.size) && "Header size and key must match");

    // Slice the block if it is too large, to reuse the free part.
    block = _slice(block, size);

    // Mark the block as used
    block->header.used = true;

    // remove this block from the free list as we are going to be allocating it
    search_block_it->second.pop_back();

    // if the whole vector is empty we need to remove the whole entry
    if (search_block_it->second.empty())
    {
      _free_list.erase(search_block_it);
    }

    return block;
  }

  // we couldn't find one block
  return nullptr;
}

/***/
FreeListAllocator::Block* FreeListAllocator::_slice(Block* block, size_t requested_size)
{
  assert(block->header.size >= requested_size &&
         "block->header.size is always greater or equal to requested size");

  // Check if the block can be sliced
  // is (this block size minus the requested size) greater or equal to a new block size
  size_t const remaining_size = block->header.size - requested_size;
  if (remaining_size < sizeof(Block))
  {
    // we don't have enough space to slice so we return the existing block
    return block;
  }

  // to get the memory to the free block part we have to add the header size to the requested size
  void* free_part = reinterpret_cast<unsigned char*>(block->data) + requested_size;

  // Now in that memory we will construct a new block
  Block* free_block = new (free_part) Block;

  // The size of the new block will be the remaining size excluding the header size
  free_block->header.size = block->header.size - requested_size - sizeof(BlockHeader);

  // This block is free - not used
  free_block->header.used = false;
  free_block->header.allocation_id = block->header.allocation_id;

  // The next block will be the next of the existing block
  free_block->header.next = block->header.next;
  // The previous will be the existing block
  free_block->header.previous = block;

  // If the free block has a next block, it now needs to point it's previous to the
  // new free block we are creating
  Block* next_next_block = free_block->header.next;
  if (next_next_block)
  {
    next_next_block->header.previous = free_block;
  }

  // Add this free block to the free list
  // Finally add this block to our free list
  auto insert_it = _free_list.emplace(free_block->header.size, std::vector<Block*>{});

  if (insert_it.second)
  {
    // if we are inserting for the first time reserve some space in the vector
    insert_it.first->second.reserve(32);
  }

  // If we failed to insert, we append to a new vector
  // If we inserted, we append to existing vector
  insert_it.first->second.emplace_back(free_block);

  // Now the existing block that we split
  block->header.size = requested_size;

  // The next is the free block that we split
  block->header.next = free_block;

  return block;
}

/**
 * Coalesces two adjacent blocks.
 */
FreeListAllocator::Block* FreeListAllocator::_coalesce_with_next(Block* block)
{
  Block* next_block = block->header.next;

  // Check if we can coalesce with the next block
  // check that
  // 1) a next block exists,
  // 2) a next block is not used
  // 3) the next block is a continuous memory with the current block (they have the same allocation id)
  bool const can_coalesce_with_next = next_block && !next_block->header.used &&
    (next_block->header.allocation_id == block->header.allocation_id);

  if (!can_coalesce_with_next)
  {
    // we didn't coalesce, return the block without touching it
    return block;
  }

  // we can coalesce.

  // Also remove the next block from the freelist as we are coalescing it.
  // To remove, we need to find the value of the free block in the free list to remove
  auto search_block_it = _free_list.find(next_block->header.size);
  assert(search_block_it != _free_list.end() && "The block must exist in the map.");

  std::vector<Block*>& blocks_vec = search_block_it->second;
  blocks_vec.erase(std::remove_if(blocks_vec.begin(), blocks_vec.end(),
                                  [next_block](Block* b) -> bool { return b == next_block; }),
                   blocks_vec.end());

  // if now the vector is empty we need to remove it from the map collection
  if (blocks_vec.empty())
  {
    _free_list.erase(search_block_it);
  }

  // Increase the size of the block including the header size of the next_block
  block->header.size += next_block->header.size + sizeof(BlockHeader);

  // The block next will point to where the next block was pointing
  block->header.next = next_block->header.next;

  // We also have to check the previous of the next_block and connect it to our new coalesced block.
  Block* next_of_next_block = next_block->header.next;
  if (next_of_next_block)
  {
    // The next of next block previous was pointing to the next_block, now it has to point to our block
    next_of_next_block->header.previous = block;
  }

  // NOTE: we also need to destroy BlockHeader from that location but we do not call
  // next_block->~Block() because Block is trivially destructible

  return block;
}

/**
 * Coalesces two adjacent blocks.
 */
FreeListAllocator::Block* FreeListAllocator::_coalesce_with_previous(Block* block)
{
  Block* previous_block = block->header.previous;

  // Check if we can coalesce with the next block
  // check that
  // 1) a previous block exists,
  // 2) a previous block is not used
  // 3) the previous block is a continuous memory with the current block (they have the same allocation id)
  bool const can_coalesce_with_previous = previous_block && !previous_block->header.used &&
    (previous_block->header.allocation_id == block->header.allocation_id);

  if (!can_coalesce_with_previous)
  {
    // we didn't coalesce, return the block without touching it
    return block;
  }

  // we can coalesce.

  // Also remove the next block from the freelist as we are coalescing it.
  // To remove, we need to find the value of the free block in the free list to remove

  // Here we remove the block from our order collection because and we can not avoid it
  // because the size of the block will also change
  auto search_block_it = _free_list.find(previous_block->header.size);
  assert(search_block_it != _free_list.end() && "The block must exist in the map.");

  std::vector<Block*>& blocks_vec = search_block_it->second;
  blocks_vec.erase(std::remove_if(blocks_vec.begin(), blocks_vec.end(),
                                  [previous_block](Block* b) -> bool { return b == previous_block; }),
                   blocks_vec.end());

  // if now the vector is empty we need to remove it from the map collection
  if (blocks_vec.empty())
  {
    _free_list.erase(search_block_it);
  }

  // Increase the size of the previous_block block including the current header size of this block
  previous_block->header.size += block->header.size + sizeof(BlockHeader);

  // The previous_block next will point to where the current block next was pointing
  previous_block->header.next = block->header.next;

  // If there is a next block it now needs to point to the previous block
  Block* next_block = block->header.next;
  if (next_block)
  {
    next_block->header.previous = previous_block;
  }

  // Note: we also need to destroy BlockHeader from that location but we do not call
  // block->~Block() because Block is trivially destructible

  // we return the address of the previous block
  return previous_block;
}

/***/
FreeListAllocator::Block* FreeListAllocator::_request_from_os(size_t size)
{
  size_t const total_size = _add_header_size(size);

  // Request aligned memory from OS
  void* buffer = aligned_alloc(alignof(std::max_align_t), total_size);

  // in the returned pointer create a block*
  Block* block = new (buffer) Block;

  // Set the size, we do not include the header size here
  block->header.size = size;
  block->header.allocation_id = static_cast<uint32_t>(_requested_from_os.size()) + 1;
  block->header.used = false;

  // Use the top block (previous) to link it to the new current block
  if (!_requested_from_os.empty())
  {
    // We have already one existing block
    Block* previous_allocated_segment_current = _requested_from_os.back();

    while (previous_allocated_segment_current)
    {
      Block* next_block = previous_allocated_segment_current->header.next;

      if (!next_block)
      {
        // if the next block is nullptr this is what we are looking for and
        // we can break
        break;
      }

      previous_allocated_segment_current = previous_allocated_segment_current->header.next;
    }

    // now we can link the last block of the previous allocated segment
    assert(!previous_allocated_segment_current->header.next &&
           "previous_allocated_segment_current->header.next needs to be nullptr");

    // the previous block will now point to our new block
    previous_allocated_segment_current->header.next = block;

    // The new block previous will point to the previous block
    block->header.previous = previous_allocated_segment_current;
  }

  // Finally store the returned pointer to a unique ptr so we can delete it later
  _requested_from_os.emplace_back(block);

  return block;
}

/**
 * We always round up the size to the max align value.
 * The BlockHeader is already 2 * sizeof(max_align_t).
 * Here we add a multiple of max_align_t for the actual size.
 * This means that we are always aligned to max_align_t size for all data* we return.
 * @param s the size we want to round
 * @return the size rounded to max_align_t, e.g 16,32,48,64 etc
 */
size_t FreeListAllocator::_add_padding(size_t s) noexcept
{
  return (s + sizeof(std::max_align_t) - 1) & ~(sizeof(std::max_align_t) - 1);
}

/**
 * Returns total allocation size, reserving in addition the space for
 * the BlockHeader structure
 * @param s the initial size
 * @return the size including the header
 */
size_t FreeListAllocator::_add_header_size(size_t s) noexcept { return s + sizeof(BlockHeader); }

} // namespace detail
} // namespace quill