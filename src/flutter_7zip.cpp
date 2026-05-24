#include "flutter_7zip.h"
#include "7zip/C/7zCrc.h"
#include "7zip/C/7z.h"
#include "7zip/C/7zFile.h"
#include <iostream>
#include <fstream>
#include <sys/stat.h>
#include <string>
#include <vector>

void *_AllocImp(ISzAllocPtr p, size_t size) {
  return malloc(size);
}

void _FreeImp(ISzAllocPtr p, void *ptr) {
  if (ptr != nullptr) {
    free(ptr);
  }
}

static ISzAlloc AllocImp = { _AllocImp, _FreeImp };

class Archive {
  CFileInStream archiveStream;
  CLookToRead2 lookStream;
  CSzArEx db;
  ISzAlloc allocImp = AllocImp;
  ISzAlloc allocTempImp = AllocImp;
  uint32_t lastBlockIndex = 0xFFFFFFFF;
  Byte* cachedBuffer = nullptr;
  size_t cachedBufferSize = 0;

public:
  ArchiveStatus status = kArchiveOK;

  explicit Archive(const char* path) {
    if (InFile_Open(&archiveStream.file, path)) {
      status = kArchiveOpenError;
      return;
    }
    CrcGenerateTable();
    FileInStream_CreateVTable(&archiveStream);
    lookStream.realStream = &archiveStream.vt;
    lookStream.buf = new Byte[16 * 1024];
    lookStream.bufSize = 16 * 1024;
    LookToRead2_CreateVTable(&lookStream, False);
    SzArEx_Init(&db);
    const SRes openRes = SzArEx_Open(&db, &lookStream.vt, &allocImp, &allocTempImp);
    if (openRes != SZ_OK) {
      status = kArchiveOpenError;
      File_Close(&archiveStream.file);
    }
  }

  ~Archive() {
    SzArEx_Free(&db, &allocImp);
    File_Close(&archiveStream.file);
    delete [] lookStream.buf;
    _FreeImp(&allocImp, cachedBuffer);
  }

  uint32_t numFiles() const {
    return db.NumFiles;
  }

  ArchiveFile getFileByIndex(const uint32_t index) const {
    ArchiveFile archiveFile;

    archiveFile.is_dir = SzArEx_IsDir(&db, index) ? 1 : 0;

    const size_t offs = db.FileNameOffsets[index];
    const size_t len = db.FileNameOffsets[index + 1] - offs;
    auto fileName = new uint16_t[len+1];
    for (auto i = 0; i < len; i++) {
      fileName[i] = db.FileNames[offs*2 + i*2] + (db.FileNames[offs*2 + i*2 + 1] << 8);
    }
    fileName[len] = 0;
    archiveFile.name = fileName;

    archiveFile.size = SzArEx_GetFileSize(&db, index);

    if (db.CRCs.Defs[index]) {
      archiveFile.crc32 = db.CRCs.Vals[index];
    } else {
      archiveFile.crc32 = 0;
    }

    archiveFile.cTime = 0;
    if (db.CTime.Defs != nullptr && db.CTime.Defs[index]) {
      const UInt64 Low = db.CTime.Vals[index].Low;
      const UInt64 High = db.CTime.Vals[index].High;
      archiveFile.cTime = Low + (High << 32);
    }

    archiveFile.mTime = 0;
    if (db.MTime.Defs != nullptr && db.MTime.Defs[index]) {
      const UInt64 Low = db.MTime.Vals[index].Low;
      const UInt64 High = db.MTime.Vals[index].High;
      archiveFile.mTime = Low + (High << 32);
    }

    return archiveFile;
  }

  unsigned char* readFile(const uint32_t index) const {
    const ArchiveFile archiveFile = getFileByIndex(index);
    if (archiveFile.is_dir) {
      return nullptr;
    }
    auto* buffer = new unsigned char[archiveFile.size];
    size_t read = 0;
    uint32_t blockIndex;
    Byte* outBuffer = nullptr;
    size_t outBufferSize;
    size_t offset;
    size_t outSizeProcessed;
    while (read < archiveFile.size) {
      const SRes res = SzArEx_Extract(&db, &lookStream.vt, index, &blockIndex, &outBuffer, &outBufferSize, &offset, &outSizeProcessed, &allocImp, &allocTempImp);
      if (res != SZ_OK) {
        delete[] buffer;
        return nullptr;
      }
      for (auto i = offset; i < outSizeProcessed + offset; i++) {
        buffer[read] = outBuffer[i];
        read++;
      }
      _FreeImp(&allocImp, outBuffer);
    }
    return buffer;
  }

