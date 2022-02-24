//
//  incremental.cpp
//  ld
//
//  Created by tttt on 2022/2/14.
//  Copyright © 2022 Apple Inc. All rights reserved.
//

#include <sys/mount.h>

#include "Architectures.hpp"
#include "incremental.hpp"

namespace ld {
namespace incremental {

template <typename A>
class Parser {
public:
    static bool validFile(const uint8_t *fileContent);
    
    Parser(const uint8_t *fileContent, uint64_t fileLength, const char *path, time_t modTime);
    bool canIncrementalUpdate();
    
private:
    typedef typename A::P P;
    typedef typename A::P::E E;
    typedef typename A::P::uint_t pint_t;

    const uint8_t*                              fileContent_;
    uint32_t                                    fileLength_;
    const macho_header<P>*                      fHeader_;
    const InputEntrySection<P>*                 fIncrementalInputSection_;
    const GlobalSymbolTableEntry<P>*            fIncrementalSymbolSection_;
    const char*                                 fIncrementalStrings_;
    bool fSlidableImage_;
    std::vector<InputEntrySection<P> *> incrInputs_;
    std::vector<GlobalSymbolTableEntry<P> *> incrSymbols_;
    
    void checkMachOHeader();
    void checkIncrementalLoadCommand();
    void checkIncrementalSection(const macho_segment_command<P>* segCmd, const macho_section<P>* sect);
    uint8_t loadCommandSizeMask();
};

template <typename A>
Parser<A>::Parser(const uint8_t *fileContent, uint64_t fileLength, const char *path, time_t modTime) : fileContent_(fileContent), fileLength_(fileLength), fHeader_(nullptr),
fIncrementalInputSection_(nullptr), fIncrementalSymbolSection_(nullptr), fIncrementalStrings_(nullptr) {
    if (!validFile(fileContent)) {
        throw "not a mach-o file that can be checked";
    }
    fHeader_ = (const macho_header<P>*)fileContent_;
    
    checkMachOHeader();
    checkIncrementalLoadCommand();
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
void Parser<A>::checkIncrementalLoadCommand() {
    const uint8_t* const endOfFile = (uint8_t*)fHeader_ + fileLength_;
    const uint8_t* const endOfLoadCommands = (uint8_t*)fHeader_ + sizeof(macho_header<P>) + fHeader_->sizeofcmds();
    const uint32_t cmd_count = fHeader_->ncmds();
    const macho_load_command<P>* const cmds = (macho_load_command<P>*)((uint8_t*)fHeader_ + sizeof(macho_header<P>));
    const macho_load_command<P>* cmd = cmds;
    const IncrementalCommand<P> *incrementalCommand;
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
        if (cmd->cmd() == LC_INCREMENTAL) {
            incrementalCommand = (IncrementalCommand<P> *)cmd;
            // incremental input files
            fIncrementalInputSection_ = (const InputEntrySection<P> *)((uint8_t *)fHeader_ + incrementalCommand->inputs_off());
            InputEntrySection<P> *incrInputPtr = const_cast<InputEntrySection<P> *>(fIncrementalInputSection_);
            for (uint32_t index = 0; index < incrementalCommand->file_count(); index++) {
                switch (fIncrementalInputSection_->type()) {
                    case ld::File::Type::Reloc: {
                        uint32_t atomCount = incrInputPtr->atomCount();
                        incrInputPtr += 5 * sizeof(uint32_t) + sizeof(ld::incremental::AtomEntry<P>) * atomCount;
                    } break;
                    case ld::File::Type::Archive: {
                        incrInputPtr += sizeof(InputEntrySection<P>);
                    } break;
                    case ld::File::Type::Dylib: {
                        incrInputPtr += sizeof(InputEntrySection<P>);
                    } break;
                    case ld::File::Type::Other: {
                        incrInputPtr += sizeof(InputEntrySection<P>);
                    } break;
                    default:
                        break;
                }
                incrInputs_.push_back(incrInputPtr);
            }
            
            // incremental global symbol table
            fIncrementalSymbolSection_ = (const GlobalSymbolTableEntry<P> *)((uint8_t *)fHeader_ + incrementalCommand->symtab_off());
            GlobalSymbolTableEntry<P> *incrSymbolPtr = const_cast<GlobalSymbolTableEntry<P> *>(fIncrementalSymbolSection_);
            
            
            // incremental string pool
            fIncrementalStrings_ = (const char *)((uint8_t *)fHeader_ + incrementalCommand->strtab_off());
            char *stringStart = const_cast<char *>(fIncrementalStrings_);
            char *stringEnd = const_cast<char *>(fIncrementalStrings_) + incrementalCommand->strtab_size();
            uint32_t stringIdx = 0;
            while (stringStart < stringEnd) {
                const char *symName = &stringStart[stringIdx++];
                if (strlen(symName) == 0) {
                    break;
                }
                if (symName) {
                    fprintf(stderr, "symbol:%s\n", symName);
                    stringStart += strlen(symName);
                }
            }
        }
        cmd = (const macho_load_command<P>*)endOfCmd;
    }
}

template <typename A>
void Parser<A>::checkIncrementalSection(const macho_segment_command<P> *segCmd, const macho_section<P> *sect) {
    
}

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

void Incremental::openIncrementalBinary() {
    if (access(_options.outputFilePath(), F_OK) != 0) {
        return;
    }
    if ((access(_options.outputFilePath(), F_OK) != 0) &&
        (access(_options.outputFilePath(), W_OK) == -1) &&
        (access(_options.outputFilePath(), R_OK) == -1)) {
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

    int fd;
    uint8_t *wholeBuffer;
    if (outputIsRegularFile && outputIsMappableFile) {
        // <rdar://problem/20959031> ld64 should clean up temporary files on SIGINT
        ::signal(SIGINT, removePathAndExit);
        fd = open(_options.outputFilePath(), O_RDWR | O_CREAT, permissions);
        if (fd == -1) {
            throwf("can't open output file for incremetal update '%s', errno=%d", _options.outputFilePath(), errno);
        }
        wholeBuffer = (uint8_t *)::mmap(NULL, stat_buf.st_size, PROT_WRITE | PROT_READ, MAP_SHARED, fd, 0);
        if (wholeBuffer == MAP_FAILED) {
            throwf("can't create buffer of %llu bytes for output", stat_buf.st_size);
        }
    } else {
        if (outputIsRegularFile) {
            fd = open(_options.outputFilePath(), O_RDWR | O_CREAT, permissions);
        } else {
            fd = open(_options.outputFilePath(), O_WRONLY);
        }
        if (fd == -1)
            throwf("can't open output file for writing: %s, errno=%d", _options.outputFilePath(), errno);
        // try to allocate buffer for entire output file content
        wholeBuffer = (uint8_t *)calloc(stat_buf.st_size, 1);
        if (wholeBuffer == NULL) {
            throwf("can't create buffer of %llu bytes for output", stat_buf.st_size);
        }
        if (read(fd, wholeBuffer, stat_buf.st_size) != stat_buf.st_size) {
            throwf("can't read incremental file");
        }
    }

    switch ( _options.architecture() ) {
#if SUPPORT_ARCH_x86_64
        case CPU_TYPE_X86_64:
            if (Parser<x86_64>::validFile(wholeBuffer)) {
                Parser<x86_64> parser(wholeBuffer, stat_buf.st_size, _options.outputFilePath(), stat_buf.st_mtime);
            }
            break;
#endif
#if SUPPORT_ARCH_i386
        case CPU_TYPE_I386:
            if (Parser<x86>::validFile(wholeBuffer)) {
                Parser<x86> parser(wholeBuffer, stat_buf.st_size, _options.outputFilePath(), stat_buf.st_mtime);
            }
            break;
#endif
#if SUPPORT_ARCH_arm_any
        case CPU_TYPE_ARM:
            if (Parser<arm>::validFile(wholeBuffer)) {
                Parser<arm> parser(wholeBuffer, stat_buf.st_size, _options.outputFilePath(), stat_buf.st_mtime);
            }
            break;
#endif
#if SUPPORT_ARCH_arm64
        case CPU_TYPE_ARM64:
            if (Parser<arm64>::validFile(wholeBuffer)) {
                Parser<arm64> parser(wholeBuffer, stat_buf.st_size, _options.outputFilePath(), stat_buf.st_mtime);
            }
            break;
#endif
#if SUPPORT_ARCH_arm64_32
        case CPU_TYPE_ARM64_32:
            if (Parser<arm64_32>::validFile(wholeBuffer)) {
                Parser<arm64_32> parser(wholeBuffer, stat_buf.st_size, _options.outputFilePath(), stat_buf.st_mtime);
            }
            break;
#endif
    }
    //    if ( _options.UUIDMode() == Options::kUUIDRandom ) {
    //        uint8_t bits[16];
    //        ::uuid_generate_random(bits);
    //        _headersAndLoadCommandAtom->setUUID(bits);
    //    }

    // compute UUID
    //    if ( _options.UUIDMode() == Options::kUUIDContent )
    //        computeContentUUID(state, wholeBuffer);

    // now that file output buffer is complete, if codesigned, compute each page's hash
    //    if ( _hasCodeSignature )
    //        _codeSignatureAtom->hash(wholeBuffer);

    if (outputIsRegularFile && outputIsMappableFile) {
        ::close(fd);
    } else {
        if (::write(fd, wholeBuffer, stat_buf.st_size) == -1) {
            throwf("can't write to output file: %s, errno=%d", _options.outputFilePath(), errno);
        }
        sDescriptorOfPathToRemove = -1;
        ::close(fd);
        // <rdar://problem/13118223> NFS: iOS incremental builds in Xcode 4.6 fail with codesign error
        // NFS seems to pad the end of the file sometimes.  Calling trunc seems to correct it...
        ::truncate(_options.outputFilePath(), stat_buf.st_size);
    }
}

} // namespace incremental
} // namespace ld
