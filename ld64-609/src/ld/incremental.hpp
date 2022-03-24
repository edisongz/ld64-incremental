//
//  incremental.hpp
//  ld
//
//  Created by tttt on 2022/2/14.
//  Copyright © 2022 Apple Inc. All rights reserved.
//

#ifndef incremental_hpp
#define incremental_hpp

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <mach-o/fat.h>
#include <mach/mach_host.h>
#include <mach/mach_init.h>
#include <mach/mach_time.h>
#include <mach/vm_statistics.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>

#include <AvailabilityMacros.h>
#include <algorithm>
#include <dlfcn.h>
#include <list>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "InputFiles.h"
#include "Options.h"
#include "ld.hpp"

#include "configure.h"

#define LC_INCREMENTAL 0x41

namespace ld {
namespace incremental {

#pragma pack (1)
struct IncrAtomEntry {
    uint32_t atomNameIndex_;
    uint64_t atomFileOffset_;
    uint64_t atomSize_;
};

/// Input entry
struct InputEntry {
    uint32_t fileIndexInStringTable_; // file index in incremental string table
    uint64_t modTime_;                // last link input file modification time
    uint32_t type_;

    struct RelocObj {
        uint32_t atomCount_;
        IncrAtomEntry atoms[0];
    };

    union {
        RelocObj relocObj[0];
    } u;
};

struct IncrFixup {
    uint64_t address_;
    uint32_t nameIndex_;
};

/// Input file relocations
struct InputFileFixup {
    uint32_t fixupCount_;             // fixupCount
    IncrFixup fixups_[0];
};

/// Global symbol entry
struct GlobalSymbolRefEntry {
    uint32_t symbolIndexInStringTable_;
    uint32_t referencedFileCount_;
    uint32_t referencedFileIndex_[0];
};

/// Incremental patch space
struct PatchSpace {
    char     sectname[17];    /* name of this section */
    uint64_t patchOffset_;
    uint32_t patchSpace_;
};

struct macho_incremental_command {
    uint32_t cmd;         // LC_INCREMENT
    uint32_t cmdsize;     // sizeof(struct incremental_command)
    uint32_t file_count;  // file offset of data in __INCREMENTAL segment
    uint32_t inputs_off;  // file size of data in __INCREMENTAL segment
    uint32_t inputs_size; // file size of data in __INCREMENTAL segment
    uint32_t fixups_off;
    uint32_t fixups_size;
    uint32_t symtab_off;  // file size of data in __INCREMENTAL segment
    uint32_t symtab_size; // file size of data in __INCREMENTAL segment
    uint32_t patch_space_off;  // file size of data in __INCREMENTAL segment
    uint32_t patch_space_size;  // file size of data in __INCREMENTAL segment
    uint32_t strtab_off;  // file size of data in __INCREMENTAL segment
    uint32_t strtab_size; // file size of data in __INCREMENTAL segment
};

struct SegmentBoundary {
    uint64_t start_;
    uint32_t size_;
};
#pragma pack()

template <typename P>
class AtomEntry {
public:
    uint32_t atomNameIndex() const INLINE {
        return E::get32(entry.atomNameIndex_);
    }
    void setAtomNameIndex_(uint32_t value) INLINE {
        E::set32(entry.atomNameIndex_, value);
    }

    uint64_t atomFileOffset() const INLINE {
        return E::get64(entry.atomFileOffset_);
    }
    void setAtomFileOffset(uint64_t value) INLINE {
        E::set64(entry.atomFileOffset_, value);
    }
    
    uint64_t atomSize() const INLINE {
        return E::get64(entry.atomSize_);
    }
    void setAtomSize(uint64_t value) INLINE {
        E::set64(entry.atomSize_, value);
    }

    typedef typename P::E E;

private:
    IncrAtomEntry entry;
};

// Incremental input entry command
template <typename P>
class InputEntrySection {
public:
    uint32_t fileIndexInStringTable() const INLINE {
        return E::get32(entry.fileIndexInStringTable_);
    }
    void setFileIndexInStringTable(uint32_t value) INLINE {
        E::set32(entry.fileIndexInStringTable_, value);
    }

    uint64_t modTime() const INLINE {
        return E::get64(entry.modTime_);
    }
    void setModTime(uint64_t value) INLINE {
        E::set64(entry.modTime_, value);
    }

    ld::File::Type type() const INLINE {
        return static_cast<ld::File::Type>(E::get32(entry.type_));
    }
    void setType(uint32_t value) INLINE {
        E::set32(entry.type_, value);
    }
    
    uint32_t atomCount() const INLINE {
        return E::get32(entry.u.relocObj->atomCount_);
    }
    void setAtomCount(uint32_t value) INLINE {
        E::set32(entry.u.relocObj->atomCount_, value);
    }
    
