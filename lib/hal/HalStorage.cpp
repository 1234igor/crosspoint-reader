#include "HalStorage.h"

#include <algorithm>
#include <cstring>

#include <FS.h>  // need to be included before SdFat.h for compatibility with FS.h's File class
#include <Logging.h>
#include <SDCardManager.h>
#if FREEINK_CAP_USB_MSC
#include <UsbMassStorage.h>
#endif

#include <cassert>

#define SDCard SDCardManager::getInstance()

namespace {
#if FREEINK_CAP_USB_MSC
freeink::UsbMassStorage usbMassStorage;
#endif
}  // namespace

HalStorage HalStorage::instance;

HalStorage::HalStorage() {
  // Recursive so the same task can re-enter StorageLock without self-deadlock.
  // openFileForRead/Write take the lock and then assign to a HalFile&
  // out-param; if that out-param already held an Impl, its destructor takes
  // the lock again to close the prior FsFile under serialization (see
  // HalFile::Impl::~Impl below). Priority inheritance still applies to
  // recursive mutexes.
  storageMutex = xSemaphoreCreateRecursiveMutex();
  assert(storageMutex != nullptr);
}

// begin() and ready() are only called from setup, no need to acquire mutex for them

bool HalStorage::begin() { return SDCard.begin(); }

bool HalStorage::ready() const { return SDCard.ready(); }

// For the rest of the methods, we acquire the mutex to ensure thread safety

class HalStorage::StorageLock {
 public:
  StorageLock() { xSemaphoreTakeRecursive(HalStorage::getInstance().storageMutex, portMAX_DELAY); }
  ~StorageLock() { xSemaphoreGiveRecursive(HalStorage::getInstance().storageMutex); }
};

void HalStorage::prepareForDeepSleep() {
  StorageLock lock;
  SDCard.shutdown();
}

#if FREEINK_CAP_USB_MSC && !FREEINK_SD_SDMMC
#error "USB Drive requires an SDMMC-backed storage profile"
#endif

bool HalStorage::beginUsbDrive() {
#if FREEINK_CAP_USB_MSC
  StorageLock lock;
  auto* const blockDevice = SDCard.detachFilesystemForRawAccess();
  if (!blockDevice) {
    LOG_ERR("USB", "USB Drive requires a mounted SDMMC filesystem");
    return false;
  }

  if (!usbMassStorage.begin(blockDevice)) {
    LOG_ERR("USB", "USB Drive MSC initialization failed");
    if (!SDCard.begin()) {
      LOG_ERR("USB", "Unable to remount SD card after USB Drive startup failure");
    }
    return false;
  }
  return true;
#else
  return false;
#endif
}

bool HalStorage::disconnectUsbDriveHost() {
#if FREEINK_CAP_USB_MSC
  StorageLock lock;
  return usbMassStorage.disconnectHost();
#else
  return false;
#endif
}

void HalStorage::endUsbDrive() {
#if FREEINK_CAP_USB_MSC
  StorageLock lock;
  usbMassStorage.end();
#endif
}

UsbDriveState HalStorage::usbDriveState() const {
#if FREEINK_CAP_USB_MSC
  StorageLock lock;
  switch (usbMassStorage.state()) {
    case freeink::UsbMassStorageState::WaitingForHost:
      return UsbDriveState::WaitingForHost;
    case freeink::UsbMassStorageState::Connected:
    case freeink::UsbMassStorageState::Accessed:
      return UsbDriveState::Connected;
    case freeink::UsbMassStorageState::Ejected:
      return UsbDriveState::Ejected;
    case freeink::UsbMassStorageState::Disconnected:
      return UsbDriveState::Disconnected;
    case freeink::UsbMassStorageState::IoError:
      return UsbDriveState::IoError;
    case freeink::UsbMassStorageState::Idle:
      break;
  }
#endif
  return UsbDriveState::Unsupported;
}

#define HAL_STORAGE_WRAPPED_CALL(method, ...) \
  HalStorage::StorageLock lock;               \
  return SDCard.method(__VA_ARGS__);

std::vector<String> HalStorage::listFiles(const char* path, int maxFiles) {
  HAL_STORAGE_WRAPPED_CALL(listFiles, path, maxFiles);
}

String HalStorage::readFile(const char* path) { HAL_STORAGE_WRAPPED_CALL(readFile, path); }

bool HalStorage::readFileToStream(const char* path, Print& out, size_t chunkSize) {
  HAL_STORAGE_WRAPPED_CALL(readFileToStream, path, out, chunkSize);
}

size_t HalStorage::readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes) {
  HAL_STORAGE_WRAPPED_CALL(readFileToBuffer, path, buffer, bufferSize, maxBytes);
}

