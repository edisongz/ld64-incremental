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
struct incremental_input_entry {
    uint32_t    fileIndexInStringTable_;            // file index in string table
    uint64_t    modTime_;                           // last link input file modification time
    uint32_t    type_;
};

struct macho_incremental_command {
    uint32_t  cmd;        // LC_INCREMENT
    uint32_t  cmdsize;    // sizeof(struct incremental_command)
    uint64_t  file_off;    // file offset of data in __LINKEDIT segment
    uint64_t  file_size;   // file size of data in __LINKEDIT segment
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
    incremental_input_entry    entry;
};

template <typename P>
class IncrementalCommand {
public:
    uint32_t    cmd() const          INLINE { return E::get32(fields.cmd); }
    void      set_cmd(uint32_t value)    INLINE { E::set32(fields.cmd, value); }

    uint32_t    cmdsize() const        INLINE { return E::get32(fields.cmdsize); }
    void      set_cmdsize(uint32_t value)  INLINE { E::set32(fields.cmdsize, value); }

    uint64_t    file_off() const        INLINE { return E::get64(fields.file_off); }
    void      set_file_off(uint64_t value)  INLINE { E::set64(fields.file_off, value);  }
  
    uint64_t    file_size() const      INLINE { return E::get64(fields.file_size); }
    void      set_file_size(uint64_t value)INLINE { E::set64(fields.file_size, value);  }
    
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