    std::vector<AtomEntry<P>> atoms() const {
        std::vector<AtomEntry<P>> v;
        IncrAtomEntry *p = (IncrAtomEntry *)entry.u.relocObj->atoms;
        for (uint32_t i = 0; i < atomCount(); i++) {
            AtomEntry<P> atom;
            atom.setAtomNameIndex_(p->atomNameIndex_);
            atom.setAtomFileOffset(p->atomFileOffset_);
            atom.setAtomSize(p->atomSize_);
            v.push_back(atom);
            p++;
        }
        return v;
    }
    
    InputEntry &entryRef() {
        return entry;
    }
    
    typedef typename P::E E;

private:
    InputEntry entry;
};

template <typename P>
class IncrFixupEntry {
public:
    uint64_t address() const INLINE { return E::get64(entry.address_); }
    void setAddress(uint64_t value) INLINE { E::set64(entry.address_, value); }
    
    uint32_t nameIndex() const INLINE { return E::get32(entry.nameIndex_); }
    void setNameIndex(uint32_t value) INLINE { E::set32(entry.nameIndex_, value); }

    typedef typename P::E E;

private:
    IncrFixup entry;
};

template <typename P>
class InputFileFixupSection {
public:
    uint32_t fixupCount() const INLINE { return E::get32(fields.fixupCount_); }
    void setFixupCount(uint32_t value)    INLINE { E::set32(fields.fixupCount_, value); }
    
    void forEachFixup(const std::function<void (const IncrFixupEntry<P> &)> &handler) const {
        IncrFixupEntry<P> *p = (IncrFixupEntry<P> *)fields.fixups_;
        for (uint32_t i = 0; i < fixupCount(); i++) {
            IncrFixupEntry<P> fixup = *p++;
            handler(fixup);
        }
    }

    typedef typename P::E E;

private:
    struct InputFileFixup fields;
};

// Incremental input entry command
template <typename P>
class GlobalSymbolTableEntry {
public:
    GlobalSymbolTableEntry(uint32_t symbolIndex, uint32_t fileCount) {
        this->setSymbolIndexInStringTable_(symbolIndex);
        this->setReferencedFileCount_(fileCount);
    }

    uint32_t symbolIndexInStringTable_() const INLINE {
        return E::get32(entry.symbolIndexInStringTable_);
    }
    void setSymbolIndexInStringTable_(uint32_t value) INLINE {
        E::set32(entry.symbolIndexInStringTable_, value);
    }

    uint32_t referencedFileCount_() const INLINE {
        return E::get32(entry.referencedFileCount_);
    }
    void setReferencedFileCount_(uint32_t value) INLINE {
        E::set32(entry.referencedFileCount_, value);
    }

    const std::set<uint32_t> referencedFileIndex_() const INLINE {
        std::set<uint32_t> v;
        uint32_t *p = (uint32_t *)entry.referencedFileIndex_;
        for (uint32_t i = 0; i < referencedFileCount_(); i++) {
            v.insert(E::get32(*p++));
        }
        return v;
    }

    void setReferencedFileIndex_(const std::set<uint32_t> &buffer) {
        uint32_t *p = (uint32_t *)entry.referencedFileIndex_;
        for (auto it = buffer.begin(); it != buffer.end(); it++) {
            E::set32(*p++, *it);
        }
    }

    typedef typename P::E E;

private:
    struct GlobalSymbolRefEntry entry;
};

template <typename P>
class PatchSpaceSectionEntry {
public:
    const char *sectname() const INLINE { return fields.sectname; }
    void setSectname(const char* value)    INLINE { strncpy(fields.sectname, value, 17); }
    
    uint64_t patchOffset() const INLINE { return E::get64(fields.patchOffset_); }
    void setPatchOffset(uint64_t value) INLINE { E::set64(fields.patchOffset_, value); }

    uint32_t patchSpace() const INLINE { return E::get32(fields.patchSpace_); }
    void setPatchSpace(uint32_t value) INLINE { E::set32(fields.patchSpace_, value); }

    typedef typename P::E E;

private:
    struct PatchSpace fields;
};

// Incremental LoadCommand
template <typename P>
class IncrementalCommand {
public:
    uint32_t cmd() const INLINE {
        return E::get32(fields.cmd);
    }
    void set_cmd(uint32_t value) INLINE {
        E::set32(fields.cmd, value);
    }

    uint32_t cmdsize() const INLINE {
        return E::get32(fields.cmdsize);
    }
    void set_cmdsize(uint32_t value) INLINE {
        E::set32(fields.cmdsize, value);
    }

    uint32_t file_count() const INLINE {
        return E::get32(fields.file_count);
    }
    void set_file_count(uint32_t value) INLINE {
        E::set32(fields.file_count, value);
    }

