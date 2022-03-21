//
//  incremental.cpp
//  ld
//
//  Created by tttt on 2022/2/14.
//  Copyright © 2022 Apple Inc. All rights reserved.
//

#include <sys/mount.h>
#include <string.h>

#include "Architectures.hpp"
#include "incremental.hpp"

namespace ld {
namespace incremental {

//
// An StubAtom has no content.  It exists so that the linker can track which
// imported symbols came from which dynamic libraries.
//
class StubAtom final : public ld::Atom
{
public:
    StubAtom(const char *nm, const char *im, uint32_t cv, bool weakDef, bool tlv, uint64_t address)
        : ld::Atom(_s_section, ld::Atom::definitionProxy,
                   (weakDef ? ld::Atom::combineByName : ld::Atom::combineNever),
                   ld::Atom::scopeLinkageUnit,
                   (tlv ? ld::Atom::typeTLV : ld::Atom::typeUnclassified),
                   symbolTableNotIn, false, false, false, ld::Atom::Alignment(0)),
          _name(nm),
          _installname(im),
          _address(address),
          _compatVersion(cv)
    {}
    virtual                 ~StubAtom() {}
    
    // overrides of ld::Atom
    virtual const ld::File*    file() const override final { return nullptr; }
    virtual const char*        name() const override final { return _name; }
    virtual uint64_t        size() const override final { return 0; }
    virtual uint64_t        objectAddress() const override final { return _address; }
    virtual void            copyRawContent(uint8_t buffer[]) const override final { }

    virtual void            setScope(Scope) { }
    virtual void            setFile(const ld::File* file) override {};
    const char *            installname() const { return _installname; }
    uint32_t                compat_version() const { return _compatVersion; }
private:

//    const File*                _file;
    const char*             _name;
    const char*             _installname;
    uint64_t                _address;
    uint32_t                _compatVersion;
    
    static ld::Section      _s_section;
};

ld::Section StubAtom::_s_section("__TEXT", "__import", ld::Section::typeImportProxies, true);

template <typename A>
class Parser {
public:
    typedef typename A::P P;
    typedef typename A::P::E E;
    typedef typename A::P::uint_t pint_t;
    using IncrInputMap = std::unordered_map<std::string, InputEntrySection<P> *>;
    using IncrPatchSpaceMap = std::unordered_map<std::string, PatchSpace>;
    
    static bool validFile(const uint8_t *fileContent);
    
    Parser(const uint8_t *fileContent, uint64_t fileLength, const char *path, time_t modTime);
    bool hasValidEntryPoint() const { return entryPoint_ != nullptr; }
    bool canIncrementalUpdate();
    IncrInputMap &incrInputsMap() { return incrInputsMap_; }
    constexpr IncrFixupsMap &incrFixupsMap() { return incrFixupsMap_; }
    IncrPatchSpaceMap &patchSpaceMap() { return incrPatchSpaceMap_; }
    constexpr std::vector<const ld::Atom *> &stubAtoms() { return stubAtoms_; }
    constexpr std::unordered_map<std::string, uint64_t> &sectionStartAddressMap() { return sectionStartAddressMap_; }
    constexpr std::unordered_map<std::string, uint32_t> &sectionFileOffsetMap() { return sectionFileOffsetMap_; }
    constexpr uint64_t baseAddress() const { return baseAddress_; }
    
private:
    void checkMachOHeader();
    bool isStaticExecutable() const;
    void parseSymbolTable(const macho_load_command<P> *cmd);
    
    void parseIndirectSymbolTable();
    uint32_t indirectSymbol(uint32_t indirectIndex) const;
    const macho_nlist<P> &symbolFromIndex(uint32_t index);
    const char *nameFromSymbol(const macho_nlist<P> &sym);
    bool weakImportFromSymbol(const macho_nlist<P> &sym);
    void parseIncrementalSections();
    
