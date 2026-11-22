#pragma once
#include <memory>
#include <string>

#include "app.hpp"
#include "stremio.hpp"
#include "ui.hpp"

// Everything the stream picker / player need to know about what's playing.
struct PlayContext {
    stremio::Meta meta;  // may be a preview only (videos empty) when launched from Continue Watching
    std::string type;
    std::string videoId;
    int season = 0, episode = 0;
    std::string epTitle;
    // autoplay (next episode): pick the stream whose bingeGroup matches
    bool autoPlay = false;
    std::string preferBinge;
};

// A tab inside the main screen.
class Page : public Screen {
public:
    virtual std::vector<ui::Hint> hints() = 0;
    virtual void onShow() {}
    // Advance scroll/focus animations without reading input. Runs instead of update() while
    // focus is on the tab bar, so a scroll in progress still finishes.
    virtual void animate(float dt) { (void)dt; }
    // Pages set this when the user presses Up past their top edge.
    bool wantsTabBar = false;
};

std::shared_ptr<Screen> makeMainScreen();
std::shared_ptr<Page> makeHomePage();
std::shared_ptr<Page> makeSearchPage();
std::shared_ptr<Page> makeLibraryPage();
std::shared_ptr<Page> makeAddonsPage();
std::shared_ptr<Page> makeAccountPage();
std::shared_ptr<Page> makeSettingsPage();

std::shared_ptr<Screen> makeDetailScreen(const stremio::MetaPreview& m);
std::shared_ptr<Screen> makeStreamsScreen(const PlayContext& ctx);
std::shared_ptr<Screen> makePlayerScreen(const PlayContext& ctx, const stremio::Stream& s);

// Helpers shared by screens
std::string episodeLabel(int season, int episode);
// Next video to play for a series (based on watch history), or nullptr.
const stremio::Video* nextUpVideo(const stremio::Meta& m, double* resumeAt);
const stremio::Video* videoAfter(const stremio::Meta& m, const std::string& videoId);
