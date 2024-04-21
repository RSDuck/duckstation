// Copyright 2016 Dolphin Emulator Project
// Copyright 2020 DuckStation Emulator Project
// Licensed under GPLv2+
// Refer to the LICENSE file included

#include "deko3d_memory_heap.h"
#include "deko3d_device.h"

#include "common/align.h"
#include "common/assert.h"

#include <cstdio>
#include <cstring>

void Deko3DMemoryHeap::BlockListPushFront(Block*& head, Block* block)
{
  if (head)
  {
    DebugAssert(head->prev == nullptr);
    head->prev = block;
  }

  block->prev = nullptr;
  block->next = head;
  head = block;
}

Deko3DMemoryHeap::Block* Deko3DMemoryHeap::BlockListPopFront(Block*& head)
{
  Block* result = head;
  DebugAssertMsg(result, "popping from empty block list");

  head = result->next;
  if (head)
    head->prev = nullptr;

  return result;
}

void Deko3DMemoryHeap::BlockListRemove(Block*& head, Block* block)
{
  DebugAssert((head == block) == !block->prev);
  if (!block->prev)
    head = block->next;

  if (block->prev)
    block->prev->next = block->next;
  if (block->next)
    block->next->prev = block->prev;
}

void Deko3DMemoryHeap::MapSizeToSecondLevel(uint32_t size, uint32_t& fl, uint32_t& sl)
{
  DebugAssertMsg(size >= 32, "block smaller than 32 bytes? Maybe freeing uninitialized block?");
  fl = 31 - __builtin_clz(size);
  sl = (size - (1 << fl)) >> (fl - 5);
}

void Deko3DMemoryHeap::MarkFree(Block* block)
{
  DebugAssert(!block->free);
  block->free = true;
  uint32_t fl, sl;
  MapSizeToSecondLevel(block->size, fl, sl);

  BlockListPushFront(m_second_free_list[(fl - 5) * 32 + sl], block);

  m_first_free_list |= 1 << (fl - 5);
  m_second_free_list_bits[fl - 5] |= 1 << sl;
}

void Deko3DMemoryHeap::UnmarkFree(Block* block)
{
  DebugAssert(block->free);
  block->free = false;
  uint32_t fl, sl;
  MapSizeToSecondLevel(block->size, fl, sl);

  BlockListRemove(m_second_free_list[(fl - 5) * 32 + sl], block);

  if (!m_second_free_list[(fl - 5) * 32 + sl])
  {
    m_second_free_list_bits[fl - 5] &= ~(1 << sl);

    if (m_second_free_list_bits[fl - 5] == 0)
      m_first_free_list &= ~(1 << (fl - 5));
  }
}

// makes a new block to the right and returns it
Deko3DMemoryHeap::Block* Deko3DMemoryHeap::SplitBlockRight(Block* block, uint32_t offset)
{
  DebugAssert(!block->free);
  DebugAssert(offset < block->size);
  Block* newBlock = BlockListPopFront(m_block_pool_unused);

  newBlock->offset = block->offset + offset;
  newBlock->size = block->size - offset;
  newBlock->sibling_left = block;
  newBlock->sibling_right = block->sibling_right;
  newBlock->free = false;
  if (newBlock->sibling_right)
  {
    DebugAssert(newBlock->sibling_right->sibling_left == block);
    newBlock->sibling_right->sibling_left = newBlock;
  }

  block->size -= newBlock->size;
  block->sibling_right = newBlock;

  return newBlock;
}

Deko3DMemoryHeap::Block* Deko3DMemoryHeap::MergeBlocksLeft(Block* block, Block* other)
{
  DebugAssert(block->sibling_right == other);
  DebugAssert(other->sibling_left == block);
  DebugAssert(!block->free);
  DebugAssert(!other->free);
  DebugAssert(block->offset + block->size == other->offset);
  block->size += other->size;
  block->sibling_right = other->sibling_right;
  if (block->sibling_right)
  {
    DebugAssert(block->sibling_right->sibling_left == other);
    block->sibling_right->sibling_left = block;
  }

  BlockListPushFront(m_block_pool_unused, other);

  return block;
}

Deko3DMemoryHeap::Deko3DMemoryHeap() = default;

Deko3DMemoryHeap::~Deko3DMemoryHeap()
{
  Destroy();
}

