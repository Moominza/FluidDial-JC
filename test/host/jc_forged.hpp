// Instruction 010A test helper: the firmware has NO builder for program start / resume. These helpers forge such
// commands directly so tests can prove every layer (policy, gate, dispatcher, transports) still refuses them.
// Never include this from src/.
#pragma once

#include "../../src/jc8048w550/JcCommandBuilder.hpp"

namespace jc8048w550 {
namespace forged {

inline JcCommand run_sd(const char* path) { return detail::make(JcCommandClass::Job, "$SD/Run=%s", path); }
inline JcCommand run_sd_as(JcCommandClass cls, const char* path) { return detail::make(cls, "$SD/Run=%s", path); }
inline JcCommand run_macro(const char* path) { return detail::make(JcCommandClass::Macro, "$Localfs/Run=%s", path); }
inline JcCommand cycle_resume() { return detail::realtime(JcCommandClass::Job, '~'); }
inline JcCommand cycle_resume_as(JcCommandClass cls) { return detail::realtime(cls, '~'); }

}  // namespace forged
}  // namespace jc8048w550