    /// Parse incremental fixup sections
    void parseIncrementalFixupSection(const IncrementalCommand<P> *incrementalCommand);
    uint8_t loadCommandSizeMask();
    
    const uint8_t *fileContent_;
    uint32_t fileLength_;
    uint64_t baseAddress_;
    const macho_header<P> *fHeader_;
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
    std::unordered_map<std::string, uint64_t> sectionStartAddressMap_;
    std::unordered_map<std::string, uint32_t> sectionFileOffsetMap_;
    
    // Incremental fixups map
    IncrFixupsMap incrFixupsMap_;
};

template <typename A>
Parser<A>::Parser(const uint8_t *fileContent, uint64_t fileLength, const char *path, time_t modTime) : fileContent_(fileContent), fileLength_(fileLength), baseAddress_(0), fHeader_(nullptr), entryPoint_(nullptr), fDynamicSymbolTable_(nullptr), symbolTable_(nullptr), symbolCount_(0), fIncrementalInputSection_(nullptr), incrementalFixupSection_(nullptr), fIncrementalPatchSpaceSection_(nullptr), fIncrementalSymbolSection_(nullptr), stringTable_(nullptr), stringTableEnd_(nullptr), indirectSymbolTable_(nullptr), indirectTableCount_(0), fIncrementalStrings_(nullptr) {
    if (!validFile(fileContent)) {
        throw "not a mach-o file that can be checked";
    }
    fHeader_ = (const macho_header<P>*)fileContent_;
    this->checkMachOHeader();
    this->parseIncrementalSections();
    this->parseIndirectSymbolTable();
}

template <typename A>
bool Parser<A>::canIncrementalUpdate() {
    return incrInputs_.size() > 0;
}

template <>
bool Parser<x86>::validFile(const uint8_t *fileContent) {
    const macho_header<P> *header = (const macho_header<P> *)fileContent;
    if (header->magic() != MH_MAGIC)
        return false;
    if (header->cputype() != CPU_TYPE_I386)
        return false;
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
    const macho_header<P> *header = (const macho_header<P> *)fileContent;
    if (header->magic() != MH_MAGIC_64)
        return false;
    if (header->cputype() != CPU_TYPE_X86_64)
        return false;
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
    const macho_header<P> *header = (const macho_header<P> *)fileContent;
    if (header->magic() != MH_MAGIC)
        return false;
    if (header->cputype() != CPU_TYPE_ARM)
        return false;
    switch (header->filetype()) {
        case MH_EXECUTE:
        case MH_DYLIB:
        case MH_BUNDLE:
        case MH_DYLINKER:
            return true;
    }
    return false;
}
#endif // SUPPORT_ARCH_arm_any

#if SUPPORT_ARCH_arm64
template <>
bool Parser<arm64>::validFile(const uint8_t *fileContent) {
    const macho_header<P> *header = (const macho_header<P> *)fileContent;
    if (header->magic() != MH_MAGIC_64)
        return false;
    if (header->cputype() != CPU_TYPE_ARM64)
        return false;
    switch (header->filetype()) {
        case MH_EXECUTE:
        case MH_DYLIB:
        case MH_BUNDLE:
        case MH_DYLINKER:
            return true;
    }
    return false;
}
#endif // SUPPORT_ARCH_arm64

#if SUPPORT_ARCH_arm64_32
template <>
bool Parser<arm64_32>::validFile(const uint8_t *fileContent) {
    const macho_header<P> *header = (const macho_header<P> *)fileContent;
    if (header->magic() != MH_MAGIC)
        return false;
    if (header->cputype() != CPU_TYPE_ARM64_32)
        return false;
    switch (header->filetype()) {
        case MH_EXECUTE:
        case MH_DYLIB:
        case MH_BUNDLE:
        case MH_DYLINKER:
            return true;
    }
    return false;
}
#endif // SUPPORT_ARCH_arm64_32

template <> uint8_t Parser<ppc>::loadCommandSizeMask()    { return 0x03; }
template <> uint8_t Parser<ppc64>::loadCommandSizeMask()    { return 0x07; }
template <> uint8_t Parser<x86>::loadCommandSizeMask()    { return 0x03; }
template <> uint8_t Parser<x86_64>::loadCommandSizeMask() { return 0x07; }
template <> uint8_t Parser<arm>::loadCommandSizeMask()    { return 0x03; }
template <> uint8_t Parser<arm64>::loadCommandSizeMask()    { return 0x07; }
#if SUPPORT_ARCH_arm64_32
template <> uint8_t Parser<arm64_32>::loadCommandSizeMask()    { return 0x03; }
#endif

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
        throw "MH_NO_REEXPORTED_DYLIBS bit of mach_header flags only valid for dylibs";
    }
    switch (fHeader_->filetype()) {
        case MH_EXECUTE:
            fSlidableImage_ = ( flags & MH_PIE );
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
    const macho_load_command<P> *const cmds = (macho_load_command<P>*)((uint8_t*)fHeader_ + sizeof(macho_header<P>));
    const macho_load_command<P> *cmd = cmds;
    if (fHeader_->filetype() == MH_EXECUTE) {
        isStaticExecutable = true;
        cmd = cmds;
        for (uint32_t i = 0; i < cmd_count; ++i) {
            switch ( cmd->cmd() ) {
                case LC_LOAD_DYLINKER:
                    isStaticExecutable = false;
                    break;
            }
            cmd = (const macho_load_command<P>*)(((uint8_t *)cmd) + cmd->cmdsize());
        }
        if (isStaticExecutable) {
            if ((fHeader_->flags() != MH_NOUNDEFS) && (fHeader_->flags() != (MH_NOUNDEFS|MH_PIE))) {
                throw "invalid bits in mach_header flags for static executable";
            }
        }
    }
    return isStaticExecutable;
}

template <typename A>
void Parser<A>::parseIncrementalSections() {
    const uint8_t *const endOfFile = (uint8_t*)fHeader_ + fileLength_;
    const uint8_t *const endOfLoadCommands = (uint8_t*)fHeader_ + sizeof(macho_header<P>) + fHeader_->sizeofcmds();
    const uint32_t cmd_count = fHeader_->ncmds();
    const macho_load_command<P> *const cmds = (macho_load_command<P>*)((uint8_t*)fHeader_ + sizeof(macho_header<P>));
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
                const macho_segment_command<P> *segCmd = (const macho_segment_command<P> *)cmd;
                if (strcmp(segCmd->segname(), "__TEXT") == 0) {
                    baseAddress_ = segCmd->vmaddr();
                } else if (strcmp(segCmd->segname(), "__LINKEDIT") == 0) {
                    linkEditSegment_ = segCmd;
                }
            } break;
            case LC_MAIN: {
                if (fHeader_->filetype() != MH_EXECUTE) {
                    throw "LC_MAIN can only be used in MH_EXECUTE file types";
                }
                entryPoint_ =  (macho_entry_point_command<P> *)cmd;
            } break;
            case LC_FUNCTION_STARTS: {
//                const macho_linkedit_data_command<P> *info = (macho_linkedit_data_command<P> *)cmd;
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
            case LC_INCREMENTAL: {
                incrementalCommand = (IncrementalCommand<P> *)cmd;
                // incremental string pool
                fIncrementalStrings_ = (const char *)((uint8_t *)fHeader_ + incrementalCommand->strtab_off());
                char *stringStart = const_cast<char *>(fIncrementalStrings_);
                char *stringEnd = const_cast<char *>(fIncrementalStrings_) + incrementalCommand->strtab_size();
                uint32_t stringIdx = 0;
                while (stringStart < stringEnd) {
                    const char *symName = &stringStart[stringIdx++];
                    if (!symName || strlen(symName) == 0) {
                        break;
                    }
                    incrStringPool_.push_back(symName);
                    stringStart += strlen(symName);
                }
                
                // incremental input files
                fIncrementalInputSection_ = (const InputEntrySection<P> *)((uint8_t *)fHeader_ + incrementalCommand->inputs_off());
                InputEntrySection<P> *incrInputPtr = const_cast<InputEntrySection<P> *>(fIncrementalInputSection_);
                for (uint32_t index = 0; index < incrementalCommand->file_count(); index++) {
                    size_t size = sizeof(InputEntrySection<P>);
                    switch (incrInputPtr->type()) {
                        case ld::File::Type::Reloc: {
                            uint32_t atomCount = incrInputPtr->atomCount();
                            size = 5 * sizeof(uint32_t) + sizeof(ld::incremental::AtomEntry<P>) * atomCount;
                        } break;
                        case ld::File::Type::Archive: {
                            
                        } break;
                        case ld::File::Type::Dylib: {
                            
                        } break;
                        case ld::File::Type::Other: {
                            
                        } break;
                        default:
                            break;
                    }
                    incrInputs_.push_back(incrInputPtr);
                    incrInputsMap_[incrStringPool_[incrInputPtr->fileIndexInStringTable()]] = incrInputPtr;
                    incrInputPtr = (InputEntrySection<P> *)(((uint8_t *)incrInputPtr) + size);
                }
                
                this->parseIncrementalFixupSection(incrementalCommand);
                
                // incremental global symbol table
                fIncrementalSymbolSection_ = (const GlobalSymbolTableEntry<P> *)((uint8_t *)fHeader_ + incrementalCommand->symtab_off());
                GlobalSymbolTableEntry<P> *symbolStart = const_cast<GlobalSymbolTableEntry<P> *>(fIncrementalSymbolSection_);
                const GlobalSymbolTableEntry<P> *symbolEnd = (const GlobalSymbolTableEntry<P> *)((uint8_t *)fHeader_ + incrementalCommand->symtab_off() + incrementalCommand->symtab_size());
                while (symbolStart < symbolEnd) {
                    if ((symbolEnd - symbolStart) < 8) {
                        break;
                    }
                    incrSymbols_.push_back(symbolStart);
                    symbolStart = (GlobalSymbolTableEntry<P> *)(((uint8_t *)symbolStart) + (2 + symbolStart->referencedFileCount_()) * sizeof(uint32_t));
                }
                
                // patch space
                fIncrementalPatchSpaceSection_ = (const PatchSpaceSectionEntry<P> *)((uint8_t *)fHeader_ + incrementalCommand->patch_space_off());
                const PatchSpaceSectionEntry<P> *patchSpaceEnd = (const PatchSpaceSectionEntry<P> *)((uint8_t *)fHeader_ + incrementalCommand->patch_space_off() + incrementalCommand->patch_space_size());
                PatchSpaceSectionEntry<P> *p = const_cast<PatchSpaceSectionEntry<P> *>(fIncrementalPatchSpaceSection_);
                uint32_t patchCount = patchSpaceEnd - fIncrementalPatchSpaceSection_;
                for (uint32_t patchIndex = 0; patchIndex < patchCount; ++patchIndex, ++p) {
                    PatchSpace patchSpace;
                    strncpy(patchSpace.sectname, p->sectname(), 17);
                    patchSpace.patchOffset_ = p->patchOffset();
                    patchSpace.patchSpace_ = p->patchSpace();
                    incrPatchSpaceMap_[patchSpace.sectname] = patchSpace;
                }
            }
                break;
            default:
                break;
        }
        cmd = (const macho_load_command<P>*)endOfCmd;
    }
}

