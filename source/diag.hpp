// In-memory diagnostics log (thread-safe ring buffer). Nothing touches the SD
// card unless writeFile() is called, e.g. when playback fails.
#pragma once
#include <string>

namespace diag {

void log(const std::string& line);
void clear();
// Writes the buffered lines to `path` (one SD write).
void writeFile(const char* path = "sdmc:/switch/NuvioNX/mpv.log");

}  // namespace diag
