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
    static bool canIncrementalUpdate(const uint8_t *fileContent);
    static bool validFile(const uint8_t *fileContent);

    Parser(const uint8_t *fileContent, uint64_t fileLength, const char *path, time_t modTime);

private:
    typedef typename A::P P;
    typedef typename A::P::E E;
    typedef typename A::P::uint_t pint_t;

    const uint8_t*                              fileContent_;
    uint32_t                                    fileLength_;
    std::vector<InputEntry> incrInputs_;
};

template <typename A>
bool Parser<A>::canIncrementalUpdate(const uint8_t *fileContent) {
    return true;
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
    if ((access(_options.outputFilePath(), F_OK) == 0) &&
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
                    (void)unlink(_options.outputFilePath());
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
    // Construct a temporary path of the form {outputFilePath}.ld_XXXXXX
    const char filenameTemplate[] = ".ld_XXXXXX";
    char tmpOutput[PATH_MAX];
    uint8_t *wholeBuffer;
    if (outputIsRegularFile && outputIsMappableFile) {
        // <rdar://problem/20959031> ld64 should clean up temporary files on SIGINT
        ::signal(SIGINT, removePathAndExit);

        strcpy(tmpOutput, _options.outputFilePath());
        // If the path is too long to add a suffix for a temporary name then
        // just fall back to using the output path.
        if (strlen(tmpOutput) + strlen(filenameTemplate) < PATH_MAX) {
            strcat(tmpOutput, filenameTemplate);
            fd = mkstemp(tmpOutput);
            sDescriptorOfPathToRemove = fd;
        } else {
            fd = open(tmpOutput, O_RDWR | O_CREAT, permissions);
        }
        if (fd == -1) {
            throwf("can't open output file for incremetal update '%s', errno=%d", tmpOutput, errno);
        }

        wholeBuffer = (uint8_t *)mmap(NULL, stat_buf.st_size, PROT_WRITE | PROT_READ, MAP_SHARED, fd, 0);
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
        if (::chmod(tmpOutput, permissions) == -1) {
            unlink(tmpOutput);
            throwf("can't set permissions on output file: %s, errno=%d", tmpOutput, errno);
        }
        if (::rename(tmpOutput, _options.outputFilePath()) == -1 && strcmp(tmpOutput, _options.outputFilePath()) != 0) {
            unlink(tmpOutput);
            throwf("can't move output file in place, errno=%d", errno);
        }
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