    uint32_t inputs_off() const INLINE {
        return E::get32(fields.inputs_off);
    }
    void set_inputs_off(uint32_t value) INLINE {
        E::set32(fields.inputs_off, value);
    }

    uint32_t inputs_size() const INLINE {
        return E::get32(fields.inputs_size);
    }
    void set_inputs_size(uint32_t value) INLINE {
        E::set32(fields.inputs_size, value);
    }
    
    uint32_t fixups_off() const INLINE {
        return E::get32(fields.fixups_off);
    }
    void set_fixups_off(uint32_t value) INLINE {
        E::set32(fields.fixups_off, value);
    }
    
    uint32_t fixups_size() const INLINE {
        return E::get32(fields.fixups_size);
    }
    void set_fixups_size(uint32_t value) INLINE {
        E::set32(fields.fixups_size, value);
    }

    uint32_t symtab_off() const INLINE {
        return E::get32(fields.symtab_off);
    }
    void set_symtab_off(uint32_t value) INLINE {
        E::set32(fields.symtab_off, value);
    }

    uint32_t symtab_size() const INLINE {
        return E::get32(fields.symtab_size);
    }
    void set_symtab_size(uint32_t value) INLINE {
        E::set32(fields.symtab_size, value);
    }

    uint32_t strtab_off() const INLINE {
        return E::get32(fields.strtab_off);
    }
    void set_strtab_off(uint32_t value) INLINE {
        E::set32(fields.strtab_off, value);
    }
    
    uint32_t patch_space_off() const INLINE { return E::get32(fields.patch_space_off); }
    void set_patch_space_off(uint32_t value) INLINE { E::set32(fields.patch_space_off, value); }
    
    uint32_t patch_space_size() const INLINE { return E::get32(fields.patch_space_size); }
    void set_patch_space_size(uint32_t value) INLINE { E::set32(fields.patch_space_size, value); }

    uint32_t strtab_size() const INLINE {
        return E::get32(fields.strtab_size);
    }
    void set_strtab_size(uint32_t value) INLINE {
        E::set32(fields.strtab_size, value);
    }

    typedef typename P::E E;

private:
    struct macho_incremental_command fields;
};

using IncrFixupsMap = std::unordered_map<std::string, std::vector<IncrFixup>>;

class Incremental {
public:
    explicit Incremental(Options &options) : _options(options), wholeBuffer_(nullptr) {}
    uint8_t *wholeBuffer() { return wholeBuffer_; }
    void openBinary();
    void closeBinary();
    constexpr uint64_t baseAddress() const { return baseAddress_; }
    constexpr uint32_t objcClassOffset(const char *className) { return objcClassIndexMap_[className]; }
    constexpr PatchSpace &patchSpace(const char *sectName) { return patchSpace_[sectName]; }
    void forEachStubAtom(ld::File::AtomHandler& handler, ld::Internal& state);
    void forEachStubAtom(const std::function<void(const ld::Atom *)> &handler);
    void forEachSegmentBoundary(const std::function<void(SegmentBoundary &, uint32_t)> &handler);
    void forEachRebaseInfo(const std::function<void(std::pair<uint8_t, uint64_t> &)> &handler);
    constexpr std::vector<IncrFixup> &findRelocations(const char *atomName) { return incrFixupsMap_[atomName]; }
    constexpr uint64_t sectionStartAddress(const char *sectName) { return sectionStartAddressMap_[sectName]; }
    constexpr uint64_t sectionFileOffset(const char *sectName) { return sectionFileOffsetMap_[sectName]; }
    constexpr uint64_t sectionPatchFileOffset(const char *sectName) { return sectionFileOffsetMap_[sectName] + patchSpace_[sectName].patchOffset_; }
    
    constexpr std::vector<std::pair<uint8_t, uint64_t>> &rebaseInfo() {
        return rebaseInfo_;
    }
    
private:
    Options &_options;
    int fd_;
    uint64_t baseAddress_;
    uint8_t *wholeBuffer_;
    /// ObjC class index map
    std::unordered_map<const char *, uint32_t> objcClassIndexMap_;
    IncrFixupsMap incrFixupsMap_;
    std::unordered_map<std::string, PatchSpace> patchSpace_;
    std::unordered_map<std::string, uint64_t> sectionStartAddressMap_;
    std::unordered_map<std::string, uint32_t> sectionFileOffsetMap_;
    std::vector<const ld::Atom *> stubAtoms_;
    std::vector<SegmentBoundary> segmentBoundaries_;
    std::vector<std::pair<uint8_t, uint64_t>> rebaseInfo_;
};

} // namespace incremental
} // namespace ld

#endif /* incremental_hpp */
