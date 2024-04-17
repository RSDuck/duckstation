// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "memmap.h"
#include "align.h"
#include "assert.h"
#include "error.h"
#include "log.h"
#include "small_string.h"
#include "string_util.h"

#include "fmt/format.h"

#if defined(_WIN32)
#include "windows_headers.h"
#elif defined(__SWITCH__)
#include <switch.h>
#elif !defined(__ANDROID__)
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#if defined(__APPLE__) && defined(__aarch64__)
// pthread_jit_write_protect_np()
#include <pthread.h>
#endif

Log_SetChannel(MemoryArena);

#ifdef _WIN32

bool MemMap::MemProtect(void* baseaddr, size_t size, PageProtect mode)
{
  DebugAssert((size & (HOST_PAGE_SIZE - 1)) == 0);

  DWORD old_protect;
  if (!VirtualProtect(baseaddr, size, static_cast<DWORD>(mode), &old_protect))
  {
    Log_ErrorPrintf("VirtualProtect() failed with error %u", GetLastError());
    return false;
  }

  return true;
}

std::string MemMap::GetFileMappingName(const char* prefix)
{
  const unsigned pid = GetCurrentProcessId();
  return fmt::format("{}_{}", prefix, pid);
}

void* MemMap::CreateSharedMemory(const char* name, size_t size, Error* error)
{
  const HANDLE mapping =
    static_cast<void*>(CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, static_cast<DWORD>(size >> 32),
                                          static_cast<DWORD>(size), StringUtil::UTF8StringToWideString(name).c_str()));
  if (!mapping)
    Error::SetWin32(error, "CreateFileMappingW() failed: ", GetLastError());

  return mapping;
}

void MemMap::DestroySharedMemory(void* ptr)
{
  CloseHandle(static_cast<HANDLE>(ptr));
}

void* MemMap::MapSharedMemory(void* handle, size_t offset, void* baseaddr, size_t size, PageProtect mode)
{
  void* ret = MapViewOfFileEx(static_cast<HANDLE>(handle), FILE_MAP_READ | FILE_MAP_WRITE,
                              static_cast<DWORD>(offset >> 32), static_cast<DWORD>(offset), size, baseaddr);
  if (!ret)
    return nullptr;

  if (mode != PageProtect::ReadWrite)
  {
    DWORD old_prot;
    if (!VirtualProtect(ret, size, static_cast<DWORD>(mode), &old_prot))
      Panic("Failed to protect memory mapping");
  }
  return ret;
}

void MemMap::UnmapSharedMemory(void* baseaddr, size_t size)
{
  if (!UnmapViewOfFile(baseaddr))
    Panic("Failed to unmap shared memory");
}

SharedMemoryMappingArea::SharedMemoryMappingArea() = default;

SharedMemoryMappingArea::~SharedMemoryMappingArea()
{
  Destroy();
}

SharedMemoryMappingArea::PlaceholderMap::iterator SharedMemoryMappingArea::FindPlaceholder(size_t offset)
{
  if (m_placeholder_ranges.empty())
    return m_placeholder_ranges.end();

  // this will give us an iterator equal or after page
  auto it = m_placeholder_ranges.lower_bound(offset);
  if (it == m_placeholder_ranges.end())
  {
    // check the last page
    it = (++m_placeholder_ranges.rbegin()).base();
  }

  // it's the one we found?
  if (offset >= it->first && offset < it->second)
    return it;

  // otherwise try the one before
  if (it == m_placeholder_ranges.begin())
    return m_placeholder_ranges.end();

  --it;
  if (offset >= it->first && offset < it->second)
    return it;
  else
    return m_placeholder_ranges.end();
}

bool SharedMemoryMappingArea::Create(size_t size)
{
  Destroy();

  AssertMsg(Common::IsAlignedPow2(size, HOST_PAGE_SIZE), "Size is page aligned");

  m_base_ptr = static_cast<u8*>(VirtualAlloc2(GetCurrentProcess(), nullptr, size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                              PAGE_NOACCESS, nullptr, 0));
  if (!m_base_ptr)
    return false;

  m_size = size;
  m_num_pages = size / HOST_PAGE_SIZE;
  m_placeholder_ranges.emplace(0, size);
  return true;
}