bool Deko3DMemoryHeap::Create(uint32_t size, uint32_t flags, uint32_t blockPoolSize)
{
  DebugAssert((size & (DK_MEMBLOCK_ALIGNMENT - 1)) == 0 && "block size not properly aligned");
  uint32_t sizeLog2 = 31 - __builtin_clz(size);
  if (((uint32_t)1 << sizeLog2) > size)
    sizeLog2++; // round up to the next power of two
  DebugAssert(sizeLog2 >= 5);

  m_memblock = dk::MemBlockMaker{Deko3DDevice::GetInstance().GetDevice(), size}
                 .setFlags(flags)
                 .create();

  if (!m_memblock)
    return false;

  uint32_t rows = sizeLog2 - 4; // remove rows below 32 bytes and round up to next

  m_second_free_list_bits = new uint32_t[rows];
  memset(m_second_free_list_bits, 0, rows * 4);
  m_second_free_list = new Block*[rows * 32];
  memset(m_second_free_list, 0, rows * 32 * 8);

  m_block_pool = new Block[blockPoolSize];
  memset(m_block_pool, 0, sizeof(Block) * blockPoolSize);
  for (uint32_t i = 0; i < blockPoolSize; i++)
    BlockListPushFront(m_block_pool_unused, &m_block_pool[i]);

  // insert heap into the free list
  Block* heap = BlockListPopFront(m_block_pool_unused);
  heap->offset = 0;
  heap->size = size - (flags & DkMemBlockFlags_Code ? DK_SHADER_CODE_UNUSABLE_SIZE : 0);
  heap->sibling_left = nullptr;
  heap->sibling_right = nullptr;
  heap->free = false;
  MarkFree(heap);

  return true;
}

void Deko3DMemoryHeap::Destroy()
{
  if (IsValid())
  {
    m_memblock.destroy();
    m_memblock = {};

    delete[] m_block_pool;
    delete[] m_second_free_list;
    delete[] m_second_free_list_bits;
  }
}

Deko3DMemoryHeap::Allocation Deko3DMemoryHeap::Alloc(uint32_t size, uint32_t align)
{
  DebugAssert(IsValid());
  DebugAssert(size > 0);
  DebugAssertMsg((align & (align - 1)) == 0, "alignment must be a power of two");
  // minimum alignment (and thus size) is 32 bytes
  align = std::max((uint32_t)32, align);
  size = Common::AlignUp(size, align);

  // printf("allocating %f MB on heap %p (used %f%%)\n", (float)size/(1024.f*1024.f), m_memblock.getCpuAddr(),
  // (float)Used/m_memblock.getSize());

  uint32_t fl, sl;
  MapSizeToSecondLevel(size + (align > 32 ? align : 0), fl, sl);

  uint32_t secondFreeListBits = m_second_free_list_bits[fl - 5] & (0xFFFFFFFF << (sl + 1));

  if (sl == 31 || secondFreeListBits == 0)
  {
    uint32_t firstFreeListBits = m_first_free_list & (0xFFFFFFFF << (fl - 5 + 1));
    DebugAssertMsg(firstFreeListBits, "out of memory :(");

    fl = __builtin_ctz(firstFreeListBits) + 5;
    secondFreeListBits = m_second_free_list_bits[fl - 5];
    DebugAssert(secondFreeListBits);
    sl = __builtin_ctz(secondFreeListBits);
  }
  DebugAssertMsg(secondFreeListBits, "out of memory :(");

  sl = __builtin_ctz(secondFreeListBits);

  Block* block = m_second_free_list[(fl - 5) * 32 + sl];
  UnmarkFree(block);

  // align within the block
  if ((block->offset & (align - 1)) > 0)
  {
    DebugAssert(align > 32);
    Block* newBlock = SplitBlockRight(block, ((block->offset + align - 1) & ~(align - 1)) - block->offset);
    MarkFree(block);
    block = newBlock;
  }
  // put remaining data back
  if (block->size > size)
  {
    MarkFree(SplitBlockRight(block, size));
  }

  DebugAssert((block->offset & (align - 1)) == 0);
  DebugAssert(block->size == size);
  return {static_cast<uint32_t>(block - m_block_pool), block->offset, block->size};
}

void Deko3DMemoryHeap::Free(Allocation allocation)
{
  DebugAssert(IsValid());
  Block* block = &m_block_pool[allocation.blockIdx];
  DebugAssert(!block->free);

  if (block->sibling_left && block->sibling_left->free)
  {
    UnmarkFree(block->sibling_left);
    block = MergeBlocksLeft(block->sibling_left, block);
  }
  if (block->sibling_right && block->sibling_right->free)
  {
    UnmarkFree(block->sibling_right);
    block = MergeBlocksLeft(block, block->sibling_right);
  }

  MarkFree(block);
}
