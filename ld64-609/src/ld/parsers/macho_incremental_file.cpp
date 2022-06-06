//
//  macho_incremental_file.cpp
//  ld
//
//  Created by tttt on 2022/6/6.
//  Copyright © 2022 Apple Inc. All rights reserved.
//

#include "macho_incremental_file.hpp"

namespace ld {
namespace incremental {

ld::Section RefsProxyAtom::_s_section("__DATA", "__objc_classrefs",
                                      ld::Section::typePageZero, true);

ld::Section IncrementalAtom::_s_section("__PAGEZERO", "__pagezero", ld::Section::typeUnclassified, true);

template <>
bool Parser<x86>::validFile(const uint8_t *fileContent) {
  const macho_header<P> *header = reinterpret_cast<const macho_header<P> *>(fileContent);
  if (header->magic() != MH_MAGIC) return false;
  if (header->cputype() != CPU_TYPE_I386) return false;
  switch (header->filetype()) {
    case MH_EXECUTE:
    case MH_DYLIB:
    case MH_BUNDLE:
    case MH_DYLINKER:
      return true;
  }
  return false;
}

template <>
bool Parser<x86_64>::validFile(const uint8_t *fileContent) {
  const macho_header<P> *header = reinterpret_cast<const macho_header<P> *>(fileContent);
  if (header->magic() != MH_MAGIC_64) return false;
  if (header->cputype() != CPU_TYPE_X86_64) return false;
  switch (header->filetype()) {
    case MH_EXECUTE:
    case MH_DYLIB:
    case MH_BUNDLE:
    case MH_DYLINKER:
      return true;
  }
  return false;
}

#if SUPPORT_ARCH_arm_any
template <>
bool Parser<arm>::validFile(const uint8_t *fileContent) {
  const macho_header<P> *header = reinterpret_cast<const macho_header<P> *>(fileContent);
  if (header->magic() != MH_MAGIC) return false;
  if (header->cputype() != CPU_TYPE_ARM) return false;
  switch (header->filetype()) {
    case MH_EXECUTE:
    case MH_DYLIB:
    case MH_BUNDLE:
    case MH_DYLINKER:
      return true;
  }
  return false;
}
#endif  // SUPPORT_ARCH_arm_any

#if SUPPORT_ARCH_arm64
template <>
bool Parser<arm64>::validFile(const uint8_t *fileContent) {
  const macho_header<P> *header = reinterpret_cast<const macho_header<P> *>(fileContent);
  if (header->magic() != MH_MAGIC_64) return false;
  if (header->cputype() != CPU_TYPE_ARM64) return false;
  switch (header->filetype()) {
    case MH_EXECUTE:
    case MH_DYLIB:
    case MH_BUNDLE:
    case MH_DYLINKER:
      return true;
  }
  return false;
}
#endif  // SUPPORT_ARCH_arm64

#if SUPPORT_ARCH_arm64_32
template <>
bool Parser<arm64_32>::validFile(const uint8_t *fileContent) {
  const macho_header<P> *header = reinterpret_cast<const macho_header<P> *>(fileContent);
  if (header->magic() != MH_MAGIC) return false;
  if (header->cputype() != CPU_TYPE_ARM64_32) return false;
  switch (header->filetype()) {
    case MH_EXECUTE:
    case MH_DYLIB:
    case MH_BUNDLE:
    case MH_DYLINKER:
      return true;
  }
  return false;
}
#endif  // SUPPORT_ARCH_arm64_32

template <>
uint8_t Parser<ppc>::loadCommandSizeMask() {
  return 0x03;
}
template <>
uint8_t Parser<ppc64>::loadCommandSizeMask() {
  return 0x07;
}
template <>
uint8_t Parser<x86>::loadCommandSizeMask() {
  return 0x03;
}
template <>
uint8_t Parser<x86_64>::loadCommandSizeMask() {
  return 0x07;
}
template <>
uint8_t Parser<arm>::loadCommandSizeMask() {
  return 0x03;
}
template <>
uint8_t Parser<arm64>::loadCommandSizeMask() {
  return 0x07;
}
#if SUPPORT_ARCH_arm64_32
template <>
uint8_t Parser<arm64_32>::loadCommandSizeMask() {
  return 0x03;
}
#endif

}  // namespace incremental
}  // namespace ld
