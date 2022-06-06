//
//  macho_incremental_file.hpp
//  ld
//
//  Created by tttt on 2022/6/6.
//  Copyright © 2022 Apple Inc. All rights reserved.
//

#ifndef MACHO_INCREMENTAL_FILE_HPP_
#define MACHO_INCREMENTAL_FILE_HPP_

#include <algorithm>
#include <list>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "Architectures.hpp"
#include "InputFiles.h"
#include "configure.h"
#include "generic_dylib_file.hpp"
#include "macho_incremental_abstraction.hpp"

namespace ld {
namespace incremental {

using IncrFixupsMap = std::unordered_map<std::string, std::vector<IncrFixup>>;
using SymbolSectionOffset =
    std::unordered_map<uint8_t, std::unordered_map<std::string, uint64_t>>;
using BindingInfoTuple =
    std::tuple<uint8_t, int, const char *, bool, uint64_t, int64_t>;

static inline uint64_t read_uleb128(const uint8_t *&p, const uint8_t *end) {
  uint64_t result = 0;
  int bit = 0;
  do {
    if (p == end) {
      throwf("malformed uleb128");
    }
    uint64_t slice = *p & 0x7f;
    if (bit >= 64 || slice << bit >> bit != slice) {
      throwf("uleb128 too big");
    } else {
      result |= (slice << bit);
      bit += 7;
    }
  } while (*p++ & 0x80);
  return result;
}

static inline int64_t read_sleb128(const uint8_t *&p, const uint8_t *end) {
  int64_t result = 0;
  int bit = 0;
  uint8_t byte;
  do {
    if (p == end) throwf("malformed sleb128");
    byte = *p++;
    result |= (((int64_t)(byte & 0x7f)) << bit);
    bit += 7;
  } while (byte & 0x80);
  // sign extend negative numbers
  if (((byte & 0x40) != 0) && (bit < 64)) result |= (-1LL) << bit;
  return result;
}

static inline const char *dylibShortName(const char *leafName) {
  const char *shortName = strstr(leafName, ".");
  size_t length = strlen(leafName);
  char *dylibName = new char[length + 1];
  if (shortName) {
    length = uintptr_t(shortName) - uintptr_t(leafName);
  }
  strncpy(dylibName, leafName, length);
  *(dylibName + length) = '\0';
  return dylibName;
}

template <typename A>
class ObjCClass {
 public:
  typedef typename A::P::uint_t pint_t;
  const char *objcClassName(uint64_t addressInClassList) const;

#pragma pack(1)
  /// __objc_data
  struct Content {
    pint_t isa;
    pint_t superclass;
    pint_t method_cache;
    pint_t vtable;
    pint_t data;
  };

  struct ROContent {
    uint32_t flags;
    uint32_t instanceStart;
    // Note there is 4-bytes of alignment padding between instanceSize
    // and ivarLayout on 64-bit archs, but no padding on 32-bit archs.
    // This union is a way to model that.
    union {
      uint32_t instanceSize;
      pint_t pad;
    } instanceSize;
    pint_t ivarLayout;
    pint_t name;
    pint_t baseMethods;
    pint_t baseProtocols;
    pint_t ivars;
    pint_t weakIvarLayout;
    pint_t baseProperties;
  };
#pragma pack()
};

//
// An ExportAtom has no content.  It exists so that the linker can track which
// imported symbols came from which dynamic libraries.
//
class RefsProxyAtom : public ld::Atom {
 public:
  RefsProxyAtom(const char *nm, uint64_t size)
      : ld::Atom(_s_section, ld::Atom::definitionRegular,
                 ld::Atom::combineNever, ld::Atom::scopeLinkageUnit,
                 ld::Atom::typeZeroFill, ld::Atom::symbolTableNotIn, false,
                 false, false, ld::Atom::Alignment(0)),
        _name(nm),
        size_(size) {}
  // overrides of ld::Atom
  virtual const ld::File *file() const { return nullptr; }
  virtual const char *name() const { return _name; }
  virtual uint64_t size() const { return size_; }
  virtual uint64_t objectAddress() const { return 0; }
  virtual void copyRawContent(uint8_t buffer[]) const {}
  virtual void setScope(Scope) {}

 protected:
  virtual ~RefsProxyAtom() {}
  const char *_name;
  uint64_t size_;
  static ld::Section _s_section;
};

template <typename A>
class Parser {
 public:
  typedef typename A::P P;
  typedef typename A::P::E E;
  typedef typename A::P::uint_t pint_t;
  using IncrInputMap = std::unordered_map<std::string, InputEntrySection<P> *>;
  using IncrPatchSpaceMap = std::unordered_map<std::string, PatchSpace>;

  static bool validFile(const uint8_t *fileContent);

  Parser(const uint8_t *fileContent, uint64_t fileLength,
         const Options &options, time_t modTime);
  bool hasValidEntryPoint() const { return entryPoint_ != nullptr; }
  bool canIncrementalUpdate();
  /// Symol table item size
  size_t MachONlistSize() const { return sizeof(macho_nlist<P>); }
  uint32_t symbolCount() const { return symbolCount_; }
  IncrInputMap &incrInputsMap() { return incrInputsMap_; }
  constexpr std::unordered_map<std::string, uint32_t> &objcClassIndexMap() {
    return objcClassIndexMap_;
  }
  constexpr IncrFixupsMap &incrFixupsMap() { return incrFixupsMap_; }
  constexpr IncrPatchSpaceMap &patchSpaceMap() { return incrPatchSpaceMap_; }
  constexpr std::vector<const ld::Atom *> &stubAtoms() { return stubAtoms_; }
  constexpr std::vector<const ld::Atom *> &objcClassRefsAtoms() {
    return objcClassRefsAtoms_;
  }
  constexpr std::unordered_set<std::string> &stubNames() { return stubNames_; }
  constexpr uint64_t baseAddress() const { return baseAddress_; }
  constexpr std::vector<SegmentBoundary> &segmentBoundaries() {
    return segmentBoundaries_;
  }

  constexpr std::unordered_map<std::string, SectionBoundary>
      &sectionBoundaryMap() {
    return sectionBoundaryMap_;
  }

  constexpr std::vector<std::pair<uint8_t, uint64_t>> &rebaseInfo() {
    return rebaseInfo_;
  }
  constexpr std::unordered_set<uint64_t> &rebaseAddresses() {
    return rebaseAddresses_;
  }

  constexpr std::vector<BindingInfoTuple> &bindingInfo() {
    return bindingInfo_;
  }

  constexpr std::vector<BindingInfoTuple> &lazyBindingInfo() {
    return lazyBindingInfo_;
  }

  constexpr std::map<const ld::dylib::File *, int> &dylibToOrdinal() {
    return dylibToOrdinal_;
  }

  constexpr std::unordered_map<std::string, int> &dylibNameToOrdinal() {
    return dylibNameToOrdinal_;
  }