  ArchiveStatus extractFileToPath(const uint32_t index, const char* path) const {
    const ArchiveFile archiveFile = getFileByIndex(index);
    if (archiveFile.is_dir) {
      return kArchiveReadError;
    }
    size_t read = 0;
    uint32_t blockIndex;
    Byte* outBuffer = nullptr;
    size_t outBufferSize;
    size_t offset;
    size_t outSizeProcessed;
    std::ofstream outFile;
    outFile.open(path, std::ios::binary | std::ios::out);
    if (!outFile.is_open()) {
      return kArchiveOpenError;
    }
    if (!outFile.good()) {
      return kArchiveOpenError;
    }
    while (read < archiveFile.size) {
      const SRes res = SzArEx_Extract(&db, &lookStream.vt, index, &blockIndex, &outBuffer, &outBufferSize, &offset, &outSizeProcessed, &allocImp, &allocTempImp);
      if (res != SZ_OK) {
        outFile.close();
        return ArchiveStatus::kArchiveReadError;
      }
      outFile.write(reinterpret_cast<const char *>(outBuffer+offset), outSizeProcessed);
      read += outSizeProcessed;
      _FreeImp(&allocImp, outBuffer);
    }
    outFile.close();
    return kArchiveOK;
  }

  int extractToDir(const uint32_t index, const char* outputDir) {
    const ArchiveFile file = getFileByIndex(index);

    std::string outPath = std::string(outputDir) + "/";
    const uint16_t* src = file.name;
    while (*src) {
      if (*src < 0x80) {
        outPath += static_cast<char>(*src);
      } else if (*src < 0x800) {
        outPath += static_cast<char>(0xC0 | (*src >> 6));
        outPath += static_cast<char>(0x80 | (*src & 0x3F));
      } else {
        outPath += static_cast<char>(0xE0 | (*src >> 12));
        outPath += static_cast<char>(0x80 | ((*src >> 6) & 0x3F));
        outPath += static_cast<char>(0x80 | (*src & 0x3F));
      }
      src++;
    }
    delete[] file.name;

    if (file.is_dir) {
      size_t pos = 0;
      while ((pos = outPath.find_first_of('/', pos)) != std::string::npos) {
        std::string dir = outPath.substr(0, pos);
        if (!dir.empty()) mkdir(dir.c_str(), 0755);
        pos++;
      }
      mkdir(outPath.c_str(), 0755);
      return 1;
    }

    size_t pos = 0;
    while ((pos = outPath.find_first_of('/', pos)) != std::string::npos) {
      std::string dir = outPath.substr(0, pos);
      mkdir(dir.c_str(), 0755);
      pos++;
    }

    size_t read = 0;
    size_t offset;
    size_t outSizeProcessed;
    std::ofstream outFile;
    outFile.open(outPath, std::ios::binary | std::ios::out);
    if (!outFile.is_open()) return 2;

    while (read < file.size) {
      const SRes res = SzArEx_Extract(&db, &lookStream.vt, index, &lastBlockIndex, &cachedBuffer, &cachedBufferSize, &offset, &outSizeProcessed, &allocImp, &allocTempImp);
      if (res != SZ_OK) {
        outFile.close();
        return 3;
      }
      outFile.write(reinterpret_cast<const char *>(cachedBuffer+offset), outSizeProcessed);
      read += outSizeProcessed;
    }
    outFile.close();
    return 0;
  }
};

FFI_PLUGIN_EXPORT void freeArchiveFile(const ArchiveFile archive) {
  delete[] archive.name;
}

FFI_PLUGIN_EXPORT void* openArchive(const char* path) {
  return new Archive{path};
}

FFI_PLUGIN_EXPORT ArchiveStatus checkArchiveStatus(void* archive) {
  const auto a = static_cast<Archive *>(archive);
  return a->status;
}

FFI_PLUGIN_EXPORT void closeArchive(void* archive) {
  delete static_cast<Archive *>(archive);
}

FFI_PLUGIN_EXPORT uint32_t getArchiveFileCount(void* archive) {
  const auto a = static_cast<Archive *>(archive);
  return a->numFiles();
}

FFI_PLUGIN_EXPORT ArchiveFile getArchiveFile(void* archive, uint32_t index) {
  const auto a = static_cast<Archive *>(archive);
  return a->getFileByIndex(index);
}

FFI_PLUGIN_EXPORT unsigned char* readArchiveFile(void* archive, uint32_t index) {
  const auto a = static_cast<Archive *>(archive);
  return a->readFile(index);
}
FFI_PLUGIN_EXPORT void freeReadData(void* p) {
  delete[] static_cast<unsigned char *>(p);
}

FFI_PLUGIN_EXPORT ArchiveStatus extractArchiveToFile(void* archive, uint32_t index, const char* path) {
  const auto a = static_cast<Archive *>(archive);
  return a->extractFileToPath(index, path);
}

FFI_PLUGIN_EXPORT int extractFileToDir(void* archive, uint32_t index, const char* outputDir) {
  const auto a = static_cast<Archive *>(archive);
  return a->extractToDir(index, outputDir);
}