cfe-8.0.1.src:
	curl -# -L https://github.com/llvm/llvm-project/releases/download/llvmorg-8.0.1/cfe-8.0.1.src.tar.xz | tar xJ

dyld-733.6:
	curl -# -L https://opensource.apple.com/tarballs/dyld/dyld-733.6.tar.gz | tar xz
	patch -p1 -d dyld-733.6 < patches/dyld.patch

llvm-8.0.1.src:
	curl -# -L https://github.com/llvm/llvm-project/releases/download/llvmorg-8.0.1/llvm-8.0.1.src.tar.xz | tar xJ

tapi-1100.0.11:
	mkdir -p $@
	curl -# -L https://opensource.apple.com/tarballs/tapi/tapi-1100.0.11.tar.gz | tar xz -C $@ --strip-components=1
	patch -p1 -d $@ < patches/tapi.patch

clean:
	rm -rf build cfe-8.0.1.src dyld-733.6 llvm-8.0.1.src tapi-1100.0.11

ld64-609-patch:
	patch -p1 -d ld64-609 < patches/ld64.patch

fetch: cfe-8.0.1.src dyld-733.6 llvm-8.0.1.src tapi-1100.0.11 ld64-609-patch
