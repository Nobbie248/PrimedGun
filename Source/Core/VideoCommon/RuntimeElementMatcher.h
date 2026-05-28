#pragma once

#include "VideoCommon/ShaderHunter.h"

namespace RuntimeElementMatcher
{
bool HasActiveGroups(const ShaderHunter::RuntimeElementSignature& signature);
bool Matches(const ShaderHunter::RuntimeElementSignature& pattern,
             const ShaderHunter::RuntimeElementSignature& candidate);
}
