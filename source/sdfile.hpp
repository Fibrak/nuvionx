// SD card file I/O through the Switch's native FS API (fsFs* / fsFile*),
// bypassing newlib's file-descriptor table.
//
// Why: sockets and stdio FILEs share newlib's descriptor table. A crash report
// showed our log write dying inside fclose() while TLS connections were being torn
// down on other threads, i.e. the FILE's descriptor slot was invalidated under us.
// Native FS handles live outside that table, so network code can't disturb them.
#pragma once
#include <string>

namespace sdfile {

// Paths may be given as "sdmc:/switch/..." or "/switch/...".
bool read(const std::string& path, std::string& out);
// Atomic write (tmp + rename), flushed. Skips the write if the file already holds `data`.
bool write(const std::string& path, const std::string& data);
bool exists(const std::string& path);
bool remove(const std::string& path);
bool mkdirs(const std::string& dirPath);

}  // namespace sdfile