template <typename A>
void Parser<A>::parseIncrementalFixupSection(const IncrementalCommand<P> *incrementalCommand) {
    incrementalFixupSection_ = (const InputFileFixupSection<P> *)((uint8_t *)fHeader_ + incrementalCommand->fixups_off());
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
void Parser<A>::parseSymbolTable(const macho_load_command<P> *cmd) {
    const macho_symtab_command<P> *symtab = (macho_symtab_command<P> *)cmd;
    symbolCount_ = symtab->nsyms();
    if (symbolCount_ != 0) {
        symbolTable_ = (const macho_nlist<P> *)((uint8_t *)fHeader_ + symtab->symoff());
        if (symtab->symoff() < linkEditSegment_->fileoff()) {
            throw "symbol table not in __LINKEDIT";
        }
        if ((symtab->symoff() + symbolCount_ * sizeof(macho_nlist<P> *)) > symtab->stroff()) {
            throw "symbol table overlaps string pool";
        }
        if ((symtab->symoff() % sizeof(pint_t)) != 0) {
            throw "symbol table start not pointer aligned";
        }
    }
    stringTable_ = (char *)fHeader_ + symtab->stroff();
    stringTableEnd_ = stringTable_ + symtab->strsize();
    if (symtab->stroff() < linkEditSegment_->fileoff()) {
        throw "string pool not in __LINKEDIT";
    }
    if ((symtab->stroff() + symtab->strsize()) > (linkEditSegment_->fileoff() + linkEditSegment_->filesize())) {
        throw "string pool extends beyond __LINKEDIT";
    }
    if ((symtab->stroff() % 4) != 0) {
        throw "string pool start not pointer aligned";
    }
}

template <typename A>
void Parser<A>::parseIndirectSymbolTable() {
    const macho_load_command<P> *const cmds = (macho_load_command<P>*)((uint8_t*)fHeader_ + sizeof(macho_header<P>));
    const uint32_t cmd_count = fHeader_->ncmds();
    indirectSymbolTable_ = (uint32_t *)((uint8_t *)fHeader_ + fDynamicSymbolTable_->indirectsymoff());
    indirectTableCount_ = fDynamicSymbolTable_->nindirectsyms();
    if (indirectTableCount_ != 0) {
        if (fDynamicSymbolTable_->indirectsymoff() < linkEditSegment_->fileoff()) {
            throw "indirect symbol table not in __LINKEDIT";
        }
        if ((fDynamicSymbolTable_->indirectsymoff() + indirectTableCount_ * 4) > (linkEditSegment_->fileoff() + linkEditSegment_->filesize())) {
            throw "indirect symbol table not in __LINKEDIT";
        }
        if ((fDynamicSymbolTable_->indirectsymoff() % sizeof(pint_t)) != 0 ) {
            throw "indirect symbol table not pointer aligned";
        }
        
        const macho_load_command<P> *cmd = cmds;
        for (uint32_t i = 0; i < cmd_count; ++i) {
            if (cmd->cmd() == macho_segment_command<P>::CMD) {
                const macho_segment_command<P> *segCmd = (const macho_segment_command<P> *)cmd;
                const macho_section<P> *const sectionsStart = (macho_section<P>*)((char*)segCmd + sizeof(macho_segment_command<P>));
                const macho_section<P> *const sectionsEnd = &sectionsStart[segCmd->nsects()];
                for (const macho_section<P> *sect = sectionsStart; sect < sectionsEnd; ++sect) {
                    // make sure all magic sections that use indirect symbol table fit within it
                    uint32_t start = 0;
                    uint32_t elementSize = 0;
                    switch (sect->flags() & SECTION_TYPE) {
                        case S_SYMBOL_STUBS:
                            elementSize = sect->reserved2();
                            start = sect->reserved1();
                            break;
//                        case S_LAZY_SYMBOL_POINTERS:
                        case S_NON_LAZY_SYMBOL_POINTERS:
                            elementSize = sizeof(pint_t);
                            start = sect->reserved1();
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
//                                const char *fromDylib = classicOrdinalName(GET_LIBRARY_ORDINAL(sym->n_desc()));
                                fprintf(stderr, "sect:%s, stub symbol:%s, %llu\n", sect->sectname(), this->nameFromSymbol(sym), (uint64_t)address);
                                stubAtoms_.push_back(new ld::incremental::StubAtom(this->nameFromSymbol(sym), "", 1, this->weakImportFromSymbol(sym), false, (uint64_t)address));
                            }
                        }
                    }
                    
                    const char *name = sect->sectname();
                    if (strlen(name) >= 16) {
                        char sectionName[17];
                        strlcpy(sectionName, name, 17);
                        sectionStartAddressMap_[sectionName] = sect->addr();
                        sectionFileOffsetMap_[sectionName] = sect->offset();
                    } else {
                        sectionStartAddressMap_[name] = sect->addr();
                        sectionFileOffsetMap_[name] = sect->offset();
                    }
                }
            }
            cmd = (const macho_load_command<P>*)(((uint8_t *)cmd) + cmd->cmdsize());
        }
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
const macho_nlist<typename A::P>& Parser<A>::symbolFromIndex(uint32_t index) {
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
    return (((sym.n_type() & N_TYPE) == N_UNDF) && ((sym.n_desc() & N_WEAK_REF) != 0));
}

#pragma mark - Incremental

static int sDescriptorOfPathToRemove = -1;
static void removePathAndExit(int sig) {
    if (sDescriptorOfPathToRemove != -1) {
        char path[MAXPATHLEN];
        if (::fcntl(sDescriptorOfPathToRemove, F_GETPATH, path) == 0)
            ::unlink(path);
    }
    fprintf(stderr, "ld: interrupted\n");
    // we are in a sig handler, don't do clean ups
    _exit(1);
}

void Incremental::openBinary() {
    if (access(_options.outputFilePath(), F_OK) != 0) {
        return;
    }
    if ((access(_options.outputFilePath(), W_OK) == -1) && (access(_options.outputFilePath(), R_OK) == -1)) {
        throwf("can't read/write output file: %s", _options.outputFilePath());
    }

    mode_t permissions = 0777;
    if (_options.outputKind() == Options::kObjectFile)
        permissions = 0666;
    mode_t umask = ::umask(0);
    ::umask(umask); // put back the original umask
    permissions &= ~umask;
    // Calling unlink first assures the file is gone so that open creates it with correct permissions
    // It also handles the case where __options.outputFilePath() file is not writable but its directory is
    // And it means we don't have to truncate the file when done writing (in case new is smaller than old)
    // Lastly, only delete existing file if it is a normal file (e.g. not /dev/null).
    struct stat stat_buf;
    bool outputIsRegularFile = false;
    bool outputIsMappableFile = false;

    if (stat(_options.outputFilePath(), &stat_buf) != -1) {
        if (stat_buf.st_mode & S_IFREG) {
            outputIsRegularFile = true;
            // <rdar://problem/12264302> Don't use mmap on non-hfs volumes
            struct statfs fsInfo;
            if (statfs(_options.outputFilePath(), &fsInfo) != -1) {
                if ((strcmp(fsInfo.f_fstypename, "hfs") == 0) || (strcmp(fsInfo.f_fstypename, "apfs") == 0)) {
                    outputIsMappableFile = true;
                }
            } else {
                outputIsMappableFile = false;
            }
        } else {
            outputIsRegularFile = false;
        }
    } else {
        // special files (pipes, devices, etc) must already exist
        outputIsRegularFile = true;
        // output file does not exist yet
        char dirPath[PATH_MAX];
        strcpy(dirPath, _options.outputFilePath());
        char *end = strrchr(dirPath, '/');
        if (end != NULL) {
            end[1] = '\0';
            struct statfs fsInfo;
            if (statfs(dirPath, &fsInfo) != -1) {
                if ((strcmp(fsInfo.f_fstypename, "hfs") == 0) || (strcmp(fsInfo.f_fstypename, "apfs") == 0)) {
                    outputIsMappableFile = true;
                }
            }
        }
    }

    if (outputIsRegularFile && outputIsMappableFile) {
        // <rdar://problem/20959031> ld64 should clean up temporary files on SIGINT
        ::signal(SIGINT, removePathAndExit);
        fd_ = open(_options.outputFilePath(), O_RDWR | O_CREAT, permissions);
        if (fd_ == -1) {
            throwf("can't open output file for incremetal update '%s', errno=%d", _options.outputFilePath(), errno);
        }
        wholeBuffer_ = (uint8_t *)::mmap(NULL, stat_buf.st_size, PROT_WRITE | PROT_READ, MAP_SHARED, fd_, 0);
        if (wholeBuffer_ == MAP_FAILED) {
            throwf("can't create buffer of %llu bytes for output", stat_buf.st_size);
        }
    }

    std::set<std::string> incrementalFiles;
    switch (_options.architecture()) {
#if SUPPORT_ARCH_x86_64
        case CPU_TYPE_X86_64: {
            Parser<x86_64> parser(wholeBuffer_, stat_buf.st_size, _options.outputFilePath(), stat_buf.st_mtime);
        }
            break;
#endif
#if SUPPORT_ARCH_i386
        case CPU_TYPE_I386: {
            Parser<x86> parser(wholeBuffer_, stat_buf.st_size, _options.outputFilePath(), stat_buf.st_mtime);
        }
            break;
#endif
#if SUPPORT_ARCH_arm_any
        case CPU_TYPE_ARM: {
            Parser<arm> parser(wholeBuffer_, stat_buf.st_size, _options.outputFilePath(), stat_buf.st_mtime);
        }
            break;
#endif
#if SUPPORT_ARCH_arm64
        case CPU_TYPE_ARM64: {
            Parser<arm64> parser(wholeBuffer_, stat_buf.st_size, _options.outputFilePath(), stat_buf.st_mtime);
            if (parser.hasValidEntryPoint()) {
                _options.markIgnoreEntryPoint();
            }
            for (auto it = _options.getInputFiles().begin(); it != _options.getInputFiles().end(); it++) {
                if (!it->fromFileList) {
                    continue;
                }
                time_t rawModTime = it->modTime;
                auto incrFile = parser.incrInputsMap()[it->path];
                if (!incrFile) {
                    fprintf(stderr, "incremental created new file:%s\n", it->path);
                    continue;
                }
                time_t incrInputModTime = incrFile->modTime();
                if (rawModTime > incrInputModTime) {
                    fprintf(stderr, "incremental changed file:%s\n", it->path);
                    continue;
                }
                incrementalFiles.insert((*it).path);
            }
            _options.removeIncrementalInputFiles(incrementalFiles);
            patchSpace_ = std::move(parser.patchSpaceMap());
            stubAtoms_ = parser.stubAtoms();
            incrFixupsMap_ = parser.incrFixupsMap();
            sectionStartAddressMap_ = parser.sectionStartAddressMap();
            sectionFileOffsetMap_ = parser.sectionFileOffsetMap();
            baseAddress_ = parser.baseAddress();
        }
            break;
#endif
#if SUPPORT_ARCH_arm64_32
        case CPU_TYPE_ARM64_32: {
                Parser<arm64_32> parser(wholeBuffer_, stat_buf.st_size, _options.outputFilePath(), stat_buf.st_mtime);
        }
            break;
#endif
    }
    _options.markValidIncrementalUpdate();
}

void Incremental::closeBinary() {
    ::close(fd_);
}

void Incremental::forEachStubAtom(ld::File::AtomHandler &handler, ld::Internal &state) {
    for (auto ait = stubAtoms_.begin(); ait != stubAtoms_.end(); ++ait) {
        handler.doAtom(*(*ait));
    }
}

void Incremental::forEachStubAtom(const std::function<void (const ld::Atom *)> &handler) {
    for (auto ait = stubAtoms_.begin(); ait != stubAtoms_.end(); ++ait) {
        handler(*ait);
    }
}

} // namespace incremental
} // namespace ld
