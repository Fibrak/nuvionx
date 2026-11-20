// Local HTTP -> HTTPS relay for the player.
//
// devkitPro's FFmpeg is built without the "https" protocol, so mpv can't open
// https:// URLs directly. The player instead opens http://127.0.0.1:<port>/s/<token>
// and this relay fetches the real URL with libcurl (which uses the Switch's system
// TLS), forwarding Range requests so seeking works.
#pragma once
#include <string>
#include <vector>

namespace proxy {

bool start();
void stop();  // waits (up to 5 s) for in-flight transfers to finish
int activeTransfers();
// Returns a local http:// URL for https:// URLs (or the URL unchanged otherwise).
std::string wrap(const std::string& url, const std::vector<std::string>& headers = {});

}  // namespace proxy
