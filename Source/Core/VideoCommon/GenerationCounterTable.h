// Copyright 2026 PrimedGun Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"

namespace VideoCommon
{
// Open-addressing occurrence counter keyed by u64. Clear() bumps a generation instead of
// releasing slots, so a table that is cleared every frame allocates nothing in steady state.
// Not thread-safe; the owner provides any locking it needs.
class GenerationCounterTable
{
public:
  // Returns the number of prior increments of this key since the last Clear().
  int Increment(u64 key)
  {
    if (m_slots.empty() || (m_live + 1) * 2 > m_slots.size())
      Grow();

    const size_t mask = m_slots.size() - 1;
    size_t index = Mix(key) & mask;
    while (true)
    {
      Slot& slot = m_slots[index];
      if (slot.generation != m_generation)
      {
        slot.key = key;
        slot.generation = m_generation;
        slot.count = 1;
        ++m_live;
        return 0;
      }
      if (slot.key == key)
        return slot.count++;
      index = (index + 1) & mask;
    }
  }

  void Clear()
  {
    m_live = 0;
    if (++m_generation != 0)
      return;
    // Generation wrapped: every slot must look empty again.
    for (Slot& slot : m_slots)
      slot.generation = 0;
    m_generation = 1;
  }

  size_t GetLiveCount() const { return m_live; }
  size_t GetCapacity() const { return m_slots.size(); }

private:
  struct Slot
  {
    u64 key = 0;
    u32 generation = 0;
    int count = 0;
  };

  static size_t Mix(u64 key)
  {
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    key *= 0xc4ceb9fe1a85ec53ULL;
    key ^= key >> 33;
    return static_cast<size_t>(key);
  }

  void Grow()
  {
    std::vector<Slot> old_slots = std::move(m_slots);
    m_slots.assign(old_slots.empty() ? INITIAL_CAPACITY : old_slots.size() * 2, Slot{});
    const size_t mask = m_slots.size() - 1;
    for (const Slot& old_slot : old_slots)
    {
      if (old_slot.generation != m_generation)
        continue;
      size_t index = Mix(old_slot.key) & mask;
      while (m_slots[index].generation == m_generation)
        index = (index + 1) & mask;
      m_slots[index] = old_slot;
    }
  }

  static constexpr size_t INITIAL_CAPACITY = 1024;

  std::vector<Slot> m_slots;
  u32 m_generation = 1;
  size_t m_live = 0;
};
}  // namespace VideoCommon
