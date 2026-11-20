// Tiny LAN web server so addon URLs can be pasted from a phone/PC browser
// instead of typed on the Switch keyboard.
#pragma once
#include <string>
#include <vector>

namespace lan {

bool start(int port = 8080);
void stop();
bool running();
int port();
// URLs submitted through the web form since the last call (main thread).
std::vector<std::string> takeSubmitted();
// Account server details submitted via /account (main thread). Returns false if none pending.
bool takeServerConfig(std::string& url, std::string& key);
// Trakt API app credentials submitted via /account (main thread).
bool takeTraktConfig(std::string& clientId, std::string& clientSecret);
// JSON body posted to /import by tools/import-from-windows.ps1 (main thread).
bool takeImport(std::string& body);

}  // namespace lan
