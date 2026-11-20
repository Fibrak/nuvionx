#include "sdfile.hpp"

#include <switch.h>

#include <cstring>
#include <mutex>

namespace sdfile {

static std::mutex g_mx;  // serialise our own writes (tmp names are shared per path)

static FsFileSystem* sd() { return fsdevGetDeviceFileSystem("sdmc"); }

static std::string fsPath(const std::string& p) {
    std::string s = p;
    if (s.compare(0, 5, "sdmc:") == 0) s = s.substr(5);
    if (s.empty() || s[0] != '/') s = "/" + s;
    return s;
}

bool read(const std::string& path, std::string& out) {
    FsFileSystem* fs = sd();
    if (!fs) return false;
    std::string p = fsPath(path);
    char buf[FS_MAX_PATH] = {0};
    strncpy(buf, p.c_str(), sizeof buf - 1);
    FsFile f;
    if (R_FAILED(fsFsOpenFile(fs, buf, FsOpenMode_Read, &f))) return false;
    s64 size = 0;
    bool ok = R_SUCCEEDED(fsFileGetSize(&f, &size)) && size >= 0 && size < 64 * 1024 * 1024;
    if (ok) {
        out.resize((size_t)size);
        u64 got = 0;
        ok = size == 0 || (R_SUCCEEDED(fsFileRead(&f, 0, &out[0], (u64)size, FsReadOption_None, &got)) &&
                           got == (u64)size);
    }
    fsFileClose(&f);
    if (!ok) out.clear();
    return ok;
}

bool exists(const std::string& path) {
    FsFileSystem* fs = sd();
    if (!fs) return false;
    char buf[FS_MAX_PATH] = {0};
    strncpy(buf, fsPath(path).c_str(), sizeof buf - 1);
    FsDirEntryType type;
    return R_SUCCEEDED(fsFsGetEntryType(fs, buf, &type));
}

bool remove(const std::string& path) {
    FsFileSystem* fs = sd();
    if (!fs) return false;
    char buf[FS_MAX_PATH] = {0};
    strncpy(buf, fsPath(path).c_str(), sizeof buf - 1);
    return R_SUCCEEDED(fsFsDeleteFile(fs, buf));
}

bool mkdirs(const std::string& dirPath) {
    FsFileSystem* fs = sd();
    if (!fs) return false;
    std::string p = fsPath(dirPath);
    for (size_t i = 1; i <= p.size(); i++) {
        if (i == p.size() || p[i] == '/') {
            char buf[FS_MAX_PATH] = {0};
            strncpy(buf, p.substr(0, i).c_str(), sizeof buf - 1);
            fsFsCreateDirectory(fs, buf);  // "already exists" is fine
        }
    }
    return true;
}

bool write(const std::string& path, const std::string& data) {
    FsFileSystem* fs = sd();
    if (!fs) return false;
    std::lock_guard<std::mutex> lk(g_mx);
    {
        std::string cur;
        if (read(path, cur) && cur == data) return true;  // unchanged: no SD write at all
    }
    std::string p = fsPath(path), t = p + ".tmp";
    char target[FS_MAX_PATH] = {0}, tmp[FS_MAX_PATH] = {0};
    strncpy(target, p.c_str(), sizeof target - 1);
    strncpy(tmp, t.c_str(), sizeof tmp - 1);

    fsFsDeleteFile(fs, tmp);
    if (R_FAILED(fsFsCreateFile(fs, tmp, (s64)data.size(), 0))) return false;
    FsFile f;
    if (R_FAILED(fsFsOpenFile(fs, tmp, FsOpenMode_Write, &f))) {
        fsFsDeleteFile(fs, tmp);
        return false;
    }
    bool ok = data.empty() || R_SUCCEEDED(fsFileWrite(&f, 0, data.data(), data.size(), FsWriteOption_Flush));
    fsFileClose(&f);
    if (!ok) {
        fsFsDeleteFile(fs, tmp);
        return false;
    }
    fsFsDeleteFile(fs, target);
    ok = R_SUCCEEDED(fsFsRenameFile(fs, tmp, target));
    fsFsCommit(fs);
    return ok;
}

}  // namespace sdfile