  constexpr SymbolSectionOffset &symToSectionOffset() {
    return symToSectionOffset_;
  }

  constexpr std::unordered_map<uint8_t, uint32_t> &symbolTypeToOffset() {
    return symbolTypeToOffset_;
  }

  constexpr std::unordered_map<std::string, uint32_t> &stringPool() {
    return stringPool_;
  }

  constexpr uint32_t currentBufferUsed() const { return currentBufferUsed_; }

  macho_section<P> *GotSection() const { return got_section_; }

  macho_section<P> *LazySymbolPtrSection() const {
    return la_symbol_ptr_section_;
  }

 private:
  void checkMachOHeader();
  pint_t segStartAddress(uint8_t segIndex);
  bool isStaticExecutable() const;
  uint8_t loadCommandSizeMask();

  /// Record MachO section boundary
  void recordSectionBoudary(const char *symbol, uint64_t fileOffset,
                            uint32_t size) {
    SectionBoundary sectionBoundary;
    sectionBoundary.address_ = baseAddress() + fileOffset;
    sectionBoundary.fileOffset_ = fileOffset;
    sectionBoundary.size_ = size;
    sectionBoundaryMap_[symbol] = sectionBoundary;
  }

  /// Parse Symbols
  void parseSymbolTable(const macho_load_command<P> *cmd);
  void parseIndirectSymbolTable();
  uint32_t indirectSymbol(uint32_t indirectIndex) const;
  const macho_nlist<P> &symbolFromIndex(uint32_t index);
  const char *nameFromSymbol(const macho_nlist<P> &sym);
  bool weakImportFromSymbol(const macho_nlist<P> &sym);

  /// Passe segment __TEXT
  void parseTextSegment(const macho_segment_command<P> *segCmd);
  void parseObjCClassName(const macho_section<P> *sect);

  /// Parse segment __DATA_CONST
  void parseDataConstSegment(const macho_segment_command<P> *segCmd);
  /// Parse ObjC classlist
  void parseObjcClassList(const macho_section<P> *sect);

  /// Parse segment __DATA
  void parseDataSegment(const macho_segment_command<P> *segCmd);
  void parseObjCClassRefs(const macho_section<P> *sect);
  void parseObjCData(const macho_section<P> *sect);

  /// Parse dynamic loader info
  void parseDyldInfoSegment(const macho_dyld_info_command<P> *segCmd);
  /// Parse __rebase section
  void parseRebaseSection(const macho_dyld_info_command<P> *segCmd);
  /// Parse __binding section
  void parseBindingSection(const macho_dyld_info_command<P> *segCmd,
                           std::vector<BindingInfoTuple> &bindingInfo,
                           bool weakBinding);
  /// Parse __lazy_binding section
  void ParseLazyBindingSection(const macho_dyld_info_command<P> *segCmd);

  /// Parse Incremental sections
  void parseSections();

  /// Parse incremental fixup section
  /// @param incrementalCommand LC_INCREMENTAL
  void parseIncrementalFixupSection(
      const IncrementalCommand<P> *incrementalCommand);

  /// Parse incremental inputs section
  /// @param incrementalCommand LC_INCREMENTAL
  void parseIncrementalInputsSection(
      const IncrementalCommand<P> *incrementalCommand);

  /// Parse incremental patch space section
  /// @param incrementalCommand LC_INCREMENTAL
  void parseIncrementalPatchSpaceSection(
      const IncrementalCommand<P> *incrementalCommand);

  /// Parse incremental global symbol table
  /// @param incrementalCommand LC_INCREMENTAL
  void parseIncrementalGlobalSymbols(
      const IncrementalCommand<P> *incrementalCommand);

  /// Parse incremental string pool
  /// @param incrementalCommand LC_INCREMENTAL
  void parseIncrementalStringPool(
      const IncrementalCommand<P> *incrementalCommand);

  const uint8_t *fileContent_;
  uint32_t fileLength_;
  const Options &options_;
  uint64_t baseAddress_;
  const macho_header<P> *fHeader_;
  macho_section<P> *got_section_;
  macho_section<P> *la_symbol_ptr_section_;
  const macho_section<P> *objcClassRefsSection_;
  const macho_dyld_info_command<P> *dyldInfo_;
  const macho_entry_point_command<P> *entryPoint_;
  const macho_segment_command<P> *linkEditSegment_;
  const macho_dysymtab_command<P> *fDynamicSymbolTable_;
  const macho_nlist<P> *symbolTable_;
  uint32_t symbolCount_;
  const InputEntrySection<P> *fIncrementalInputSection_;
  const InputFileFixupSection<P> *incrementalFixupSection_;
  const PatchSpaceSectionEntry<P> *fIncrementalPatchSpaceSection_;
  const GlobalSymbolTableEntry<P> *fIncrementalSymbolSection_;
  const char *stringTable_;
  const char *stringTableEnd_;
  std::unordered_map<std::string, uint32_t> stringPool_;
  uint32_t currentBufferUsed_;
  const uint32_t *indirectSymbolTable_;
  uint32_t indirectTableCount_;
  const char *fIncrementalStrings_;
  bool fSlidableImage_;
  std::vector<InputEntrySection<P> *> incrInputs_;
  std::unordered_map<std::string, InputEntrySection<P> *> incrInputsMap_;
  std::vector<GlobalSymbolTableEntry<P> *> incrSymbols_;
  std::vector<std::string> incrStringPool_;
  IncrPatchSpaceMap incrPatchSpaceMap_;
  std::vector<const ld::Atom *> stubAtoms_;
  std::vector<const ld::Atom *> refsAtoms_;
  std::unordered_set<std::string> stubNames_;
  /// MachO sections boundary
  std::unordered_map<std::string, SectionBoundary> sectionBoundaryMap_;
  /// ObjC class address
  std::vector<uint64_t> objcClassAddresses_;
  std::unordered_map<uint64_t, uint32_t> objcClassSectionOffsetMap_;
  /// ObjC class index map
  std::unordered_map<std::string, uint32_t> objcClassIndexMap_;

  /// ObjC class refs
  std::vector<const ld::Atom *> objcClassRefsAtoms_;

