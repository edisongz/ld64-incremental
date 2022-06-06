//
//  incremental.cpp
//  ld
//
//  Created by tttt on 2022/2/14.
//  Copyright © 2022 Apple Inc. All rights reserved.
//

#include "incremental.hpp"

#include <string.h>
#include <sys/mount.h>

namespace ld {
namespace incremental {

static int sDescriptorOfPathToRemove = -1;
static void removePathAndExit(int sig) {
  if (sDescriptorOfPathToRemove != -1) {
    char path[MAXPATHLEN];
    if (::fcntl(sDescriptorOfPathToRemove, F_GETPATH, path) == 0)
      ::unlink(path);
  }
  fprintf(stderr, "ld: interrupted\n");
  // we are in a sig handler, don't do clean uprintfps
  _exit(1);
}

void Incremental::openBinary() {
  if (access(_options.outputFilePath(), F_OK) != 0) {
    return;
  }
  if ((access(_options.outputFilePath(), W_OK) == -1) &&
      (access(_options.outputFilePath(), R_OK) == -1)) {
    throwf("can't read/write output file: %s", _options.outputFilePath());
  }

  mode_t permissions = 0777;
  if (_options.outputKind() == Options::kObjectFile) permissions = 0666;
  mode_t umask = ::umask(0);
  ::umask(umask);  // put back the original umask
  permissions &= ~umask;
  struct stat stat_buf;
  bool outputIsRegularFile = false;
  bool outputIsMappableFile = false;

  if (stat(_options.outputFilePath(), &stat_buf) != -1) {
    if (stat_buf.st_mode & S_IFREG) {
      outputIsRegularFile = true;
      // <rdar://problem/12264302> Don't use mmap on non-hfs volumes
      struct statfs fsInfo;
      if (statfs(_options.outputFilePath(), &fsInfo) != -1) {
        if ((strcmp(fsInfo.f_fstypename, "hfs") == 0) ||
            (strcmp(fsInfo.f_fstypename, "apfs") == 0)) {
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
        if ((strcmp(fsInfo.f_fstypename, "hfs") == 0) ||
            (strcmp(fsInfo.f_fstypename, "apfs") == 0)) {
          outputIsMappableFile = true;
        }
      }
    }
  }

  if (outputIsRegularFile && outputIsMappableFile) {
    ::signal(SIGINT, removePathAndExit);
    fd_ = open(_options.outputFilePath(), O_RDWR | O_CREAT, permissions);
    if (fd_ == -1) {
      throwf("can't open output file for incremetal update '%s', errno=%d",
             _options.outputFilePath(), errno);
    }
    wholeBuffer_ = (uint8_t *)::mmap(
        NULL, stat_buf.st_size, PROT_WRITE | PROT_READ, MAP_SHARED, fd_, 0);
    if (wholeBuffer_ == MAP_FAILED) {
      throwf("can't create buffer of %llu bytes for output", stat_buf.st_size);
    }
  }

  std::set<std::string> incrementalFiles;
  switch (_options.architecture()) {
#if SUPPORT_ARCH_x86_64
    case CPU_TYPE_X86_64: {
      Parser<x86_64> parser(wholeBuffer_, stat_buf.st_size, _options,
                            stat_buf.st_mtime);
    } break;
#endif
#if SUPPORT_ARCH_i386
    case CPU_TYPE_I386: {
      Parser<x86> parser(wholeBuffer_, stat_buf.st_size, _options,
                         stat_buf.st_mtime);
    } break;
#endif
#if SUPPORT_ARCH_arm_any
    case CPU_TYPE_ARM: {
      Parser<arm> parser(wholeBuffer_, stat_buf.st_size, _options,
                         stat_buf.st_mtime);
    } break;
#endif
#if SUPPORT_ARCH_arm64
    case CPU_TYPE_ARM64: {
      Parser<arm64> parser(wholeBuffer_, stat_buf.st_size, _options,
                           stat_buf.st_mtime);
      if (parser.hasValidEntryPoint()) {
        _options.markIgnoreEntryPoint();
      }
      for (auto it = _options.getInputFiles().begin();
           it != _options.getInputFiles().end(); it++) {
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
      objcClassSectionOffsetMap_ = parser.objcClassIndexMap();
      machoNlistSize_ = parser.MachONlistSize();
      symbolCount_ = parser.symbolCount();
      patchSpace_ = parser.patchSpaceMap();
      stubAtoms_ = parser.stubAtoms();
      objcClassRefsAtoms_ = parser.objcClassRefsAtoms();
      stubNames_ = parser.stubNames();
      incrFixupsMap_ = parser.incrFixupsMap();
      baseAddress_ = parser.baseAddress();
      segmentBoundaries_ = parser.segmentBoundaries();
      sectionBoundaryMap_ = parser.sectionBoundaryMap();
      // Dyld info
      rebaseInfo_ = parser.rebaseInfo();
      rebaseAddresses_ = parser.rebaseAddresses();
      bindingInfo_ = parser.bindingInfo();
      lazyBindingInfo_ = parser.lazyBindingInfo();
      dylibToOrdinal_ = parser.dylibToOrdinal();
      dylibNameToOrdinal_ = parser.dylibNameToOrdinal();
      // Symbols
      symToSectionOffset_ = parser.symToSectionOffset();
      symbolTypeToOffset_ = parser.symbolTypeToOffset();
      stringPool_ = parser.stringPool();
      currentBufferUsed_ = parser.currentBufferUsed();
      // Stubs
      got_section_ = parser.GotSection();
      la_symbol_ptr_section_ = parser.LazySymbolPtrSection();
    } break;
#endif
#if SUPPORT_ARCH_arm64_32
    case CPU_TYPE_ARM64_32: {
      Parser<arm64_32> parser(wholeBuffer_, stat_buf.st_size, _options,
                              stat_buf.st_mtime);
    } break;
#endif
  }
  _options.markValidIncrementalUpdate();
}

void Incremental::closeBinary() { ::close(fd_); }

void Incremental::forEachStubAtom(ld::File::AtomHandler &handler,
                                  ld::Internal &state) {
  for (auto ait = stubAtoms_.begin(); ait != stubAtoms_.end(); ++ait) {
    handler.doAtom(*(*ait));
  }
}

void Incremental::forEachStubAtom(
    const std::function<void(const ld::Atom *)> &handler) {
  for (auto ait = stubAtoms_.begin(); ait != stubAtoms_.end(); ++ait) {
    handler(*ait);
  }
}

void Incremental::forEachRefsAtom(ld::File::AtomHandler &handler,
                                  ld::Internal &state) {
  std::for_each(objcClassRefsAtoms_.begin(), objcClassRefsAtoms_.end(),
                [&](const ld::Atom *atom) { handler.doAtom(*atom); });
}

void Incremental::forEachSegmentBoundary(
    const std::function<void(SegmentBoundary &, uint32_t)> &handler) {
  uint32_t index = 0;
  for (auto it = segmentBoundaries_.begin(); it != segmentBoundaries_.end();
       ++it) {
    handler(*it, index++);
  }
}

void Incremental::forEachRebaseInfo(
    const std::function<void(std::pair<uint8_t, uint64_t> &)> &handler) {
  for (auto it = rebaseInfo_.begin(); it != rebaseInfo_.end(); ++it) {
    handler(*it);
  }
}

void Incremental::forEachBindingInfo(
    const std::function<void(BindingInfoTuple &)> &handler) {
  for (auto it = bindingInfo_.begin(); it != bindingInfo_.end(); ++it) {
    handler(*it);
  }
}

void Incremental::forEachLazyBindingInfo(
    const std::function<void(BindingInfoTuple &)> &handler) {
  for (auto it = lazyBindingInfo_.begin(); it != lazyBindingInfo_.end(); ++it) {
    handler(*it);
  }
}

void Incremental::addSymSectionOffset(uint8_t type, const char *symbol) {
  if (symToSectionOffset_.find(type) == symToSectionOffset_.end()) {
    return;
  }
  auto &offsetMap = symToSectionOffset_[type];
  offsetMap[symbol] = symbolCount_++ * machoNlistSize_;
}

uint32_t Incremental::addUnique(const char *symbol) {
  auto it = stringPool_.find(symbol);
  if (it != stringPool_.end()) {
    return it->second;
  }
  uint32_t offset = currentBufferUsed_;
  stringPool_[symbol] = offset;
  appendStrings_.push_back(symbol);
  currentBufferUsed_ += strlen(symbol) + 1;
  return offset;
}

void Incremental::forEachAppendedString(
    const std::function<void(const std::string &)> &handler) {
  for (auto it = appendStrings_.begin(); it != appendStrings_.end(); it++) {
    handler(*it);
  }
}

void Incremental::UpdateIndirectSymbolIndex(const char *sectionName,
                                            uint32_t index) {
#if SUPPORT_ARCH_arm64
  if (strcmp(sectionName, "__got") == 0) {
    assert(got_section_ != nullptr);
    got_section_->set_reserved1(index);
  } else if (strcmp(sectionName, "__la_symbol_ptr") == 0) {
    assert(la_symbol_ptr_section_ != nullptr);
    la_symbol_ptr_section_->set_reserved1(index);
  }
#endif
}

void Incremental::updateDylibOrdinal(
    std::map<const ld::dylib::File *, int> &dylibToOrdinal,
    ld::dylib::File *dylib) {
  const char *shortName = dylibShortName(dylib->leafName());
  auto dit = dylibNameToOrdinal_.find(shortName);
  if (dit != dylibNameToOrdinal_.end()) {
    dylibToOrdinal[dylib] = dit->second;
  }
}

void Incremental::findReferencedAtoms(const ld::Atom *changeAtom) {
  // TODO: find all of referenced atoms
}

}  // namespace incremental
}  // namespace ld
