#pragma once

// Constants for the offset locators, which decode a disp32 straight out of a
// `mov [base+disp32], reg` or `mov reg, [base+disp32]` instruction.

#include "libmem/libmem.h"

#include <cstdint>


// Byte offset of the disp32 inside the anchored instruction.  This depends on
// how many opcode bytes precede it, NOT on the architecture as a whole, so each
// locator declares its own value:
//
//   89 86 <disp32>        i386   m_ClientUtils, m_pUser   -> +2
//   48 89 83 <disp32>     x86-64 m_ClientUtils            -> +3  (REX)
//   8B B7 <disp32>        x86-64 m_PipeIndex              -> +2  (no REX)
//   8B 91 <disp32>        i386   m_PipeIndex              -> +2
//
// A single per-arch constant is therefore wrong: x86-64 mixes both forms.
constexpr lm_address_t kDispAfterOpcodeModrm = 2;  // opcode + ModRM
constexpr lm_address_t kDispAfterRexOpcodeModrm = 3;  // REX + opcode + ModRM
