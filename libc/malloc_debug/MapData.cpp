/*
 * Copyright (C) 2012 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <ctype.h>
#include <elf.h>
#include <inttypes.h>
#include <link.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <vector>

#include "MapData.h"

// Format of /proc/<PID>/maps:
//   6f000000-6f01e000 rwxp 00000000 00:0c 16389419   /system/lib/libcomposer.so
static MapEntry* parse_line(char* line) {
  uintptr_t start;
  uintptr_t end;
  uintptr_t offset;
  int flags;
  char permissions[5];
  int name_pos;
  if (sscanf(line, "%" PRIxPTR "-%" PRIxPTR " %4s %" PRIxPTR " %*x:%*x %*d %n", &start, &end,
             permissions, &offset, &name_pos) < 2) {
    return nullptr;
  }

  const char* name = line + name_pos;
  size_t name_len = strlen(name);
  if (name_len && name[name_len - 1] == '\n') {
    name_len -= 1;
  }

  flags = 0;
  if (permissions[0] == 'r') {
    flags |= PROT_READ;
  }
  if (permissions[2] == 'x') {
    flags |= PROT_EXEC;
  }

  MapEntry* entry = new MapEntry(start, end, offset, name, name_len, flags);
  if (!(flags & PROT_READ)) {
    // Any unreadable map will just get a zero load bias.
    entry->load_bias = 0;
    entry->init = true;
    entry->valid = false;
  }
  return entry;
}

template <typename T>
static inline bool get_val(MapEntry* entry, uintptr_t addr, T* store) {
  if (!(entry->flags & PROT_READ) || addr < entry->start || addr + sizeof(T) > entry->end) {
    return false;
  }
  // Make sure the address is aligned properly.
  if (addr & (sizeof(T) - 1)) {
    return false;
  }
  *store = *reinterpret_cast<T*>(addr);
  return true;
}

static bool valid_elf(MapEntry* entry) {
  uintptr_t addr = entry->start;
  uintptr_t end;
  if (__builtin_add_overflow(addr, SELFMAG, &end) || end >= entry->end) {
    return false;
  }

  return memcmp(reinterpret_cast<void*>(addr), ELFMAG, SELFMAG) == 0;
}

static void read_loadbias(MapEntry* entry) {
  entry->load_bias = 0;
  uintptr_t addr = entry->start;
  ElfW(Ehdr) ehdr;
  if (!get_val<ElfW(Half)>(entry, addr + offsetof(ElfW(Ehdr), e_phnum), &ehdr.e_phnum)) {
    return;
  }
  if (!get_val<ElfW(Off)>(entry, addr + offsetof(ElfW(Ehdr), e_phoff), &ehdr.e_phoff)) {
    return;
  }
  addr += ehdr.e_phoff;
  for (size_t i = 0; i < ehdr.e_phnum; i++) {
    ElfW(Phdr) phdr;
    if (!get_val<ElfW(Word)>(entry, addr + offsetof(ElfW(Phdr), p_type), &phdr.p_type)) {
      return;
    }
    if (!get_val<ElfW(Off)>(entry, addr + offsetof(ElfW(Phdr), p_offset), &phdr.p_offset)) {
      return;
    }
    if (phdr.p_type == PT_LOAD && phdr.p_offset == entry->offset) {
      if (!get_val<ElfW(Addr)>(entry, addr + offsetof(ElfW(Phdr), p_vaddr), &phdr.p_vaddr)) {
        return;
      }
      entry->load_bias = phdr.p_vaddr;
      return;
    }
    addr += sizeof(phdr);
  }
}

static void inline init(MapEntry* entry) {
  if (entry->init) {
    return;
  }
  entry->init = true;
  if (valid_elf(entry)) {
    entry->valid = true;
    read_loadbias(entry);
  }
}

bool MapData::ReadMaps() {
  FILE* fp = fopen("/proc/self/maps", "re");
  if (fp == nullptr) {
    return false;
  }

  std::vector<char> buffer(1024);
  while (fgets(buffer.data(), buffer.size(), fp) != nullptr) {
    MapEntry* entry = parse_line(buffer.data());
    if (entry == nullptr) {
      fclose(fp);
      return false;
    }

    auto it = entries_.find(entry);
    if (it == entries_.end()) {
      entries_.insert(entry);
    } else {
      delete entry;
    }
  }
  fclose(fp);
  return true;
}

MapData::~MapData() {
  for (auto* entry : entries_) {
    delete entry;
  }
  entries_.clear();
}

// Find the containing map info for the PC.
const MapEntry* MapData::find(uintptr_t pc, uintptr_t* rel_pc) {
  MapEntry pc_entry(pc);

  std::lock_guard<std::mutex> lock(m_);

  auto it = entries_.find(&pc_entry);
  if (it == entries_.end()) {
    ReadMaps();
  }
  it = entries_.find(&pc_entry);
  if (it == entries_.end()) {
    return nullptr;
  }

  MapEntry* entry = *it;
  init(entry);

  if (rel_pc != nullptr) {
    // Need to check to see if this is a read-execute map and the read-only
    // map is the previous one.
    if (!entry->valid && it != entries_.begin()) {
      MapEntry* prev_entry = *--it;
      if (prev_entry->flags == PROT_READ && prev_entry->offset < entry->offset &&
          prev_entry->name == entry->name) {
        init(prev_entry);

        if (prev_entry->valid) {
          entry->elf_start_offset = prev_entry->offset;
          *rel_pc = pc - entry->start + entry->offset + prev_entry->load_bias;
          return entry;
        }
      }
    }
    *rel_pc = pc - entry->start + entry->offset + entry->load_bias;
  }
  return entry;
}
