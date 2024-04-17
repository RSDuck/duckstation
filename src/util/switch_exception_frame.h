#pragma once

#include "common/types.h"

struct ExceptionFrameA64
{
  u64 x[9];
  u64 lr, sp, pc;
  u32 pstate, afsr0, afsr1, esr;
  u64 far;
};
