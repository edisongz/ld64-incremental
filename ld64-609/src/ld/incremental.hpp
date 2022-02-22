//
//  incremental.hpp
//  ld
//
//  Created by tttt on 2022/2/14.
//  Copyright © 2022 Apple Inc. All rights reserved.
//

#ifndef incremental_hpp
#define incremental_hpp

#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <mach/mach_time.h>
#include <mach/vm_statistics.h>
#include <mach/mach_init.h>
#include <mach/mach_host.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/fat.h>

#include <string>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <list>
#include <algorithm>
#include <dlfcn.h>
#include <AvailabilityMacros.h>

#include "Options.h"
#include "ld.hpp"
#include "InputFiles.h"
#include "SymbolTable.h"
#include "Resolver.h"

#include "configure.h"

#define LC_INCREMENTAL 0x41

namespace ld {
namespace incremental {

#pragma pack (1)
struct InputEntry {
    uint32_t    fileIndexInStringTable_;            // file index in incremental string table
    uint64_t    modTime_;                           // last link input file modification time
    uint32_t    type_;
    
    struct RelocEntry {
        uint32_t    atomCount_;
        uint32_t    atomNameIndex_;
        uint64_t    atomFileOffset_;
        uint64_t    atomSize_;                  
    };
    
    union {
        RelocEntry relocEntry;
    } u;
};

struct GlobalSymbolRefEntry {
    uint32_t symbolIndexInStringTable_;
    uint32_t referencedFileCount_;
    uint32_t referencedFileIndex_[0];
};

struct macho_incremental_command {
    uint32_t  cmd;        // LC_INCREMENT
    uint32_t  cmdsize;    // sizeof(struct incremental_command)
    uint32_t  file_count;    // file offset of data in __LINKEDIT segment
    uint32_t  inputs_off;   // file size of data in __LINKEDIT segment
    uint32_t  inputs_size;   // file size of data in __LINKEDIT segment
    uint32_t  symtab_off;   // file size of data in __LINKEDIT segment
    uint32_t  symtab_size;   // file size of data in __LINKEDIT segment
    uint32_t  strtab_off;   // file size of data in __LINKEDIT segment
    uint32_t  strtab_size;   // file size of data in __LINKEDIT segment
};
#pragma pack()

// Incremental input entry command
template <typename P>
class InputEntryCommand {
public:
    uint32_t        fileIndexInStringTable() const             INLINE { return E::get32(entry.fileIndexInStringTable_); }
    void            setFileIndexInStringTable(uint32_t value)    INLINE { E::set32(entry.fileIndexInStringTable_, value); }
    
    uint64_t        modTime() const             INLINE { return E::get64(entry.modTime_); }
    void            setModTime(uint64_t value)    INLINE { E::set64(entry.modTime_, value); }

    ld::File::Type  type() const                INLINE { return E::get32(entry.type_); }
    void            setType(uint32_t value)     INLINE { E::set32(entry.type_, value);  }
        
    typedef typename P::E        E;
    
private:
    InputEntry    entry;
};

// Incremental input entry command
template <typename P>
class GlobalSymbolTableEntry {
public:
    GlobalSymbolTableEntry(uint32_t symbolIndex, uint32_t fileCount) {
        this->setSymbolIndexInStringTable_(symbolIndex);
        this->setReferencedFileCount_(fileCount);
    }
    
    uint32_t        symbolIndexInStringTable_() const             INLINE { return E::get32(entry.symbolIndexInStringTable_); }
    void            setSymbolIndexInStringTable_(uint32_t value)    INLINE { E::set32(entry.symbolIndexInStringTable_, value); }
    
    uint32_t        referencedFileCount_() const             INLINE { return E::get32(entry.referencedFileCount_); }
    void            setReferencedFileCount_(uint32_t value)    INLINE { E::set32(entry.referencedFileCount_, value); }
        
    const uint32_t*    referencedFileIndex_() const                             {
        return entry.referencedFileIndex_;
    }
    
    void            setReferencedFileIndex_(const std::set<uint32_t> &buffer)         {
        for (auto it = buffer.begin(); it != buffer.end(); it++) {
            E::set32(entry.referencedFileIndex_, *it);
        }
    }
    
    typedef typename P::E        E;
    
private:
    GlobalSymbolRefEntry    entry;
};

// Incremental LoadCommand
template <typename P>
class IncrementalCommand {
public:
    uint32_t    cmd() const          INLINE { return E::get32(fields.cmd); }
    void      set_cmd(uint32_t value)    INLINE { E::set32(fields.cmd, value); }

    uint32_t    cmdsize() const        INLINE { return E::get32(fields.cmdsize); }
    void      set_cmdsize(uint32_t value)  INLINE { E::set32(fields.cmdsize, value); }

    uint32_t    file_count() const        INLINE { return E::get32(fields.file_count); }
    void      set_file_count(uint32_t value)  INLINE { E::set32(fields.file_count, value);  }
    
    uint32_t    inputs_off() const        INLINE { return E::get32(fields.inputs_off); }
    void      set_inputs_off(uint32_t value)  INLINE { E::set32(fields.inputs_off, value);  }
  
    uint32_t    inputs_size() const      INLINE { return E::get32(fields.inputs_size); }
    void      set_inputs_size(uint32_t value)INLINE { E::set32(fields.inputs_size, value);  }
    
    uint32_t    symtab_off() const        INLINE { return E::get32(fields.symtab_off); }
    void      set_symtab_off(uint32_t value)  INLINE { E::set32(fields.symtab_off, value);  }
    
    uint32_t    symtab_size() const        INLINE { return E::get32(fields.symtab_size); }
    void      set_symtab_size(uint32_t value)  INLINE { E::set32(fields.symtab_size, value);  }
    
    uint32_t    strtab_off() const        INLINE { return E::get32(fields.strtab_off); }
    void      set_strtab_off(uint32_t value)  INLINE { E::set32(fields.strtab_off, value);  }
    
    uint32_t    strtab_size() const        INLINE { return E::get32(fields.strtab_size); }
    void      set_strtab_size(uint32_t value)  INLINE { E::set32(fields.strtab_size, value);  }
  
    typedef typename P::E    E;
    
private:
    struct macho_incremental_command  fields;
};

}  // incremental
}  // ld

#endif /* incremental_hpp */
