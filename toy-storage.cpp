
#include "toy-storage.h"

#ifdef _WIN32
#include <Windows.h>
#else
#include <sys/mman.h>   // mmap, msync, munmap
#include <sys/stat.h>   // stat
#include <fcntl.h>      // open, O_RDWR, O_CREAT
#include <unistd.h>     // ftruncate, close
#endif

#include <cstdio>

// ── Open ──────────────────────────────────────────────────────────────────────

bool MappedFile::open(const char* path, size_t size, bool& isNew) {
    size_ = size;
    isNew = false;

#ifdef _WIN32

    // Detect whether the file already exists before opening
    DWORD attr = GetFileAttributesA(path);
    isNew = (attr == INVALID_FILE_ATTRIBUTES);

    // Open or create the file
    HANDLE fh = CreateFileA(
        path,
        GENERIC_READ | GENERIC_WRITE,
        0,          // no sharing
        nullptr,    // default security
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (fh == INVALID_HANDLE_VALUE) {
        printf("[MappedFile] CreateFileA failed: %lu\n", GetLastError());
        return false;
    }

    fileHandle_ = fh;

    // Set the file size — only needed on creation
    // On subsequent opens the file is already the correct size
    if (isNew) {
        LARGE_INTEGER li;
        li.QuadPart = static_cast<LONGLONG>(size);
        if (!SetFilePointerEx(fh, li, nullptr, FILE_BEGIN) ||
            !SetEndOfFile(fh)) {
            printf("[MappedFile] SetEndOfFile failed: %lu\n", GetLastError());
            close();
            return false;
        }
    }

    // Create the file mapping
    HANDLE mh = CreateFileMappingA(
        fh,
        nullptr,            // default security
        PAGE_READWRITE,
        0,                  // high-order DWORD of size — 0 uses file size
        0,                  // low-order DWORD of size — 0 uses file size
        nullptr             // no name for the mapping object
    );

    if (mh == nullptr) {
        printf("[MappedFile] CreateFileMappingA failed: %lu\n", GetLastError());
        close();
        return false;
    }

    mapHandle_ = mh;

    // Map the view into process address space
    void* ptr = MapViewOfFile(
        mh,
        FILE_MAP_ALL_ACCESS,
        0, 0,       // offset — map from the beginning
        size
    );

    if (ptr == nullptr) {
        printf("[MappedFile] MapViewOfFile failed: %lu\n", GetLastError());
        close();
        return false;
    }

    mapped_ = ptr;

#else   // ── Linux ─────────────────────────────────────────────────────────────

    // Detect whether the file already exists before opening
    struct stat st;
    isNew = (stat(path, &st) != 0);

    // Open or create the file
    int fd = ::open(path, O_RDWR | O_CREAT, 0644);
    if (fd == -1) {
        perror("[MappedFile] open");
        return false;
    }

    fd_ = fd;

    // Set the file size — ftruncate extends or truncates to exactly size bytes
    // On Linux newly extended bytes are guaranteed to be zero
    if (ftruncate(fd, static_cast<off_t>(size)) == -1) {
        perror("[MappedFile] ftruncate");
        close();
        return false;
    }

    // Map the file into process address space
    void* ptr = mmap(
        nullptr,            // let the OS choose the address
        size,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,         // changes are written back to the file
        fd,
        0                   // offset — map from the beginning
    );

    if (ptr == MAP_FAILED) {
        perror("[MappedFile] mmap");
        close();
        return false;
    }

    mapped_ = ptr;

#endif

    return true;
}

// ── Flush ─────────────────────────────────────────────────────────────────────

void MappedFile::flush() {
    if (!mapped_) return;

#ifdef _WIN32
    FlushViewOfFile(mapped_, size_);
    if (fileHandle_)
        FlushFileBuffers(static_cast<HANDLE>(fileHandle_));
#else
    msync(mapped_, size_, MS_SYNC);
#endif
}

// ── Close ─────────────────────────────────────────────────────────────────────

void MappedFile::close() {
#ifdef _WIN32
    if (mapped_) {
        UnmapViewOfFile(mapped_);
        mapped_ = nullptr;
    }
    if (mapHandle_) {
        CloseHandle(static_cast<HANDLE>(mapHandle_));
        mapHandle_ = nullptr;
    }
    if (fileHandle_) {
        CloseHandle(static_cast<HANDLE>(fileHandle_));
        fileHandle_ = nullptr;
    }
#else
    if (mapped_) {
        munmap(mapped_, size_);
        mapped_ = nullptr;
    }
    if (fd_ != -1) {
        ::close(fd_);
        fd_ = -1;
    }
#endif
    size_ = 0;
}