  /// Incremental fixups map
  IncrFixupsMap incrFixupsMap_;
  /// All Segments
  std::vector<const macho_segment_command<P> *> fSegments_;
  /// Segment boundary
  std::vector<SegmentBoundary> segmentBoundaries_;
  /// Dyld rebase info
  std::vector<std::pair<uint8_t, uint64_t>> rebaseInfo_;
  std::unordered_set<uint64_t> rebaseAddresses_;
  /// Dyld binding info
  std::vector<BindingInfoTuple> bindingInfo_;
  /// Dyld weak binding info
  std::vector<BindingInfoTuple> weakBindingInfo_;
  /// Dyld lazy binding info
  std::vector<BindingInfoTuple> lazyBindingInfo_;
  /// LOAD_DYLIB
  std::vector<const macho_dylib_command<P> *> dylibLoadCommands_;
  /// Dylib ordinal map
  std::map<const ld::dylib::File *, int> dylibToOrdinal_;
  std::unordered_map<std::string, int> dylibNameToOrdinal_;
  std::unordered_map<std::string, const macho_nlist<P> *> dylibSymbolMap_;
  /// Symbol table section offset
  SymbolSectionOffset symToSectionOffset_;
  std::unordered_map<uint8_t, uint32_t> symbolTypeToOffset_;
  std::unordered_map<uint64_t, uint32_t> symbolAddressToIndex_;
};

template <typename A>
Parser<A>::Parser(const uint8_t *fileContent, uint64_t fileLength,
                  const Options &options, time_t modTime)
    : fileContent_(fileContent),
      fileLength_(fileLength),
      options_(options),
      baseAddress_(0),
      fHeader_(nullptr),
      dyldInfo_(nullptr),
      entryPoint_(nullptr),
      fDynamicSymbolTable_(nullptr),
      symbolTable_(nullptr),
      symbolCount_(0),
      fIncrementalInputSection_(nullptr),
      incrementalFixupSection_(nullptr),
      fIncrementalPatchSpaceSection_(nullptr),
      fIncrementalSymbolSection_(nullptr),
      stringTable_(nullptr),
      stringTableEnd_(nullptr),
      indirectSymbolTable_(nullptr),
      indirectTableCount_(0),
      fIncrementalStrings_(nullptr) {
  if (!validFile(fileContent)) {
    throw "not a mach-o file that can be checked";
  }
  fHeader_ = reinterpret_cast<const macho_header<P> *>(fileContent_);
  this->checkMachOHeader();
  this->parseSections();
  this->parseDyldInfoSegment(dyldInfo_);
  this->parseObjCClassRefs(objcClassRefsSection_);
  this->parseIndirectSymbolTable();
}

template <typename A>
bool Parser<A>::canIncrementalUpdate() {
  return incrInputs_.size() > 0;
}

template <typename A>
typename A::P::uint_t Parser<A>::segStartAddress(uint8_t segIndex) {
  if (segIndex > fSegments_.size()) {
    throw "segment index out of range";
  }
  return fSegments_[segIndex]->vmaddr();
}

template <typename A>
void Parser<A>::checkMachOHeader() {
  if ((fHeader_->sizeofcmds() + sizeof(macho_header<P>)) > fileLength_) {
    throw "sizeofcmds in mach_header is larger than file";
  }
  uint32_t flags = fHeader_->flags();
  const uint32_t invalidBits = MH_INCRLINK | MH_LAZY_INIT | 0xF0000000;
  if (flags & invalidBits) {
    throw "invalid bits in mach_header flags";
  }
  if ((flags & MH_NO_REEXPORTED_DYLIBS) && (fHeader_->filetype() != MH_DYLIB)) {
    throw "MH_NO_REEXPORTED_DYLIBS bit of mach_header flags only valid for "
          "dylibs";
  }
  switch (fHeader_->filetype()) {
    case MH_EXECUTE:
      fSlidableImage_ = (flags & MH_PIE);
      break;
    case MH_DYLIB:
    case MH_BUNDLE:
      fSlidableImage_ = true;
      break;
    default:
      throw "not a mach-o file type supported by this tool";
  }
}

template <typename A>
bool Parser<A>::isStaticExecutable() const {
  bool isStaticExecutable = false;
  const uint32_t cmd_count = fHeader_->ncmds();
  const macho_load_command<P> *const cmds =
      (macho_load_command<P> *)((uint8_t *)fHeader_ + sizeof(macho_header<P>));
  const macho_load_command<P> *cmd = cmds;
  if (fHeader_->filetype() == MH_EXECUTE) {
    isStaticExecutable = true;
    cmd = cmds;
    for (uint32_t i = 0; i < cmd_count; ++i) {
      switch (cmd->cmd()) {
        case LC_LOAD_DYLINKER:
          isStaticExecutable = false;
          break;
      }
      cmd = (const macho_load_command<P> *)(((uint8_t *)cmd) + cmd->cmdsize());
    }
    if (isStaticExecutable) {
      if ((fHeader_->flags() != MH_NOUNDEFS) &&
          (fHeader_->flags() != (MH_NOUNDEFS | MH_PIE))) {
        throw "invalid bits in mach_header flags for static executable";
      }
    }
  }
  return isStaticExecutable;
}

template <typename A>
void Parser<A>::parseSections() {
  const uint8_t *const endOfFile = (uint8_t *)fHeader_ + fileLength_;
  const uint8_t *const endOfLoadCommands =
      (uint8_t *)fHeader_ + sizeof(macho_header<P>) + fHeader_->sizeofcmds();
  const uint32_t cmd_count = fHeader_->ncmds();
  const macho_load_command<P> *const cmds =
      (macho_load_command<P> *)((uint8_t *)fHeader_ + sizeof(macho_header<P>));
  const macho_load_command<P> *cmd = cmds;
  const IncrementalCommand<P> *incrementalCommand;
  bool isStaticExecutable = this->isStaticExecutable();
  for (uint32_t i = 0; i < cmd_count; ++i) {
    uint32_t size = cmd->cmdsize();
    if ((size & this->loadCommandSizeMask()) != 0) {
      throwf("load command #%d has a unaligned size", i);
    }
    const uint8_t *endOfCmd = ((uint8_t *)cmd) + cmd->cmdsize();
    if (endOfCmd > endOfLoadCommands) {
      throwf("load command #%d extends beyond the end of the load commands", i);
    }
    if (endOfCmd > endOfFile) {
      throwf("load command #%d extends beyond the end of the file", i);
    }
    switch (cmd->cmd()) {
      case macho_segment_command<P>::CMD: {
        const macho_segment_command<P> *segCmd =
            (const macho_segment_command<P> *)cmd;
        fSegments_.push_back(segCmd);
        SegmentBoundary boundary;
        boundary.start_ = segCmd->vmaddr();
        boundary.size_ = segCmd->vmsize();
        segmentBoundaries_.push_back(boundary);
        if (strcmp(segCmd->segname(), "__TEXT") == 0) {
          baseAddress_ = segCmd->vmaddr();
          parseTextSegment(segCmd);
        } else if (strcmp(segCmd->segname(), "__DATA_CONST") == 0) {
          parseDataConstSegment(segCmd);
        } else if (strcmp(segCmd->segname(), "__DATA") == 0) {
          parseDataSegment(segCmd);
        } else if (strcmp(segCmd->segname(), "__LINKEDIT") == 0) {
          linkEditSegment_ = segCmd;
        }
      } break;
      case LC_DYLD_INFO:
      case LC_DYLD_INFO_ONLY: {
        dyldInfo_ = (const macho_dyld_info_command<P> *)cmd;
      } break;
      case LC_MAIN: {
        if (fHeader_->filetype() != MH_EXECUTE) {
          throw "LC_MAIN can only be used in MH_EXECUTE file types";
        }
        entryPoint_ = (macho_entry_point_command<P> *)cmd;
      } break;
      case LC_FUNCTION_STARTS: {
        //                const macho_linkedit_data_command<P> *info =
        //                (macho_linkedit_data_command<P> *)cmd;
      } break;
      case LC_SYMTAB: {
        this->parseSymbolTable(cmd);
      } break;
      case LC_DYSYMTAB: {
        if (isStaticExecutable && !fSlidableImage_) {
          throw "LC_DYSYMTAB should not be used in static executable";
        }
        fDynamicSymbolTable_ = (const macho_dysymtab_command<P> *)cmd;
      } break;
      case LC_LOAD_DYLIB:
      case LC_LOAD_WEAK_DYLIB:
      case LC_REEXPORT_DYLIB:
      case LC_LOAD_UPWARD_DYLIB:
      case LC_LAZY_LOAD_DYLIB: {
        const macho_dylib_command<P> *dylib =
            (const macho_dylib_command<P> *)cmd;
        dylibLoadCommands_.push_back(dylib);
      } break;
      case LC_INCREMENTAL: {
        incrementalCommand = (IncrementalCommand<P> *)cmd;
        this->parseIncrementalStringPool(incrementalCommand);
        this->parseIncrementalInputsSection(incrementalCommand);
        this->parseIncrementalFixupSection(incrementalCommand);
        this->parseIncrementalGlobalSymbols(incrementalCommand);
        this->parseIncrementalPatchSpaceSection(incrementalCommand);
      } break;
      default:
        break;
    }
    cmd = (const macho_load_command<P> *)endOfCmd;
  }
}

template <typename A>
void Parser<A>::parseTextSegment(const macho_segment_command<P> *segCmd) {
  const macho_section<P> *const sectionsStart =
      (macho_section<P> *)((char *)segCmd + sizeof(macho_segment_command<P>));
  const macho_section<P> *const sectionsEnd = &sectionsStart[segCmd->nsects()];
  for (const macho_section<P> *sect = sectionsStart; sect < sectionsEnd;
       ++sect) {
    if (strncmp(sect->sectname(), "__objc_classname", 16) == 0) {
      parseObjCClassName(sect);
    }
  }
}

template <typename A>
void Parser<A>::parseObjCClassName(const macho_section<P> *sect) {}

template <typename A>
void Parser<A>::parseDataConstSegment(const macho_segment_command<P> *segCmd) {
  const macho_section<P> *const sectionsStart =
      (macho_section<P> *)((char *)segCmd + sizeof(macho_segment_command<P>));
  const macho_section<P> *const sectionsEnd = &sectionsStart[segCmd->nsects()];
  for (const macho_section<P> *sect = sectionsStart; sect < sectionsEnd;
       ++sect) {
    if (strcmp(sect->sectname(), "__got") == 0) {
      got_section_ = const_cast<macho_section<P> *>(sect);
    } else if (strncmp(sect->sectname(), "__objc_classlist", 16) == 0) {
      parseObjcClassList(sect);
    }
  }
}

template <typename A>
void Parser<A>::parseObjcClassList(const macho_section<P> *sect) {
  const uint64_t *p = (const uint64_t *)((uint8_t *)fHeader_ + sect->offset());
  uint32_t entryCount = sect->size() / sizeof(pint_t);
  for (uint32_t i = 0; i < entryCount; i++) {
    uint64_t objcClassAddress = E::get64(*p++);
    objcClassAddresses_.push_back(objcClassAddress);
    objcClassSectionOffsetMap_[objcClassAddress] = i * sizeof(pint_t);
  }
}

template <typename A>
void Parser<A>::parseDataSegment(const macho_segment_command<P> *segCmd) {
  const macho_section<P> *const sectionsStart =
      (macho_section<P> *)((char *)segCmd + sizeof(macho_segment_command<P>));
  const macho_section<P> *const sectionsEnd = &sectionsStart[segCmd->nsects()];
  for (const macho_section<P> *sect = sectionsStart; sect < sectionsEnd;
       ++sect) {
    if (strcmp(sect->sectname(), "__la_symbol_ptr") == 0) {
      la_symbol_ptr_section_ = const_cast<macho_section<P> *>(sect);
    } else if (strncmp(sect->sectname(), "__objc_classrefs", 16) == 0) {
      objcClassRefsSection_ = sect;
    } else if (strcmp(sect->sectname(), "__objc_data") == 0) {
      parseObjCData(sect);
    }
  }
}

template <typename A>
void Parser<A>::parseObjCClassRefs(const macho_section<P> *sect) {
  const uint64_t *p = (const uint64_t *)((uint8_t *)fHeader_ + sect->offset());
  uint32_t entryCount = sect->size() / sizeof(pint_t);
  for (uint32_t i = 0; i < entryCount; i++) {
    uint64_t address = E::get64(*p++);
    if (symbolAddressToIndex_.find(address) != symbolAddressToIndex_.end()) {
      uint32_t symIndex = symbolAddressToIndex_[address];
      const macho_nlist<P> &sym = this->symbolFromIndex(symIndex);
      const char *symName = &stringTable_[sym.n_strx()];
      auto refsAtom = new RefsProxyAtom(symName, sizeof(pint_t));
      refsAtom->setSectionOffset(i * sizeof(pint_t));
      objcClassRefsAtoms_.push_back(refsAtom);
    }
  }
}

template <typename A>
void Parser<A>::parseObjCData(const macho_section<P> *sect) {
  const uint8_t *sectionStart = (uint8_t *)fHeader_ + sect->offset();
  const uint8_t *sectionEnd =
      (uint8_t *)fHeader_ + sect->offset() + sect->size();
  for (auto ait = objcClassAddresses_.begin(); ait != objcClassAddresses_.end();
       ++ait) {
    // section __objc_classlist
    uint64_t offset = *ait - baseAddress();
    const uint8_t *objcClassPtr = (uint8_t *)fHeader_ + offset;
    assert(objcClassPtr >= sectionStart && objcClassPtr < sectionEnd);
    const uint64_t *objcClassContent =
        (const uint64_t *)(objcClassPtr +
                           offsetof(typename ObjCClass<A>::Content, data));
    // section __objc_const
    const uint64_t objcClassDataOffset =
        E::get64(*objcClassContent) - baseAddress();
    const uint8_t *objcClassDataPtr = (uint8_t *)fHeader_ + objcClassDataOffset;
    const uint64_t *objcClassNamePtr =
        (const uint64_t *)(objcClassDataPtr +
                           offsetof(typename ObjCClass<A>::ROContent, name));
    // section __objc_classname
    const uint64_t objcClassNameOffset =
        E::get64(*objcClassNamePtr) - baseAddress();
    const char *objcClassName =
        (const char *)((uint8_t *)fHeader_ + objcClassNameOffset);
    objcClassIndexMap_[objcClassName] = objcClassSectionOffsetMap_[*ait];
  }
}

template <typename A>
void Parser<A>::parseDyldInfoSegment(const macho_dyld_info_command<P> *segCmd) {
  this->parseRebaseSection(segCmd);
  this->parseBindingSection(segCmd, bindingInfo_, false);
  this->parseBindingSection(segCmd, weakBindingInfo_, true);
  this->ParseLazyBindingSection(segCmd);
}

template <typename A>
void Parser<A>::parseRebaseSection(const macho_dyld_info_command<P> *segCmd) {
  if (!segCmd || segCmd->rebase_off() == 0) {
    return;
  }
  const uint32_t rebasePatchOffset =
      incrPatchSpaceMap_["__rebase"].patchOffset_;
  const uint8_t *p = (uint8_t *)fHeader_ + segCmd->rebase_off();
  const uint8_t *sectionEnd = &p[rebasePatchOffset];
  uint8_t type = 0;
  uint64_t segOffset = 0;
  uint32_t count;
  uint32_t skip;
  int segIndex;
  pint_t segStartAddr = 0;
  pint_t addr;
  bool done = false;
  while (!done && (p < sectionEnd)) {
    uint8_t immediate = *p & REBASE_IMMEDIATE_MASK;
    uint8_t opcode = *p & REBASE_OPCODE_MASK;
    ++p;
    switch (opcode) {
      case REBASE_OPCODE_DONE:
        done = true;
        break;
      case REBASE_OPCODE_SET_TYPE_IMM:
        type = immediate;
        break;
      case REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
        segIndex = immediate;
        segStartAddr = segStartAddress(segIndex);
        segOffset = read_uleb128(p, sectionEnd);
        break;
      case REBASE_OPCODE_ADD_ADDR_ULEB:
        segOffset += read_uleb128(p, sectionEnd);
        break;
      case REBASE_OPCODE_ADD_ADDR_IMM_SCALED:
        segOffset += immediate * sizeof(pint_t);
        break;
      case REBASE_OPCODE_DO_REBASE_IMM_TIMES:
        for (int i = 0; i < immediate; ++i) {
          addr = segStartAddr + segOffset;
          //                    if ((rangeStart <= addr) && (addr < rangeEnd))
          //                        return;
          rebaseAddresses_.insert(addr);
          rebaseInfo_.push_back({type, addr});
          segOffset += sizeof(pint_t);
        }
        break;
      case REBASE_OPCODE_DO_REBASE_ULEB_TIMES:
        count = read_uleb128(p, sectionEnd);
        for (uint32_t i = 0; i < count; ++i) {
          addr = segStartAddr + segOffset;
          //                    if ( (rangeStart <= addr) && (addr < rangeEnd) )
          //                        return;
          rebaseAddresses_.insert(addr);
          rebaseInfo_.push_back({type, addr});
          segOffset += sizeof(pint_t);
        }
        break;
      case REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB:
        addr = segStartAddr + segOffset;
        //                if ((rangeStart <= addr) && (addr < rangeEnd))
        //                    return;
        rebaseAddresses_.insert(addr);
        rebaseInfo_.push_back({type, addr});
        segOffset += read_uleb128(p, sectionEnd) + sizeof(pint_t);
        break;
      case REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB:
        count = read_uleb128(p, sectionEnd);
        skip = read_uleb128(p, sectionEnd);
        for (uint32_t i = 0; i < count; ++i) {
          addr = segStartAddr + segOffset;
          //                    if ( (rangeStart <= addr) && (addr < rangeEnd) )
          //                        return;
          rebaseAddresses_.insert(addr);
          rebaseInfo_.push_back({type, addr});
          segOffset += skip + sizeof(pint_t);
        }
        break;
      default:
        throwf("bad rebase opcode %d", *p);
    }
  }
}

template <typename A>
void Parser<A>::parseBindingSection(const macho_dyld_info_command<P> *segCmd,
                                    std::vector<BindingInfoTuple> &bindingInfo,
                                    bool weakBinding) {
  if (!segCmd) {
    printf("no compressed binding info\n");
    return;
  } else if (!weakBinding && (segCmd->bind_off() == 0)) {
    printf("no compressed binding info\n");
    return;
  } else if (weakBinding && (segCmd->weak_bind_off() == 0)) {
    printf("no compressed weak binding info\n");
    return;
  }

  const uint8_t *start;
  const uint8_t *end;
  if (weakBinding) {
    printf("weak binding opcodes:\n");
    start = (uint8_t *)fHeader_ + segCmd->weak_bind_off();
    end = &start[segCmd->weak_bind_size()];
  } else {
    printf("binding opcodes:\n");
    start = (uint8_t *)fHeader_ + segCmd->bind_off();
    end = &start[segCmd->bind_size()];
    this->recordSectionBoudary("__binding", segCmd->bind_off(),
                               segCmd->bind_size());
  }
  const uint8_t *p = start;
  uint8_t type = 0;
  uint8_t flags;
  uint64_t address = baseAddress_;
  const char *symbolName = nullptr;
  int libraryOrdinal = 0;
  int64_t addend = 0;
  uint32_t segmentIndex = 0;
  uint32_t count;
  uint32_t skip;
  bool weakImport = false;
  bool done = false;
  while (!done && (p < end)) {
    uint8_t immediate = *p & BIND_IMMEDIATE_MASK;
    uint8_t opcode = *p & BIND_OPCODE_MASK;
    ++p;
    switch (opcode) {
      case BIND_OPCODE_DONE:
        done = true;
        break;
      case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
        libraryOrdinal = immediate;
        break;
      case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
        libraryOrdinal = read_uleb128(p, end);
        break;
      case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
        // the special ordinals are negative numbers
        if (immediate == 0) {
          libraryOrdinal = 0;
        } else {
          int8_t signExtended = BIND_OPCODE_MASK | immediate;
          libraryOrdinal = signExtended;
        }
        break;
      case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
        flags = immediate & BIND_SYMBOL_FLAGS_WEAK_IMPORT;
        symbolName = (char *)p;
        while (*p != '\0') ++p;
        ++p;
        weakImport = (flags != 0);
        break;
      case BIND_OPCODE_SET_TYPE_IMM:
        type = immediate;
        break;
      case BIND_OPCODE_SET_ADDEND_SLEB:
        addend = read_sleb128(p, end);
        break;
      case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
        segmentIndex = immediate;
        address = this->segStartAddress(segmentIndex) + read_uleb128(p, end);
        break;
      case BIND_OPCODE_ADD_ADDR_ULEB: {
        uint64_t val = read_uleb128(p, end);
        address += val;
      } break;
      case BIND_OPCODE_DO_BIND:
        bindingInfo.push_back(std::make_tuple(type, libraryOrdinal, symbolName,
                                              weakImport, address, addend));
        address += sizeof(pint_t);
        break;
      case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
        skip = read_uleb128(p, end);
        bindingInfo.push_back(std::make_tuple(type, libraryOrdinal, symbolName,
                                              weakImport, address, addend));
        address += sizeof(pint_t) + skip;
        break;
      case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
        skip = immediate * sizeof(pint_t) + sizeof(pint_t);
        bindingInfo.push_back(std::make_tuple(type, libraryOrdinal, symbolName,
                                              weakImport, address, addend));
        address += skip;
        break;
      case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:
        count = read_uleb128(p, end);
        skip = read_uleb128(p, end);
        for (uint32_t i = 0; i < count; ++i) {
          bindingInfo.push_back(std::make_tuple(
              type, libraryOrdinal, symbolName, weakImport, address, addend));
          address += sizeof(pint_t) + skip;
        }
        break;
      case BIND_OPCODE_THREADED:
        // Note the immediate is a sub opcode
        switch (immediate) {
          case BIND_SUBOPCODE_THREADED_SET_BIND_ORDINAL_TABLE_SIZE_ULEB:
            count = read_uleb128(p, end);
            break;
          case BIND_SUBOPCODE_THREADED_APPLY:
            break;
          default:
            throwf("unknown threaded bind subopcode %d", immediate);
        }
        break;
      default:
        throwf("unknown bind opcode %d", *p);
    }
  }
}

template <typename A>
void Parser<A>::ParseLazyBindingSection(
    const macho_dyld_info_command<P> *segCmd) {
  if (!segCmd || segCmd->lazy_bind_off() == 0) {
    printf("no compressed lazy binding info\n");
    return;
  }
  this->recordSectionBoudary("__lazy_binding", segCmd->lazy_bind_off(),
                             segCmd->lazy_bind_size());
  const uint8_t *const start = (uint8_t *)fHeader_ + segCmd->lazy_bind_off();
  const uint8_t *const end = &start[segCmd->lazy_bind_size()];

  uint8_t type = BIND_TYPE_POINTER;
  uint8_t segIndex = 0;
  uint64_t segOffset = 0;
  const char *symbolName = nullptr;
  int libraryOrdinal = 0;
  int64_t addend = 0;
  pint_t segStartAddr = 0;
  bool weakImport = false;
  for (const uint8_t *p = start; p < end;) {
    uint8_t immediate = *p & BIND_IMMEDIATE_MASK;
    uint8_t opcode = *p & BIND_OPCODE_MASK;
    ++p;
    switch (opcode) {
      case BIND_OPCODE_DONE:
        break;
      case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
        libraryOrdinal = immediate;
        break;
      case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
        libraryOrdinal = read_uleb128(p, end);
        break;
      case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
        // the special ordinals are negative numbers
        if (immediate == 0)
          libraryOrdinal = 0;
        else {
          int8_t signExtended = BIND_OPCODE_MASK | immediate;
          libraryOrdinal = signExtended;
        }
        break;
      case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
        symbolName = (char *)p;
        while (*p != '\0') ++p;
        ++p;
        weakImport = (immediate & BIND_SYMBOL_FLAGS_WEAK_IMPORT) != 0;
        break;
      case BIND_OPCODE_SET_TYPE_IMM:
        type = immediate;
        break;
      case BIND_OPCODE_SET_ADDEND_SLEB:
        addend = read_sleb128(p, end);
        break;
      case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
        segIndex = immediate;
        segStartAddr = segStartAddress(segIndex);
        segOffset = read_uleb128(p, end);
        break;
      case BIND_OPCODE_ADD_ADDR_ULEB:
        segOffset += read_uleb128(p, end);
        break;
      case BIND_OPCODE_DO_BIND:
        lazyBindingInfo_.push_back(
            std::make_tuple(type, libraryOrdinal, symbolName, weakImport,
                            segStartAddr + segOffset, addend));
        segOffset += sizeof(pint_t);
        break;
      default:
        throwf("bad lazy bind opcode %d", *p);
    }
  }
}

template <typename A>
void Parser<A>::parseIncrementalInputsSection(
    const IncrementalCommand<P> *incrementalCommand) {
  if (!incrementalCommand) {
    return;
  }
  fIncrementalInputSection_ =
      (const InputEntrySection<P> *)((uint8_t *)fHeader_ +
                                     incrementalCommand->inputs_off());
  InputEntrySection<P> *incrInputPtr =
      const_cast<InputEntrySection<P> *>(fIncrementalInputSection_);
  for (uint32_t index = 0; index < incrementalCommand->file_count(); index++) {
    size_t size = sizeof(InputEntrySection<P>);
    switch (incrInputPtr->type()) {
      case ld::File::Type::Reloc: {
        uint32_t atomCount = incrInputPtr->atomCount();
        size = 5 * sizeof(uint32_t) +
               sizeof(ld::incremental::AtomEntry<P>) * atomCount;
      } break;
      case ld::File::Type::Archive: {
      } break;
      case ld::File::Type::Dylib: {
      } break;
      default:
        break;
    }
    incrInputs_.push_back(incrInputPtr);
    incrInputsMap_[incrStringPool_[incrInputPtr->fileIndexInStringTable()]] =
        incrInputPtr;
    incrInputPtr = (InputEntrySection<P> *)(((uint8_t *)incrInputPtr) + size);
  }
}

template <typename A>
void Parser<A>::parseIncrementalFixupSection(
    const IncrementalCommand<P> *incrementalCommand) {
  incrementalFixupSection_ =
      (const InputFileFixupSection<P> *)((uint8_t *)fHeader_ +
                                         incrementalCommand->fixups_off());
  incrementalFixupSection_->forEachFixup([&](const IncrFixupEntry<P> &fixup) {
    std::string key = incrStringPool_[fixup.nameIndex()];
    if (incrFixupsMap_.find(key) == incrFixupsMap_.end()) {
      std::vector<IncrFixup> fixups;
      fixups.push_back({fixup.address(), fixup.nameIndex()});
      incrFixupsMap_[key] = fixups;
    } else {
      auto &fixups = incrFixupsMap_[key];
      fixups.push_back({fixup.address(), fixup.nameIndex()});
    }
  });
}

template <typename A>
void Parser<A>::parseIncrementalPatchSpaceSection(
    const IncrementalCommand<P> *incrementalCommand) {
  fIncrementalPatchSpaceSection_ =
      (const PatchSpaceSectionEntry<P>
           *)((uint8_t *)fHeader_ + incrementalCommand->patch_space_off());
  const PatchSpaceSectionEntry<P> *patchSpaceEnd =
      (const PatchSpaceSectionEntry<P>
           *)((uint8_t *)fHeader_ + incrementalCommand->patch_space_off() +
              incrementalCommand->patch_space_size());
  PatchSpaceSectionEntry<P> *p =
      const_cast<PatchSpaceSectionEntry<P> *>(fIncrementalPatchSpaceSection_);
  uint32_t patchCount = patchSpaceEnd - fIncrementalPatchSpaceSection_;
  for (uint32_t patchIndex = 0; patchIndex < patchCount; ++patchIndex, ++p) {
    if (strcmp(p->sectname(), "__string_pool") == 0) {
      continue;
    }
    PatchSpace patchSpace;
    strncpy(patchSpace.sectname, p->sectname(), 17);
    patchSpace.patchOffset_ = p->patchOffset();
    patchSpace.patchSpace_ = p->patchSpace();
    incrPatchSpaceMap_[patchSpace.sectname] = patchSpace;
  }
}

template <typename A>
void Parser<A>::parseIncrementalGlobalSymbols(
    const IncrementalCommand<P> *incrementalCommand) {
  if (!incrementalCommand) {
    return;
  }
  // incremental global symbol table
  fIncrementalSymbolSection_ =
      (const GlobalSymbolTableEntry<P> *)((uint8_t *)fHeader_ +
                                          incrementalCommand->symtab_off());
  GlobalSymbolTableEntry<P> *symbolStart =
      const_cast<GlobalSymbolTableEntry<P> *>(fIncrementalSymbolSection_);
  const GlobalSymbolTableEntry<P> *symbolEnd =
      (const GlobalSymbolTableEntry<P> *)((uint8_t *)fHeader_ +
                                          incrementalCommand->symtab_off() +
                                          incrementalCommand->symtab_size());
  while (symbolStart < symbolEnd) {
    if ((symbolEnd - symbolStart) < 8) {
      break;
    }
    incrSymbols_.push_back(symbolStart);
    symbolStart =
        (GlobalSymbolTableEntry<P> *)(((uint8_t *)symbolStart) +
                                      (2 +
                                       symbolStart->referencedFileCount_()) *
                                          sizeof(uint32_t));
  }
}

template <typename A>
void Parser<A>::parseIncrementalStringPool(
    const IncrementalCommand<P> *incrementalCommand) {
  if (!incrementalCommand) {
    return;
  }
  // incremental string pool
  fIncrementalStrings_ =
      (const char *)((uint8_t *)fHeader_ + incrementalCommand->strtab_off());
  char *p = const_cast<char *>(fIncrementalStrings_);
  const char *stringEnd =
      fIncrementalStrings_ + incrementalCommand->strtab_size();
  uint32_t stringIdx = 0;
  while (p < stringEnd) {
    const char *symName = &fIncrementalStrings_[stringIdx];
    size_t length = strlen(symName);
    if (!symName || length == 0) {
      break;
    }
    incrStringPool_.push_back(symName);
    p += length + 1;
    stringIdx += length + 1;
  }
}

template <typename A>
void Parser<A>::parseSymbolTable(const macho_load_command<P> *cmd) {
  const macho_symtab_command<P> *symtab = (macho_symtab_command<P> *)cmd;
  // String pool
  stringTable_ = (char *)fHeader_ + symtab->stroff();
  stringTableEnd_ = stringTable_ + symtab->strsize();
  if (symtab->stroff() < linkEditSegment_->fileoff()) {
    throw "string pool not in __LINKEDIT";
  }
  if ((symtab->stroff() + symtab->strsize()) >
      (linkEditSegment_->fileoff() + linkEditSegment_->filesize())) {
    throw "string pool extends beyond __LINKEDIT";
  }
  if ((symtab->stroff() % 4) != 0) {
    throw "string pool start not pointer aligned";
  }

  char *p = const_cast<char *>(stringTable_);
  uint32_t stringIdx = 0;
  while (p < stringTableEnd_) {
    const char *symName = &stringTable_[stringIdx];
    size_t length = strlen(symName);
    if (!symName || length == 0) {
      break;
    }
    stringPool_[symName] = stringIdx;
    p += length + 1;
    stringIdx += length + 1;
  }
  ptrdiff_t patchOffset = uintptr_t(p) - uintptr_t(stringTable_);
  currentBufferUsed_ = patchOffset;
  PatchSpace patchSpace;
  strcpy(patchSpace.sectname, "__string_pool");
  patchSpace.patchOffset_ = patchOffset;
  patchSpace.patchSpace_ = symtab->strsize() - patchOffset;
  incrPatchSpaceMap_["__string_pool"] = patchSpace;

  this->recordSectionBoudary("__string_pool", symtab->stroff(),
                             symtab->strsize());

  // Symbol table
  symbolCount_ = symtab->nsyms();
  if (symbolCount_ != 0) {
    symbolTable_ =
        (const macho_nlist<P> *)((uint8_t *)fHeader_ + symtab->symoff());
    if (symtab->symoff() < linkEditSegment_->fileoff()) {
      throw "symbol table not in __LINKEDIT";
    }
    if ((symtab->symoff() + symbolCount_ * sizeof(macho_nlist<P> *)) >
        symtab->stroff()) {
      throw "symbol table overlaps string pool";
    }
    if ((symtab->symoff() % sizeof(pint_t)) != 0) {
      throw "symbol table start not pointer aligned";
    }
  }
  for (uint32_t i = 0; i < symbolCount_; ++i) {
    const macho_nlist<P> *sym = &symbolTable_[i];
    if (sym->n_strx() == 0) {
      symbolCount_ = i;
      break;
    }
    const char *symName = this->nameFromSymbol(*sym);
    if ((sym->n_type() & N_TYPE) == N_UNDF && (sym->n_type() & N_EXT) != 0) {
      // dylib symbol
      dylibSymbolMap_[symName] = sym;
    }

    if (symToSectionOffset_.find(sym->n_type()) == symToSectionOffset_.end()) {
      std::unordered_map<std::string, uint64_t> offsetMap;
      offsetMap[symName] = i * sizeof(macho_nlist<P>);
      symToSectionOffset_[sym->n_type()] = offsetMap;
    } else {
      auto &offsetMap = symToSectionOffset_[sym->n_type()];
      offsetMap[symName] = i * sizeof(macho_nlist<P>);
    }
    if (symbolTypeToOffset_.find(sym->n_type()) == symbolTypeToOffset_.end()) {
      symbolTypeToOffset_[sym->n_type()] = i * sizeof(macho_nlist<P>);
    }
    if (sym->n_value() != 0) {
      symbolAddressToIndex_[sym->n_value()] = i;
    }
  }
  this->recordSectionBoudary("__symbol_table", symtab->symoff(),
                             symtab->nsyms() * sizeof(macho_nlist<P>));
}

template <typename A>
void Parser<A>::parseIndirectSymbolTable() {
  const macho_load_command<P> *const cmds =
      (macho_load_command<P> *)((uint8_t *)fHeader_ + sizeof(macho_header<P>));
  const uint32_t cmd_count = fHeader_->ncmds();
  indirectSymbolTable_ = (uint32_t *)((uint8_t *)fHeader_ +
                                      fDynamicSymbolTable_->indirectsymoff());
  indirectTableCount_ = fDynamicSymbolTable_->nindirectsyms();
  if (indirectTableCount_ != 0) {
    if (fDynamicSymbolTable_->indirectsymoff() < linkEditSegment_->fileoff()) {
      throw "indirect symbol table not in __LINKEDIT";
    }
    if ((fDynamicSymbolTable_->indirectsymoff() + indirectTableCount_ * 4) >
        (linkEditSegment_->fileoff() + linkEditSegment_->filesize())) {
      throw "indirect symbol table not in __LINKEDIT";
    }
    if ((fDynamicSymbolTable_->indirectsymoff() % sizeof(pint_t)) != 0) {
      throw "indirect symbol table not pointer aligned";
    }
    this->recordSectionBoudary(
        "__ind_sym_tab", fDynamicSymbolTable_->indirectsymoff(),
        fDynamicSymbolTable_->nindirectsyms() * sizeof(macho_nlist<P>));

    const macho_load_command<P> *cmd = cmds;
    for (uint32_t i = 0; i < cmd_count; ++i) {
      switch (cmd->cmd()) {
        case macho_segment_command<P>::CMD: {
          const macho_segment_command<P> *segCmd =
              (const macho_segment_command<P> *)cmd;
          const macho_section<P> *const sectionsStart =
              (macho_section<P> *)((char *)segCmd +
                                   sizeof(macho_segment_command<P>));
          const macho_section<P> *const sectionsEnd =
              &sectionsStart[segCmd->nsects()];
          for (const macho_section<P> *sect = sectionsStart; sect < sectionsEnd;
               ++sect) {
            // make sure all magic sections that use indirect symbol table fit
            // within it
            uint32_t start = 0;
            uint32_t elementSize = 0;
            switch (sect->flags() & SECTION_TYPE) {
              case S_SYMBOL_STUBS:
                elementSize = sect->reserved2();
                start = sect->reserved1();
                break;
                //  case S_LAZY_SYMBOL_POINTERS:
                //  case S_THREAD_LOCAL_VARIABLE_POINTERS:
                //              case S_NON_LAZY_SYMBOL_POINTERS:
                //                elementSize = sizeof(pint_t);
                //                start = sect->reserved1();
              default:
                break;
            }
            if (elementSize != 0) {
              auto patch = incrPatchSpaceMap_[sect->sectname()];
              uint32_t count = (sect->size() - patch.patchSpace_) / elementSize;
              for (uint32_t index = 0; index < count; ++index) {
                uint32_t symIndex = this->indirectSymbol(start + index);
                if (symIndex != INDIRECT_SYMBOL_LOCAL) {
                  const macho_nlist<P> &sym = this->symbolFromIndex(symIndex);
                  pint_t address = sect->addr() + index * sizeof(pint_t);
                  uint32_t ordinal = GET_LIBRARY_ORDINAL(sym.n_desc());
                  const macho_dylib_command<P> *dylibCommand =
                      dylibLoadCommands_[ordinal - 1];
                  const char *symbolName = this->nameFromSymbol(sym);
                  if (strcmp(symbolName, "dyld_stub_binder") == 0) {
                    continue;
                  }
                  auto file = new generic::dylib::File(
                      dylibCommand->name(), 0,
                      ld::File::Ordinal::makeArgOrdinal(ordinal),
                      options_.platforms(), false, false, false, false, true);
                  auto atom = new generic::dylib::ExportAtom(
                      *file, symbolName, "", 1, this->weakImportFromSymbol(sym),
                      false, (uint64_t)address);
                  atom->setFile(file);
                  stubAtoms_.push_back(atom);
                  stubNames_.insert(symbolName);
                  dylibToOrdinal_[file] = ordinal;
                  dylibNameToOrdinal_[dylibShortName(file->leafName())] =
                      ordinal;
                  dylibSymbolMap_.erase(symbolName);
                }
              }
            }

            SectionBoundary sectionBoundary;
            sectionBoundary.address_ = sect->addr();
            sectionBoundary.fileOffset_ = sect->offset();
            sectionBoundary.size_ = sect->size();
            const char *name = sect->sectname();
            if (strlen(name) >= 16) {
              char sectionName[17];
              strlcpy(sectionName, name, 17);
              sectionBoundaryMap_[sectionName] = sectionBoundary;
            } else {
              sectionBoundaryMap_[name] = sectionBoundary;
            }
          }
        } break;
        case LC_DYLD_INFO:
        case LC_DYLD_INFO_ONLY: {
          const macho_dyld_info_command<P> *segCmd =
              (const macho_dyld_info_command<P> *)cmd;
          this->recordSectionBoudary("__rebase", segCmd->rebase_off(),
                                     segCmd->rebase_size());
        } break;
        default:
          break;
      }
      cmd = (const macho_load_command<P> *)(((uint8_t *)cmd) + cmd->cmdsize());
    }
  }

  for (auto const &x : dylibSymbolMap_) {
    const macho_nlist<P> *sym = x.second;
    uint32_t ordinal = GET_LIBRARY_ORDINAL(sym->n_desc());
    const macho_dylib_command<P> *dylibCommand =
        dylibLoadCommands_[ordinal - 1];
    auto file = new generic::dylib::File(
        dylibCommand->name(), 0, ld::File::Ordinal::makeArgOrdinal(ordinal),
        options_.platforms(), false, false, false, false, true);
    dylibToOrdinal_[file] = ordinal;
    dylibNameToOrdinal_[dylibShortName(file->leafName())] = ordinal;
  }
}

template <typename A>
uint32_t Parser<A>::indirectSymbol(uint32_t indirectIndex) const {
  if (indirectIndex >= indirectTableCount_) {
    throw "indirect symbol index out of range";
  }
  return E::get32(indirectSymbolTable_[indirectIndex]);
}

template <typename A>
const macho_nlist<typename A::P> &Parser<A>::symbolFromIndex(uint32_t index) {
  if (index > symbolCount_) {
    throw "symbol index out of range";
  }
  return symbolTable_[index];
}

template <typename A>
const char *Parser<A>::nameFromSymbol(const macho_nlist<P> &sym) {
  uint32_t strOffset = sym.n_strx();
  if (strOffset >= (stringTableEnd_ - stringTable_)) {
    throw "malformed nlist string offset";
  }
  return &stringTable_[strOffset];
}

template <typename A>
bool Parser<A>::weakImportFromSymbol(const macho_nlist<P> &sym) {
  return (((sym.n_type() & N_TYPE) == N_UNDF) &&
          ((sym.n_desc() & N_WEAK_REF) != 0));
}

}  // namespace incremental
}  // namespace ld

#endif  // MACHO_INCREMENTAL_FILE_HPP_