void SharedMemoryMappingArea::Destroy()
{
  AssertMsg(m_num_mappings == 0, "No mappings left");

  // hopefully this will be okay, and we don't need to coalesce all the placeholders...
  if (m_base_ptr && !VirtualFreeEx(GetCurrentProcess(), m_base_ptr, 0, MEM_RELEASE))
    Panic("Failed to release shared memory area");

  m_placeholder_ranges.clear();
  m_base_ptr = nullptr;
  m_size = 0;
  m_num_pages = 0;
  m_num_mappings = 0;
}

u8* SharedMemoryMappingArea::Map(void* file_handle, size_t file_offset, void* map_base, size_t map_size,
                                 PageProtect mode)
{
  DebugAssert(static_cast<u8*>(map_base) >= m_base_ptr && static_cast<u8*>(map_base) < (m_base_ptr + m_size));

  const size_t map_offset = static_cast<u8*>(map_base) - m_base_ptr;
  DebugAssert(Common::IsAlignedPow2(map_offset, HOST_PAGE_SIZE));
  DebugAssert(Common::IsAlignedPow2(map_size, HOST_PAGE_SIZE));

  // should be a placeholder. unless there's some other mapping we didn't free.
  PlaceholderMap::iterator phit = FindPlaceholder(map_offset);
  DebugAssertMsg(phit != m_placeholder_ranges.end(), "Page we're mapping is a placeholder");
  DebugAssertMsg(map_offset >= phit->first && map_offset < phit->second, "Page is in returned placeholder range");
  DebugAssertMsg((map_offset + map_size) <= phit->second, "Page range is in returned placeholder range");

  // do we need to split to the left? (i.e. is there a placeholder before this range)
  const size_t old_ph_end = phit->second;
  if (map_offset != phit->first)
  {
    phit->second = map_offset;

    // split it (i.e. left..start and start..end are now separated)
    if (!VirtualFreeEx(GetCurrentProcess(), OffsetPointer(phit->first), (map_offset - phit->first),
                       MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
    {
      Panic("Failed to left split placeholder for map");
    }
  }
  else
  {
    // start of the placeholder is getting used, we'll split it right below if there's anything left over
    m_placeholder_ranges.erase(phit);
  }

  // do we need to split to the right? (i.e. is there a placeholder after this range)
  if ((map_offset + map_size) != old_ph_end)
  {
    // split out end..ph_end
    m_placeholder_ranges.emplace(map_offset + map_size, old_ph_end);

    if (!VirtualFreeEx(GetCurrentProcess(), OffsetPointer(map_offset), map_size,
                       MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
    {
      Panic("Failed to right split placeholder for map");
    }
  }

  // actually do the mapping, replacing the placeholder on the range
  if (!MapViewOfFile3(static_cast<HANDLE>(file_handle), GetCurrentProcess(), map_base, file_offset, map_size,
                      MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0))
  {
    Log_ErrorPrintf("MapViewOfFile3() failed: %u", GetLastError());
    return nullptr;
  }

  if (mode != PageProtect::ReadWrite)
  {
    DWORD old_prot;
    if (!VirtualProtect(map_base, map_size, static_cast<DWORD>(mode), &old_prot))
      Panic("Failed to protect memory mapping");
  }

  m_num_mappings++;
  return static_cast<u8*>(map_base);
}

bool SharedMemoryMappingArea::Unmap(void* map_base, size_t map_size)
{
  DebugAssert(static_cast<u8*>(map_base) >= m_base_ptr && static_cast<u8*>(map_base) < (m_base_ptr + m_size));

  const size_t map_offset = static_cast<u8*>(map_base) - m_base_ptr;
  DebugAssert(Common::IsAlignedPow2(map_offset, HOST_PAGE_SIZE));
  DebugAssert(Common::IsAlignedPow2(map_size, HOST_PAGE_SIZE));

  // unmap the specified range
  if (!UnmapViewOfFile2(GetCurrentProcess(), map_base, MEM_PRESERVE_PLACEHOLDER))
  {
    Log_ErrorPrintf("UnmapViewOfFile2() failed: %u", GetLastError());
    return false;
  }

  // can we coalesce to the left?
  PlaceholderMap::iterator left_it = (map_offset > 0) ? FindPlaceholder(map_offset - 1) : m_placeholder_ranges.end();
  if (left_it != m_placeholder_ranges.end())
  {
    // the left placeholder should end at our start
    DebugAssert(map_offset == left_it->second);
    left_it->second = map_offset + map_size;

    // combine placeholders before and the range we're unmapping, i.e. to the left
    if (!VirtualFreeEx(GetCurrentProcess(), OffsetPointer(left_it->first), left_it->second - left_it->first,
                       MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS))
    {
      Panic("Failed to coalesce placeholders left for unmap");
    }
  }
  else
  {
    // this is a new placeholder
    left_it = m_placeholder_ranges.emplace(map_offset, map_offset + map_size).first;
  }

  // can we coalesce to the right?
  PlaceholderMap::iterator right_it =
    ((map_offset + map_size) < m_size) ? FindPlaceholder(map_offset + map_size) : m_placeholder_ranges.end();
  if (right_it != m_placeholder_ranges.end())
  {
    // should start at our end
    DebugAssert(right_it->first == (map_offset + map_size));
    left_it->second = right_it->second;
    m_placeholder_ranges.erase(right_it);

    // combine our placeholder and the next, i.e. to the right
    if (!VirtualFreeEx(GetCurrentProcess(), OffsetPointer(left_it->first), left_it->second - left_it->first,
                       MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS))
    {
      Panic("Failed to coalescae placeholders right for unmap");
    }
  }

  m_num_mappings--;
  return true;
}

#elif defined(__SWITCH__)

// welcome to the hack zone
// everything here can fall apart the moment you look at it the wrong way

// in particular only the first mapping of a shared memory is arbitrarily
// reprotectable

struct Mirror
{
  void *addr, *source;
  u64 size;
  std::vector<bool> mapping_state;

  u64 FindIslandSize(u64 offset)
  {
    bool start_state = mapping_state[offset >> 12];
    u64 island_size = 0x1000;
    while ((offset + island_size) < size && mapping_state[(offset + island_size) >> 12] == start_state)
      island_size += 0x1000;

    return island_size;
  }
};

struct VMemReservation
{
  void* addr;
  VirtmemReservation* reservation;
};

struct CodeMemoryMapping
{
  u64 heap_memory;
  void* code_memory;
  size_t size;
  void* heap_base;
};

std::vector<VMemReservation> VMemReservations;
std::vector<CodeMemoryMapping> CodeMemories;
std::vector<Mirror> Mappings;

// not necessary on Switch
std::string MemMap::GetFileMappingName(const char* prefix)
{
  return {};
}

void* ReserveVirtmem(size_t size)
{
  void* addr = virtmemFindAslr(size, 0x1000);
  if (!addr)
  {
    Log_ErrorPrintf("virtmemFindAslr failed (size %zx)", size);
    return nullptr;
  }
  VirtmemReservation* reservation = virtmemAddReservation(addr, size);
  if (!addr)
    Log_ErrorPrintf("virtmemAddReservation failed");

  VMemReservations.push_back({addr, reservation});

  return addr;
}

void FreeVirtmem(void* addr)
{
  for (auto it = VMemReservations.begin(); it != VMemReservations.end(); ++it)
  {
    if (it->addr == addr)
    {
      virtmemRemoveReservation(it->reservation);

      VMemReservations.erase(it);
      return;
    }
  }

  Log_ErrorPrintf("Trying to free unknown virtmem reservation %p", addr);
}

void* MemMap::CreateSharedMemory(const char* name, size_t size, Error* error)
{
  void* heapMemory = aligned_alloc(0x1000, size);

  if (!heapMemory)
  {
    Log_ErrorPrintf("Failed to allocate heap memory backing %zx", size);
    virtmemUnlock();
    return nullptr;
  }

  return heapMemory;
}

void MemMap::DestroySharedMemory(void* ptr)
{
  for (auto it = CodeMemories.begin(); it != CodeMemories.end();)
  {
    if (it->heap_base == ptr)
    {
      Result result = svcUnmapProcessCodeMemory(envGetOwnProcessHandle(), reinterpret_cast<u64>(it->code_memory),
                                                it->heap_memory, it->size);
      if (R_FAILED(result))
      {
        Log_ErrorPrintf("Unmapping code memory failed %x %p %lx %lx", result, it->code_memory, it->heap_memory, it->size);
      }

      it = CodeMemories.erase(it);
    }
    else
    {
      it++;
    }
  }

  free(ptr);
}

u32 ToHOSPermission(PageProtect mode)
{
  u32 permissions = 0;
  if (mode == PageProtect::ReadOnly)
    permissions = Perm_R;
  else if (mode == PageProtect::ReadWrite)
    permissions = Perm_Rw;
  else if (mode == PageProtect::NoAccess)
    permissions = Perm_None;
  else
    Panic("Invalid permissions");
  return permissions;
}

void* MemMap::MapSharedMemory(void* handle, size_t offset, void* baseaddr, size_t size, PageProtect mode)
{
  AssertMsg(mode == PageProtect::ReadWrite, "Switch requires mapping to be ReadWrite initially");
  virtmemLock();

  if (!baseaddr)
  {
    baseaddr = ReserveVirtmem(size);
    if (!baseaddr)
    {
      virtmemLock();
      return nullptr;
    }
  }
  virtmemUnlock();

  u64 heap_memory = reinterpret_cast<u64>(handle) + offset;

  for (auto it = CodeMemories.begin(); it != CodeMemories.end(); it++)
  {
    if (it->heap_memory == reinterpret_cast<u64>(handle) + offset)
    {
      DebugAssert(mode == PageProtect::ReadWrite);
      Mappings.push_back({baseaddr, static_cast<u8*>(it->code_memory), size, std::vector<bool>(size >> 12)});

      MemMap::MemProtect(baseaddr, size, PageProtect::ReadWrite);

      return reinterpret_cast<u8*>(baseaddr);
    }
  }

  Result result = svcMapProcessCodeMemory(envGetOwnProcessHandle(), reinterpret_cast<u64>(baseaddr), heap_memory, size);
  if (R_FAILED(result))
  {
    Log_ErrorPrintf("svcMapProcessCodeMemory failed %x", result);
    return nullptr;
  }
  result = svcSetProcessMemoryPermission(envGetOwnProcessHandle(), reinterpret_cast<u64>(baseaddr), size,
                                         ToHOSPermission(mode));
  if (R_FAILED(result))
  {
    Log_ErrorPrintf("svcSetProcessMemoryPermission failed %x", result);
    return nullptr;
  }
  CodeMemories.push_back(CodeMemoryMapping{heap_memory, baseaddr, size, handle});

  Log_DebugPrintf("Creating code memory %p %lx", baseaddr, size);
  return baseaddr;
}

void MemMap::UnmapSharedMemory(void* baseaddr, size_t size)
{
  for (auto it = CodeMemories.begin(); it != CodeMemories.end(); it++)
  {
    if (it->code_memory == baseaddr)
    {
      // we can only unmap code memories once all mapping based on them
      // are destroyed. So we'll destroy them later when the "shared memory"
      // is destroyed.
      return;
    }
  }

  for (auto it = Mappings.begin(); it != Mappings.end(); ++it)
  {
    if (it->addr == baseaddr)
    {
      if (!MemMap::MemProtect(baseaddr, size, PageProtect::NoAccess))
      {
        Log_ErrorPrintf("Failed to unmap memory mapping");
        return;
      }

      Mappings.erase(it);
      return;
    }
  }

  Log_ErrorPrintf("Trying to unmap unknown shared memory (baseaddr=%p, size=%zx)", baseaddr, size);
}

bool MemMap::MemProtect(void* baseaddr, size_t size, PageProtect mode)
{
  for (auto it = CodeMemories.begin(); it != CodeMemories.end(); it++)
  {
    if (reinterpret_cast<u64>(baseaddr) >= reinterpret_cast<u64>(it->code_memory) &&
        reinterpret_cast<u64>(baseaddr) + size <= reinterpret_cast<u64>(it->code_memory) + it->size)
    {
      u32 perms = ToHOSPermission(mode);
      Result result = svcSetMemoryPermission(baseaddr, size, perms);
      if (R_FAILED(result))
      {
        Log_ErrorPrintf("svcSetProcessMemoryPermission failed %x %p %lx\n", result, baseaddr, size);
        return false;
      }
      return true;
    }
  }
  for (auto it = Mappings.begin(); it != Mappings.end(); ++it)
  {
    if (reinterpret_cast<u64>(baseaddr) >= reinterpret_cast<u64>(it->addr) &&
        reinterpret_cast<u64>(baseaddr) + size <= reinterpret_cast<u64>(it->addr) + it->size)
    {
      u64 offset = reinterpret_cast<u64>(baseaddr) - reinterpret_cast<u64>(it->addr);
      u64 src = reinterpret_cast<u64>(it->source);

      while (size > 0)
      {
        u64 island_size = it->FindIslandSize(offset);
        if (island_size > size)
          island_size = size;

        if (mode == PageProtect::ReadWrite)
        {
          if (!it->mapping_state[offset >> 12])
          {
            if (R_FAILED(svcMapProcessMemory(reinterpret_cast<void*>(reinterpret_cast<u64>(it->addr) + offset),
                                             envGetOwnProcessHandle(), src + offset, island_size)))
            {
              Log_ErrorPrintf("Map process memory failed\n");
              return false;
            }
          }
        }
        else
        {
          if (it->mapping_state[offset >> 12])
          {
            if (R_FAILED(svcUnmapProcessMemory(reinterpret_cast<void*>(reinterpret_cast<u64>(it->addr) + offset),
                                               envGetOwnProcessHandle(), src + offset, island_size)))
            {
              Log_ErrorPrintf("Unmap process memory failed %lx %lx\n", offset, island_size);
              return false;
            }
          }
        }

        bool original_state = it->mapping_state[offset >> 12];
        for (u64 i = 0; i < island_size >> 12; i++)
        {
          DebugAssert(it->mapping_state[(offset >> 12) + i] == original_state);
          it->mapping_state[(offset >> 12) + i] = mode == PageProtect::ReadWrite;
        }

        offset += island_size;
        size -= island_size;
      }
      return true;
    }
  }

  Log_ErrorPrintf("Trying to reprotect memory which was never mapped");
  return false;
}

SharedMemoryMappingArea::SharedMemoryMappingArea()
{
}
SharedMemoryMappingArea::~SharedMemoryMappingArea()
{
}

bool SharedMemoryMappingArea::Create(size_t size)
{
  virtmemLock();
  m_base_ptr = reinterpret_cast<u8*>(ReserveVirtmem(size));
  if (!m_base_ptr)
    Log_ErrorPrintf("failed to create memory area (size=%zx)", size);

  virtmemUnlock();
  return m_base_ptr != nullptr;
}

void SharedMemoryMappingArea::Destroy()
{
  virtmemLock();
  FreeVirtmem(m_base_ptr);
  virtmemUnlock();
}

u8* SharedMemoryMappingArea::Map(void* file_handle, size_t file_offset, void* map_base, size_t map_size,
                                 PageProtect mode)
{
  return static_cast<u8*>(MemMap::MapSharedMemory(file_handle, file_offset, map_base, map_size, mode));
}

bool SharedMemoryMappingArea::Unmap(void* map_base, size_t map_size)
{
  MemMap::UnmapSharedMemory(map_base, map_size);
  return true; // cheat
}

ALWAYS_INLINE static void BeginCodeWrite()
{
}
ALWAYS_INLINE static void EndCodeWrite()
{
}

#elif !defined(__ANDROID__)

bool MemMap::MemProtect(void* baseaddr, size_t size, PageProtect mode)
{
  DebugAssertMsg((size & (HOST_PAGE_SIZE - 1)) == 0, "Size is page aligned");

  const int result = mprotect(baseaddr, size, static_cast<int>(mode));
  if (result != 0)
  {
    Log_ErrorPrintf("mprotect() for %zu at %p failed", size, baseaddr);
    return false;
  }

  return true;
}

std::string MemMap::GetFileMappingName(const char* prefix)
{
  const unsigned pid = static_cast<unsigned>(getpid());
#if defined(__FreeBSD__)
  // FreeBSD's shm_open(3) requires name to be absolute
  return fmt::format("/tmp/{}_{}", prefix, pid);
#else
  return fmt::format("{}_{}", prefix, pid);
#endif
}

void* MemMap::CreateSharedMemory(const char* name, size_t size, Error* error)
{
  const int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0)
  {
    Error::SetErrno(error, "shm_open failed: ", errno);
    return nullptr;
  }

  // we're not going to be opening this mapping in other processes, so remove the file
  shm_unlink(name);

  // use fallocate() to ensure we don't SIGBUS later on.
#ifdef __linux__
  if (fallocate(fd, 0, 0, static_cast<off_t>(size)) < 0)
  {
    Error::SetErrno(error, TinyString::from_format("fallocate({}) failed: ", size), errno);
    return nullptr;
  }
#else
  // ensure it's the correct size
  if (ftruncate(fd, static_cast<off_t>(size)) < 0)
  {
    Error::SetErrno(error, TinyString::from_format("ftruncate({}) failed: ", size), errno);
    return nullptr;
  }
#endif

  return reinterpret_cast<void*>(static_cast<intptr_t>(fd));
}

void MemMap::DestroySharedMemory(void* ptr)
{
  close(static_cast<int>(reinterpret_cast<intptr_t>(ptr)));
}

void* MemMap::MapSharedMemory(void* handle, size_t offset, void* baseaddr, size_t size, PageProtect mode)
{
  const int flags = (baseaddr != nullptr) ? (MAP_SHARED | MAP_FIXED) : MAP_SHARED;
  void* ptr = mmap(baseaddr, size, static_cast<int>(mode), flags, static_cast<int>(reinterpret_cast<intptr_t>(handle)),
                   static_cast<off_t>(offset));
  if (ptr == MAP_FAILED)
    return nullptr;

  return ptr;
}

void MemMap::UnmapSharedMemory(void* baseaddr, size_t size)
{
  if (mmap(baseaddr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED)
    Panic("Failed to unmap shared memory");
}

SharedMemoryMappingArea::SharedMemoryMappingArea() = default;

SharedMemoryMappingArea::~SharedMemoryMappingArea()
{
  Destroy();
}

bool SharedMemoryMappingArea::Create(size_t size)
{
  AssertMsg(Common::IsAlignedPow2(size, HOST_PAGE_SIZE), "Size is page aligned");
  Destroy();

  void* alloc = mmap(nullptr, size, PROT_NONE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  if (alloc == MAP_FAILED)
    return false;

  m_base_ptr = static_cast<u8*>(alloc);
  m_size = size;
  m_num_pages = size / HOST_PAGE_SIZE;
  return true;
}

void SharedMemoryMappingArea::Destroy()
{
  AssertMsg(m_num_mappings == 0, "No mappings left");

  if (m_base_ptr && munmap(m_base_ptr, m_size) != 0)
    Panic("Failed to release shared memory area");

  m_base_ptr = nullptr;
  m_size = 0;
  m_num_pages = 0;
}

u8* SharedMemoryMappingArea::Map(void* file_handle, size_t file_offset, void* map_base, size_t map_size,
                                 PageProtect mode)
{
  DebugAssert(static_cast<u8*>(map_base) >= m_base_ptr && static_cast<u8*>(map_base) < (m_base_ptr + m_size));

  void* const ptr = mmap(map_base, map_size, static_cast<int>(mode), MAP_SHARED | MAP_FIXED,
                         static_cast<int>(reinterpret_cast<intptr_t>(file_handle)), static_cast<off_t>(file_offset));
  if (ptr == MAP_FAILED)
    return nullptr;

  m_num_mappings++;
  return static_cast<u8*>(ptr);
}

bool SharedMemoryMappingArea::Unmap(void* map_base, size_t map_size)
{
  DebugAssert(static_cast<u8*>(map_base) >= m_base_ptr && static_cast<u8*>(map_base) < (m_base_ptr + m_size));

  if (mmap(map_base, map_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED)
    return false;

  m_num_mappings--;
  return true;
}

#endif

#if defined(__APPLE__) && defined(__aarch64__)

static thread_local int s_code_write_depth = 0;

void MemMap::BeginCodeWrite()
{
  // Log_DebugFmt("BeginCodeWrite(): {}", s_code_write_depth);
  if ((s_code_write_depth++) == 0)
  {
    // Log_DebugPrint("  pthread_jit_write_protect_np(0)");
    pthread_jit_write_protect_np(0);
  }
}

void MemMap::EndCodeWrite()
{
  // Log_DebugFmt("EndCodeWrite(): {}", s_code_write_depth);

  DebugAssert(s_code_write_depth > 0);
  if ((--s_code_write_depth) == 0)
  {
    // Log_DebugPrint("  pthread_jit_write_protect_np(1)");
    pthread_jit_write_protect_np(1);
  }
}

#endif
