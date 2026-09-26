#pragma once
#include <cstdint>
#include <cstddef>

// ── MappedFile ────────────────────────────────────────────────────────────────
// Cross-platform memory mapped file abstraction
// Windows: CreateFileMapping / MapViewOfFile
// Linux:   mmap / msync

class MappedFile {
public:
    MappedFile() = default;
    ~MappedFile() { close(); }

    // Non-copyable — owns the file handle and mapping
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    // Open or create a memory mapped file of exactly size bytes
    // Returns true on success
    // isNew is set to true if the file was created, false if it already existed
    bool open(const char* path, size_t size, bool& isNew);

    // Flush changes to disk
    void flush();

    // Close the mapping and file handles
    void close();

    // Returns pointer to the mapped memory — cast to your struct layout
    void* data()     const { return mapped_; }
    size_t size()     const { return size_; }
    bool   isOpen()   const { return mapped_ != nullptr; }

private:
    void* mapped_ = nullptr;
    size_t size_ = 0;

#ifdef _WIN32
    void* fileHandle_ = nullptr;   // HANDLE
    void* mapHandle_ = nullptr;   // HANDLE
#else
    int   fd_ = -1;
#endif
};