bool HalStorage::writeFile(const char* path, const String& content) {
  HAL_STORAGE_WRAPPED_CALL(writeFile, path, content);
}

bool HalStorage::ensureDirectoryExists(const char* path) { HAL_STORAGE_WRAPPED_CALL(ensureDirectoryExists, path); }

class HalFile::Impl {
 public:
  Impl(FsFile&& fsFile) : file(std::move(fsFile)) {}
  // SdFat is not thread-safe; FsFile::close() touches SD/SPI and must run
  // under StorageLock or it races SdSpiCard::m_spiActive across tasks and
  // trips FreeRTOS's xTaskPriorityDisinherit assert. The FsFile member
  // destructor (DESTRUCTOR_CLOSES_FILE=1) will close() again after the lock
  // releases, but close() on an already-closed FsFile is a no-op. See SdFat
  // issue #518 and the HAL note in CLAUDE.md.
  ~Impl() {
    HalStorage::StorageLock lock;
    file.close();
  }
  FsFile file;
  // Read-ahead state (see HalFile::setReadAhead). Caller-owned buffer.
  uint8_t* ra = nullptr;
  size_t raCap = 0;
  size_t raStart = 0;  // file offset of ra[0]
  size_t raFill = 0;   // valid bytes in ra
  size_t raPos = 0;    // logical read position while ra != nullptr
};

HalFile::HalFile() = default;
HalFile::HalFile(std::unique_ptr<Impl> impl) : impl(std::move(impl)) {}
HalFile::~HalFile() = default;
HalFile::HalFile(HalFile&&) = default;
HalFile& HalFile::operator=(HalFile&&) = default;

HalFile HalStorage::open(const char* path, const oflag_t oflag) {
  StorageLock lock;  // ensure thread safety for the duration of this function
  return HalFile(std::make_unique<HalFile::Impl>(SDCard.open(path, oflag)));
}

bool HalStorage::mkdir(const char* path, const bool pFlag) { HAL_STORAGE_WRAPPED_CALL(mkdir, path, pFlag); }

bool HalStorage::exists(const char* path) { HAL_STORAGE_WRAPPED_CALL(exists, path); }

bool HalStorage::remove(const char* path) { HAL_STORAGE_WRAPPED_CALL(remove, path); }
bool HalStorage::rename(const char* oldPath, const char* newPath) {
  HAL_STORAGE_WRAPPED_CALL(rename, oldPath, newPath);
}

bool HalStorage::rmdir(const char* path) { HAL_STORAGE_WRAPPED_CALL(rmdir, path); }

bool HalStorage::openFileForRead(const char* moduleName, const char* path, HalFile& file) {
  StorageLock lock;  // ensure thread safety for the duration of this function
  FsFile fsFile;
  bool ok = SDCard.openFileForRead(moduleName, path, fsFile);
  file = HalFile(std::make_unique<HalFile::Impl>(std::move(fsFile)));
  return ok;
}

