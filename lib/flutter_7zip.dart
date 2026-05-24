import 'dart:ffi';
import 'dart:io';
import 'dart:isolate';
import 'dart:typed_data';
import 'package:ffi/ffi.dart';

import 'src/flutter_7zip_bindings_generated.dart';

const String _libName = 'flutter_7zip';

/// The dynamic library in which the symbols for [Flutter7zipBindings] can be found.
final DynamicLibrary _dylib = () {
  if (Platform.isMacOS || Platform.isIOS) {
    return DynamicLibrary.open('$_libName.framework/$_libName');
  }
  if (Platform.isAndroid || Platform.isLinux) {
    return DynamicLibrary.open('lib$_libName.so');
  }
  if (Platform.isWindows) {
    return DynamicLibrary.open('$_libName.dll');
  }
  throw UnsupportedError('Unknown platform: ${Platform.operatingSystem}');
}();

/// The bindings to the native functions in [_dylib].
final Flutter7zipBindings _bindings = Flutter7zipBindings(_dylib);

final _nativeFreeDataFunc =
    _dylib.lookup<NativeFunction<Void Function(Pointer<Void>)>>('freeReadData');

/// Timing statistics for a single extraction operation.
class FileExtractStat {
  /// Index of the file in the archive.
  final int fileIndex;

  /// Whether this entry was a directory.
  final bool isDirectory;

  /// Elapsed milliseconds for this single file extraction.
  final int elapsedMs;

  const FileExtractStat(this.fileIndex, this.isDirectory, this.elapsedMs);
}

/// Wrapper around [SZArchive.extract] return value.
class ArchiveExtractResult {
  /// Total elapsed milliseconds for the entire extraction.
  final int totalElapsedMs;

  /// Number of files extracted.
  final int totalFiles;

  const ArchiveExtractResult(this.totalElapsedMs, this.totalFiles);

  @override
  String toString() => '${totalFiles}files/${totalElapsedMs}ms';
}

class SZArchive {
  final Pointer<Void> _archive;
  final _pointers = <Pointer>[];
  final Stopwatch _stopwatch = Stopwatch();
  final List<FileExtractStat> _fileStats = [];

  SZArchive._(this._archive);

  /// Dispose the archive and free all resources.
  void dispose() {
    _bindings.closeArchive(_archive);
    for (var p in _pointers) {
      malloc.free(p);
    }
  }

  /// Total elapsed milliseconds spent extracting files from this archive.
  int get elapsedMs => _stopwatch.elapsedMilliseconds;

  /// Per-file extraction statistics collected since opening.
  List<FileExtractStat> get fileStats => List.unmodifiable(_fileStats);

  /// Reset timing counters.
  void resetTiming() {
    _stopwatch.reset();
    _fileStats.clear();
  }

  /// Get the number of files in the archive.
  int get numFiles => _bindings.getArchiveFileCount(_archive);

  DateTime _parseCTime(int timestamp) {
    try {
      const int secondsThreshold = 1000000000;
      const int millisecondsThreshold = 1000000000000;
      const int windowsEpochDiff = 116444736000000000;

      if (timestamp < secondsThreshold) {
        throw ArgumentError('Invalid timestamp: $timestamp');
      } else if (timestamp < millisecondsThreshold) {
        return DateTime.fromMillisecondsSinceEpoch(timestamp * 1000);
      } else if (timestamp >= millisecondsThreshold &&
          timestamp < windowsEpochDiff) {
        return DateTime.fromMillisecondsSinceEpoch(timestamp);
      } else {
        final unixMilliseconds = (timestamp - windowsEpochDiff) ~/ 10000;
        return DateTime.fromMillisecondsSinceEpoch(unixMilliseconds);
      }
    } catch (e) {
      return DateTime(0);
    }
  }

  /// Get the file at the given [index].
  ArchiveFile getFile(int index) {
    final cFile = _bindings.getArchiveFile(_archive, index);
    final name = cFile.name.cast<Utf16>().toDartString();
    final size = cFile.size;
    final crc32 = cFile.crc32;
    final isDirectory = cFile.is_dir == 1;
    DateTime? createTime;
    DateTime? modifyTime;
    if (cFile.cTime != 0) {
      createTime = _parseCTime(cFile.cTime);
    }
    if (cFile.mTime != 0) {
      modifyTime = _parseCTime(cFile.mTime);
    }
    _bindings.freeArchiveFile(cFile);
    return ArchiveFile(
      name,
      size,
      crc32,
      createTime,
      modifyTime,
      isDirectory,
    );
  }

