#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "net.hpp"
#include "stremio.hpp"

struct SDL_Window;
struct SDL_Renderer;

class Screen {
public:
    virtual ~Screen() = default;
    virtual void update(float dt) = 0;
    virtual void render() = 0;
    virtual void onResume() {}             // became the top screen again
    virtual void onPresented() {}          // after SDL_RenderPresent / vblank (player reports swap to mpv)
    virtual bool opaque() const { return true; }

protected:
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

    // Run `work` on a worker thread, then `done(result)` on the main thread,
    // but only if this screen still exists.
    template <class Work, class Done>
    void async(Work work, Done done) {
        using T = decltype(work());
        std::weak_ptr<bool> w = alive_;
        net::runAsync([w, work, done]() mutable {
            if (w.expired()) return;
            auto res = std::make_shared<T>(work());
            net::postMain([w, res, done]() mutable {
                if (!w.expired()) done(*res);
            });
        });
    }
};

class App {
public:
    static App& get();

    bool init();
    void run();
    void shutdown();

    void push(std::shared_ptr<Screen> s);
    void pop();
    void replaceTop(std::shared_ptr<Screen> s);
    void toast(const std::string& msg);
    void requestQuit() { quit_ = true; }

    // Installed addons, in priority order (parallel to store::addonEntries()).
    std::vector<stremio::Addon>& addons() { return addons_; }
    std::vector<stremio::Addon> enabledAddons() const;
    int addonsRevision() const { return rev_; }
    int manifestsLoading() const { return loading_; }
    void installAddon(const std::string& url);
    void removeAddon(size_t i);
    void toggleAddon(size_t i);
    void moveAddon(size_t i, int dir);

    std::string lanUrl() const;
    int memoryBudgetMB() const { return memoryMB_; }
    // Battery charge 0-100 (-1 if unavailable) and whether a charger is connected.
    int batteryPercent(bool* charging = nullptr) const;

    // Nuvio account sync
    void syncAccount(bool userInitiated);
    bool syncing() const { return syncing_; }
    const std::string& syncStatus() const { return syncStatus_; }
    struct ProfileInfo {
        int index;
        std::string name;
    };
    const std::vector<ProfileInfo>& profiles() const { return profiles_; }
    // Look up titles/artwork for Continue Watching entries imported without them.
    void enrichProgress();
    // Trakt
    void syncTrakt(bool userInitiated);
    bool traktSyncing() const { return traktSyncing_; }
    const std::string& traktStatus() const { return traktStatus_; }
    // Fire-and-forget pushes (run on a worker; refreshed tokens are saved afterwards).
    void traktScrobble(const std::string& action, const std::string& imdb, const std::string& type, int season,
                       int episode, double pct);
    void traktSetWatched(const std::string& imdb, const std::string& type, int season, int episode, bool watched);

    // Import a nuvio-import.json produced by tools/import-from-windows.ps1.
    bool applyImport(const std::string& body, std::string& summary);

private:
    bool syncing_ = false;
    std::string syncStatus_;
    bool traktSyncing_ = false;
    std::string traktStatus_;
    std::vector<ProfileInfo> profiles_;
    std::vector<std::string> enriched_;
    App() = default;
    void loadAddons();
    void refreshManifest(const std::string& manifestUrl);
    void syncEntries();
    void drawToasts();
    void drawPerfOverlay(float dt);

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    bool quit_ = false;
    bool socketsUp_ = false, romfsUp_ = false, plUp_ = false, sslUp_ = false, psmUp_ = false;
    mutable int battPct_ = -1;
    mutable bool battCharging_ = false;
    mutable double battChecked_ = -100;
    int memoryMB_ = 400;
    std::vector<std::shared_ptr<Screen>> stack_;
    std::vector<std::function<void()>> ops_;
    std::vector<stremio::Addon> addons_;
    int rev_ = 0;
    int loading_ = 0;
    struct Toast {
        std::string msg;
        double until;
    };
    std::vector<Toast> toasts_;
};