bool HalStorage::openFileForRead(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForRead(const char* moduleName, const String& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const char* path, HalFile& file) {
  StorageLock lock;  // ensure thread safety for the duration of this function
  FsFile fsFile;
  bool ok = SDCard.openFileForWrite(moduleName, path, fsFile);
  file = HalFile(std::make_unique<HalFile::Impl>(std::move(fsFile)));
  return ok;
}

bool HalStorage::openFileForWrite(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const String& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::removeDir(const char* path) { HAL_STORAGE_WRAPPED_CALL(removeDir, path); }

// HalFile implementation
// Allow doing file operations while ensuring thread safety via HalStorage's mutex.
// Please keep the list below in sync with the HalFile.h header

#define HAL_FILE_WRAPPED_CALL(method, ...) \
  HalStorage::StorageLock lock;            \
  assert(impl != nullptr);                 \
  return impl->file.method(__VA_ARGS__);

#define HAL_FILE_FORWARD_CALL(method, ...) \
  assert(impl != nullptr);                 \
  return impl->file.method(__VA_ARGS__);

void HalFile::flush() { HAL_FILE_WRAPPED_CALL(flush, ); }
size_t HalFile::getName(char* name, size_t len) { HAL_FILE_WRAPPED_CALL(getName, name, len); }
size_t HalFile::size() { HAL_FILE_FORWARD_CALL(size, ); }              // already thread-safe, no need to wrap
size_t HalFile::fileSize() { HAL_FILE_FORWARD_CALL(fileSize, ); }      // already thread-safe, no need to wrap
uint64_t HalFile::fileSize64() { HAL_FILE_FORWARD_CALL(fileSize, ); }  // already thread-safe, no need to wrap
void HalFile::setReadAhead(uint8_t* buf, size_t cap) {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  if (impl->ra) {
    impl->file.seekSet(impl->raPos);  // hand the logical position back to SdFat
  }
  impl->ra = (buf != nullptr && cap > 0) ? buf : nullptr;
  impl->raCap = impl->ra ? cap : 0;
  impl->raStart = 0;
  impl->raFill = 0;
  impl->raPos = impl->file.position();
}

bool HalFile::seek(size_t pos) {
  assert(impl != nullptr);
  if (impl->ra) {
    if (pos > impl->file.fileSize()) return false;
    impl->raPos = pos;  // lazy: the buffer is (re)filled on the next read
    return true;
  }
  HAL_FILE_WRAPPED_CALL(seekSet, pos);
}
bool HalFile::seek64(uint64_t pos) { return seek(static_cast<size_t>(pos)); }
bool HalFile::seekCur(int64_t offset) {
  assert(impl != nullptr);
  if (impl->ra) return seek(static_cast<size_t>(static_cast<int64_t>(impl->raPos) + offset));
  HAL_FILE_WRAPPED_CALL(seekCur, offset);
}
bool HalFile::seekSet(size_t offset) { return seek(offset); }
int HalFile::available() const {
  assert(impl != nullptr);
  if (impl->ra) {
    const size_t sz = impl->file.fileSize();
    return impl->raPos < sz ? static_cast<int>(sz - impl->raPos) : 0;
  }
  HAL_FILE_WRAPPED_CALL(available, );
}
size_t HalFile::position() const {
  assert(impl != nullptr);
  if (impl->ra) return impl->raPos;
  HAL_FILE_WRAPPED_CALL(position, );
}
int HalFile::read(void* buf, size_t count) {
  assert(impl != nullptr);
  if (!impl->ra) {
    HAL_FILE_WRAPPED_CALL(read, buf, count);
  }
  HalStorage::StorageLock lock;
  auto* out = static_cast<uint8_t*>(buf);
  size_t done = 0;
  while (done < count) {
    if (impl->raFill > 0 && impl->raPos >= impl->raStart && impl->raPos < impl->raStart + impl->raFill) {
      const size_t off = impl->raPos - impl->raStart;
      const size_t n = std::min(count - done, impl->raFill - off);
      memcpy(out + done, impl->ra + off, n);
      done += n;
      impl->raPos += n;
      continue;
    }
    if (!impl->file.seekSet(impl->raPos)) break;
    if (count - done >= impl->raCap) {
      // Bulk read: straight through, no point staging it in the buffer.
      const int n = impl->file.read(out + done, count - done);
      if (n <= 0) break;
      done += static_cast<size_t>(n);
      impl->raPos += static_cast<size_t>(n);
      break;
    }
    const int n = impl->file.read(impl->ra, impl->raCap);
    if (n <= 0) {
      impl->raFill = 0;
      break;
    }
    impl->raStart = impl->raPos;
    impl->raFill = static_cast<size_t>(n);
  }
  return static_cast<int>(done);
}
int HalFile::read() {
  assert(impl != nullptr);
  if (impl->ra) {
    uint8_t b = 0;
    return read(&b, 1) == 1 ? b : -1;
  }
  HAL_FILE_WRAPPED_CALL(read, );
}
size_t HalFile::write(const uint8_t* buf, size_t count) {
  assert(impl != nullptr);
  if (impl->ra) {
    HalStorage::StorageLock lock;
    impl->file.seekSet(impl->raPos);
    const size_t n = impl->file.write(buf, count);
    impl->raPos += n;
    impl->raFill = 0;  // stale after a write
    return n;
  }
  HAL_FILE_WRAPPED_CALL(write, buf, count);
}
size_t HalFile::write(const void* buf, size_t count) { return write(static_cast<const uint8_t*>(buf), count); }
size_t HalFile::write(uint8_t b) { return write(&b, 1); }
bool HalFile::rename(const char* newPath) { HAL_FILE_WRAPPED_CALL(rename, newPath); }
bool HalFile::isDirectory() const { HAL_FILE_FORWARD_CALL(isDirectory, ); }  // already thread-safe, no need to wrap
void HalFile::rewindDirectory() { HAL_FILE_WRAPPED_CALL(rewindDirectory, ); }
bool HalFile::close() {
  assert(impl != nullptr);
  impl->ra = nullptr;
  impl->raCap = 0;
  impl->raFill = 0;
  HAL_FILE_WRAPPED_CALL(close, );
}
HalFile HalFile::openNextFile() {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  return HalFile(std::make_unique<Impl>(impl->file.openNextFile()));
}
bool HalFile::isOpen() const { return impl != nullptr && impl->file.isOpen(); }  // already thread-safe, no need to wrap
HalFile::operator bool() const { return isOpen(); }
