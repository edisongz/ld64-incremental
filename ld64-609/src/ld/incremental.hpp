//
//  incremental.hpp
//  ld
//
//  Created by tttt on 2022/2/14.
//  Copyright © 2022 Apple Inc. All rights reserved.
//

#ifndef __INCREMENTAL_HPP__
#define __INCREMENTAL_HPP__

#include "macho_incremental_file.hpp"

namespace ld {
namespace incremental {

class Incremental {
 public:
  explicit Incremental(Options &options)
      : _options(options), wholeBuffer_(nullptr) {}
  uint8_t *wholeBuffer() { return wholeBuffer_; }
  void openBinary();
  void closeBinary();
  constexpr uint64_t baseAddress() const { return baseAddress_; }
  constexpr uint32_t objcClassSectionOffset(const char *className) {
    return objcClassSectionOffsetMap_[className];
  }
  constexpr PatchSpace &patchSpace(const char *sectName) {
    return patchSpace_[sectName];
  }
  // Compatible for legacy code
  void forEachStubAtom(ld::File::AtomHandler &handler, ld::Internal &state);
  void forEachStubAtom(const std::function<void(const ld::Atom *)> &handler);
  void forEachRefsAtom(ld::File::AtomHandler &handler, ld::Internal &state);
  bool containsStubName(const char *name) const {
    return stubNames_.find(name) != stubNames_.end();
  }
  void forEachSegmentBoundary(
      const std::function<void(SegmentBoundary &, uint32_t)> &handler);
  void forEachRebaseInfo(
      const std::function<void(std::pair<uint8_t, uint64_t> &)> &handler);
  void forEachBindingInfo(
      const std::function<void(BindingInfoTuple &)> &handler);
  void forEachLazyBindingInfo(
      const std::function<void(BindingInfoTuple &)> &handler);
  constexpr std::vector<IncrFixup> &findRelocations(const char *atomName) {
    return incrFixupsMap_[atomName];
  }

  SectionBoundary &sectionBoundary(const char *sectName) {
    return sectionBoundaryMap_[sectName];
  }

  constexpr uint64_t sectionStartAddress(const char *sectName) {
    return sectionBoundary(sectName).address_;
  }
  constexpr uint64_t sectionFileOffset(const char *sectName) {
    return sectionBoundary(sectName).fileOffset_;
  }
  constexpr uint64_t sectionPatchFileOffset(const char *sectName) {
    return sectionBoundary(sectName).fileOffset_ +
           patchSpace_[sectName].patchOffset_;
  }

  constexpr std::vector<std::pair<uint8_t, uint64_t>> &rebaseInfo() {
    return rebaseInfo_;
  }
  bool containsRebaseAddress(uint64_t addr) const {
    return rebaseAddresses_.find(addr) != rebaseAddresses_.end();
  }

  constexpr std::map<const ld::dylib::File *, int> &dylibToOrdinal() {
    return dylibToOrdinal_;
  }

  void updateDylibOrdinal(
      std::map<const ld::dylib::File *, int> &dylibToOrdinal,
      ld::dylib::File *dylib);

  uint64_t symSectionOffset(uint8_t type, const char *symbol) {
    auto &offsetMap = symToSectionOffset_[type];
    if (offsetMap.find(symbol) != offsetMap.end()) {
      return offsetMap[symbol];
    }
    return ULONG_MAX;
  }

  uint32_t symbolOffsetForType(uint8_t type) {
    return symbolTypeToOffset_[type];
  }

  void addSymSectionOffset(uint8_t type, const char *symbol);

  uint32_t symbolIndexInStrings(const char *symbol) const {
    auto it = stringPool_.find(symbol);
    if (it != stringPool_.end()) {
      return it->second;
    }
    return UINT_MAX;
  }

  uint32_t addUnique(const char *symbol);

  void forEachAppendedString(
      const std::function<void(const std::string &)> &handler);

  void UpdateIndirectSymbolIndex(const char *sectionName, uint32_t index);

 private:
  Options &_options;
  int fd_;
  uint64_t baseAddress_;
  uint8_t *wholeBuffer_;
  size_t machoNlistSize_{0};
  uint32_t symbolCount_{0};
  /// ObjC class section offset map
  std::unordered_map<std::string, uint32_t> objcClassSectionOffsetMap_;
  IncrFixupsMap incrFixupsMap_;
  std::unordered_map<std::string, PatchSpace> patchSpace_;
  std::vector<const ld::Atom *> stubAtoms_;
  std::vector<const ld::Atom *> objcClassRefsAtoms_;
  std::unordered_set<std::string> stubNames_;
  std::vector<SegmentBoundary> segmentBoundaries_;
  std::unordered_map<std::string, SectionBoundary> sectionBoundaryMap_;
  std::vector<std::pair<uint8_t, uint64_t>> rebaseInfo_;
  std::unordered_set<uint64_t> rebaseAddresses_;
  std::vector<BindingInfoTuple> bindingInfo_;
  std::vector<BindingInfoTuple> lazyBindingInfo_;
  std::map<const ld::dylib::File *, int> dylibToOrdinal_;
  std::unordered_map<std::string, int> dylibNameToOrdinal_;
  SymbolSectionOffset symToSectionOffset_;
  std::unordered_map<uint8_t, uint32_t> symbolTypeToOffset_;
  std::unordered_map<std::string, uint32_t> stringPool_;
  std::vector<std::string> appendStrings_;
  uint32_t currentBufferUsed_{0};
#if SUPPORT_ARCH_arm64
  macho_section<arm64::P> *got_section_{nullptr};
  macho_section<arm64::P> *la_symbol_ptr_section_{nullptr};
#endif
};

}  // namespace incremental
}  // namespace ld

#endif  // __INCREMENTAL_HPP__
