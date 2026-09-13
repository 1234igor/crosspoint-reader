#pragma once
#include <HalStorage.h>

#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

class ZipFile {
 public:
  struct FileStatSlim {
    uint16_t method;             // Compression method
    uint32_t compressedSize;     // Compressed size
    uint32_t uncompressedSize;   // Uncompressed size
    uint32_t localHeaderOffset;  // Offset of local file header
  };

  struct ZipDetails {
    uint32_t centralDirOffset;
    uint16_t totalEntries;
    bool isSet;
  };

  // Target for batch uncompressed size lookup (sorted by hash, then len)
  struct SizeTarget {
    uint64_t hash;   // FNV-1a 64-bit hash of normalized path
    uint16_t len;    // Length of path for collision reduction
    uint16_t index;  // Caller's index (e.g. spine index)
  };

  // FNV-1a 64-bit hash computed from char buffer (no std::string allocation)
  static uint64_t fnvHash64(const char* s, size_t len) {
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < len; i++) {
      hash ^= static_cast<uint8_t>(s[i]);
      hash *= 1099511628211ull;
    }
    return hash;
  }

 private:
  const std::string& filePath;
  HalFile file;
  // Read-ahead for the central-directory walks: each entry is ~12 tiny reads,
  // which this turns into memcpy from one 512-byte SD read. Data reads of a
  // chunk size >= 512 bypass the buffer (see HalFile::setReadAhead). Heap,
  // not stack: ZipFile is built on the stack inside expat callbacks on the
  // 8 KB render task.
  static constexpr size_t SCAN_BUF_SIZE = 512;
  std::unique_ptr<uint8_t[]> scanBuf_;
  ZipDetails zipDetails = {0, 0, false};

  // Persistent central-directory index (see buildIndex). Records sorted by
  // (hash, nameLen); a lookup is a binary search over the file plus one name
  // verification read, instead of a linear walk of the central directory.
  struct IndexHeader {
    uint32_t magic;
    uint32_t zipSize;
    uint16_t entries;
    uint16_t reserved;
  };
  struct IndexRec {
    uint64_t hash;
    uint32_t cdOffset;  // central-directory entry offset, for name verification
    uint32_t localHeaderOffset;
    uint32_t compressedSize;
    uint32_t uncompressedSize;
    uint16_t method;
    uint16_t nameLen;
  };
  static constexpr uint32_t INDEX_MAGIC = 0x58444943;  // "CIDX"
  std::string indexPath_;
  enum class IndexLookup { Found, NotFound, Unavailable };
  IndexLookup lookupIndex(const char* filename, FileStatSlim* fileStat);
  std::unordered_map<std::string, FileStatSlim> fileStatSlimCache;

  // Cursor for sequential central-dir scanning optimization
  uint32_t lastCentralDirPos = 0;
  bool lastCentralDirPosValid = false;

  bool loadFileStatSlim(const char* filename, FileStatSlim* fileStat);
  long getDataOffset(const FileStatSlim& fileStat);
  bool loadZipDetails();

 public:
  explicit ZipFile(const std::string& filePath) : filePath(filePath) {}
  // indexPath: where buildIndex() wrote (or will write) this zip's index.
  ZipFile(const std::string& filePath, std::string indexPath) : filePath(filePath), indexPath_(std::move(indexPath)) {}
  ~ZipFile() = default;
  // Scan the central directory once and write the sorted index to indexPath_.
  // Validated on use against the zip's size and entry count, so a replaced
  // book silently falls back to the linear scan until it is re-indexed.
  bool buildIndex();
  // Zip file can be opened and closed by hand in order to allow for quick calculation of inflated file size
  // It is NOT recommended to pre-open it for any kind of inflation due to memory constraints
  bool isOpen() const { return !!file; }
  bool open();
  bool close();
  bool loadAllFileStatSlims();
  bool getInflatedFileSize(const char* filename, size_t* size);
  // Batch lookup: scan ZIP central dir once and fill sizes for matching targets.
  // targets must be sorted by (hash, len). sizes[target.index] receives uncompressedSize.
  // Returns number of targets matched.
  int fillUncompressedSizes(std::deque<SizeTarget>& targets, std::deque<uint32_t>& sizes);
  // Due to the memory required to run each of these, it is recommended to not preopen the zip file for multiple
  // These functions will open and close the zip as needed
  uint8_t* readFileToMemory(const char* filename, size_t* size = nullptr, bool trailingNullByte = false);
  // allowEarlyStop: a short write from `out` is treated as the sink asking to
  // stop (returns true) instead of a write failure — used by header probes
  // that only need the first bytes of an entry.
  bool readFileToStream(const char* filename, Print& out, size_t chunkSize, bool allowEarlyStop = false);

  template <typename F>
  bool enumerateFilePaths(F&& callback) {
    if (!fileStatSlimCache.empty()) {
      for (const auto& entry : fileStatSlimCache) {
        callback(std::string_view{entry.first});
      }
      return true;
    }

    return enumerateFileEntries([&callback](std::string_view path, uint32_t, uint32_t) { callback(path); });
  }

  // Callback receives (path, crc32, compressedSize) for each central-directory
  // entry. Always scans the central directory: the slim-stat cache does not
  // hold CRCs.
  template <typename F>
  bool enumerateFileEntries(F&& callback) {
    const bool wasOpen = isOpen();
    if (!wasOpen && !open()) {
      return false;
    }

    if (!loadZipDetails()) {
      if (!wasOpen) {
        close();
      }
      return false;
    }

    file.seek(zipDetails.centralDirOffset);

    uint32_t sig;
    char itemName[256];

    while (file.available()) {
      file.read(&sig, 4);
      if (sig != 0x02014b50) {
        break;
      }

      file.seekCur(12);
      uint32_t crc32, compressedSize;
      file.read(&crc32, 4);
      file.read(&compressedSize, 4);
      file.seekCur(4);
      uint16_t nameLen, m, k;
      file.read(&nameLen, 2);
      file.read(&m, 2);
      file.read(&k, 2);
      file.seekCur(12);

      if (nameLen < sizeof(itemName)) {
        file.read(itemName, nameLen);
        itemName[nameLen] = '\0';
        callback(std::string_view{itemName, nameLen}, crc32, compressedSize);
      } else {
        file.seekCur(nameLen);
      }

      file.seekCur(m + k);
    }

    if (!wasOpen) {
      close();
    }
    return true;
  }
};
