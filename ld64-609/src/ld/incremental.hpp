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
#include "Resolver.h"
#include "SymbolTable.h"
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
//        uint32_t fixupCount_;
//        relocation_info fixups_[0];
};

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

struct GlobalSymbolRefEntry {
    uint32_t symbolIndexInStringTable_;
    uint32_t referencedFileCount_;
    uint32_t referencedFileIndex_[0];
};

struct macho_incremental_command {
    uint32_t cmd;         // LC_INCREMENT
    uint32_t cmdsize;     // sizeof(struct incremental_command)
    uint32_t file_count;  // file offset of data in __INCREMENTAL segment
    uint32_t inputs_off;  // file size of data in __INCREMENTAL segment
    uint32_t inputs_size; // file size of data in __INCREMENTAL segment
    uint32_t symtab_off;  // file size of data in __INCREMENTAL segment
    uint32_t symtab_size; // file size of data in __INCREMENTAL segment
    uint32_t strtab_off;  // file size of data in __INCREMENTAL segment
    uint32_t strtab_size; // file size of data in __INCREMENTAL segment
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
    GlobalSymbolRefEntry entry;
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

class Incremental {
public:
    explicit Incremental(const Options &options)
        : _options(options) {}
    void openIncrementalBinary();
    
private:
    const Options &_options;
};

} // namespace incremental
} // namespace ld

#endif /* incremental_hpp */