  /// Extract the file at the given [index] to a [Uint8List].
  Uint8List extractFile(int index) {
    var archive = getFile(index);
    var data = _bindings.readArchiveFile(_archive, index).cast<Uint8>();
    if (data == nullptr) {
      throw Exception('Failed to read file from archive.');
    }
    return data.asTypedList(archive.size, finalizer: _nativeFreeDataFunc);
  }

  /// Extract the file at the given [index] to a file at the given [path].
  void extractToFile(int index, String path) {
    var p = path.toNativeUtf8();
    if (!File(path).parent.existsSync()) {
      File(path).parent.createSync(recursive: true);
    }
    final status = _bindings.extractArchiveToFile(_archive, index, p.cast());
    malloc.free(p);
    if (status != ArchiveStatus.kArchiveOK) {
      throw Exception('Failed to extract file to $path.');
    }
  }

  /// Extract file by [index] to [outputDir], creating subdirs as needed.
  /// Returns true if entry was a directory (already created).
  /// Throws on error.
  bool extractToDir(int index, String outputDir) {
    _stopwatch.start();
    final before = _stopwatch.elapsedMilliseconds;
    var p = outputDir.toNativeUtf8();
    final result = _bindings.extractFileToDir(_archive, index, p.cast());
    malloc.free(p);
    if (result < 0 || result > 1) {
      _stopwatch.stop();
      throw Exception('Failed to extract file index $index (code=$result)');
    }
    final after = _stopwatch.elapsedMilliseconds;
    _fileStats.add(FileExtractStat(index, result == 1, after - before));
    return result == 1;
  }

  /// Open an archive at the given [path].
  static SZArchive open(String path) {
    var p = path.toNativeUtf8();
    final archive = _bindings.openArchive(p.cast());
    var status = _bindings.checkArchiveStatus(archive);
    if (status != ArchiveStatus.kArchiveOK) {
      malloc.free(p);
      throw Exception('Failed to open archive.');
    }
    var a = SZArchive._(archive);
    a._pointers.add(p);
    return a;
  }

  /// Extract the archive at the given [archivePath] to the given [outputPath].
  ///
  /// Returns [ArchiveExtractResult] with total timing and file count.
  static ArchiveExtractResult extract(String archivePath, String outputPath) {
    final sw = Stopwatch()..start();
    var archive = open(archivePath);
    var total = archive.numFiles;
    for (var i = 0; i < total; i++) {
      archive.extractToDir(i, outputPath);
    }
    archive.dispose();
    sw.stop();
    return ArchiveExtractResult(sw.elapsedMilliseconds, total);
  }

  /// Extract the archive at the given [archivePath] to the given [outputPath] with [isolatesCount] isolates.
  ///
  /// The method is asynchronous and will not block the main thread.
  static Future<ArchiveExtractResult> extractIsolates(
    String archivePath,
    String outputPath,
    int isolatesCount,
  ) async {
    final sw = Stopwatch()..start();
    var archive = open(archivePath);
    var total = archive.numFiles;
    var filesPerIsolate = total ~/ isolatesCount;
    var futures = <Future>[];
    for (var i = 0; i < isolatesCount; i++) {
      var start = i * filesPerIsolate;
      var end = i == isolatesCount - 1 ? total : (i + 1) * filesPerIsolate;
      futures.add(SZArchive._extractIsolate(
        archivePath,
        outputPath,
        start,
        end,
      ));
    }
    await Future.wait(futures);
    sw.stop();
    return ArchiveExtractResult(sw.elapsedMilliseconds, total);
  }

  static Future<void> _extractIsolate(
    String archivePath,
    String outputPath,
    int start,
    int end,
  ) async {
    return Isolate.run(() {
      var archive = open(archivePath);
      for (var i = start; i < end; i++) {
        archive.extractToDir(i, outputPath);
      }
      archive.dispose();
    });
  }
}

class ArchiveFile {
  /// Relative path to the archive.
  final String name;
  /// The size of the file.
  final int size;
  /// The CRC32 checksum of the file.
  final int crc32;
  /// The creation time of the file.
  final DateTime? createTime;
  /// The modification time of the file.
  final DateTime? modifyTime;
  /// Whether the file is a directory.
  final bool isDirectory;

  /// A file in an archive.
  const ArchiveFile(
    this.name,
    this.size,
    this.crc32,
    this.createTime,
    this.modifyTime,
    this.isDirectory,
  );
}
