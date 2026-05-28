// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

namespace HideObjectEngine
{

enum class HideObjectType
{
  Bits8 = 0,
  Bits16,
  Bits24,
  Bits32,
  Bits40,
  Bits48,
  Bits56,
  Bits64,
  Bits72,
  Bits80,
  Bits88,
  Bits96,
  Bits104,
  Bits112,
  Bits120,
  Bits128,
  Count = 16
};

// Returns byte count for a given type (Bits8 = 1 byte, Bits16 = 2, ... Bits128 = 16)
inline int GetByteCount(HideObjectType type)
{
  return static_cast<int>(type) + 1;
}

// String names for each type (for UI and INI serialization)
const char* GetTypeName(HideObjectType type);
HideObjectType ParseTypeName(const std::string& name);

struct HideObjectEntry
{
  HideObjectType type = HideObjectType::Bits16;
  u64 value_upper = 0;  // High bytes for types > 64 bits
  u64 value_lower = 0;  // Low bytes (always used)
};

struct HideObject
{
  std::string name;
  std::vector<HideObjectEntry> entries;  // Usually 1, but format allows multiple per code
  bool active = true;
  bool user_defined = true;
};

// Flattened byte array for fast memcmp matching
using SkipEntry = std::vector<u8>;

// --- Static INI helpers (direct file I/O, bypasses IniFile to avoid corruption) ---
std::vector<HideObject> LoadFromINI(const std::string& game_id);
void SaveToINI(const std::string& game_id, const std::vector<HideObject>& codes);

// --- Runtime singleton for hide-object matching ---
class Engine
{
public:
  static Engine& GetInstance();

  // Load codes from per-game INI and apply active ones
  void LoadCodes(const std::string& game_id);
  void LoadCodesIfNeeded(const std::string& game_id);

  // Convert HideObject list to flattened byte patterns for matching
  void ApplyCodes(const std::vector<HideObject>& codes);

  // Called from VertexLoaderManager::RunVertices — returns true if src matches any active code
  bool ShouldHide(const u8* src) const;
  bool HasCodes() const;

private:
  Engine() = default;

  mutable std::mutex m_mutex;
  std::vector<SkipEntry> m_active_entries;  // Flattened byte patterns for memcmp
  std::string m_loaded_game_id;

  // Thread-safety coordination between UI updates and render-thread checks
  std::atomic<bool> m_updating{false};
  std::atomic<bool> m_done{true};
};

}  // namespace HideObjectEngine
