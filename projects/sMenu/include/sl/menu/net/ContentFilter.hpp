#pragma once
#include <string>
#include <vector>

// Adult content filtering for news and art fetching.
//
// Blocks content based on:
//   - Game ratings (ESRB M/AO, PEGI 16/18, USK 16/18, etc.)
//   - Content tags from Steam, SteamGridDB, and news sources
//   - Metadata in news articles and cover art descriptions
//
// Filtering can be enabled/disabled via config file:
//   sdmc:/slaunch/config/content_filter.txt -> "enabled=1" (or 0)

namespace sl::menu::net {

    class ContentFilter {
        public:
            static bool IsEnabled();
            static void SetEnabled(bool enabled);
            static bool ShouldFilterGameByName(const std::string &title);
            static bool ShouldFilterByEsrb(const std::string &esrb_rating);
            static bool ShouldFilterByPegi(const std::string &pegi_rating);
            static bool ShouldFilterByUsk(const std::string &usk_rating);
            static bool ShouldFilterByTags(const std::vector<std::string> &tags);
            static std::vector<std::string> ParseTags(const std::string &tags_str);
            static bool ShouldFilterBySteamTags(const std::string &json_body);
            static bool ShouldFilterNewsArticle(const std::string &title,
                                                 const std::string &summary,
                                                 const std::string &category);
            static bool ShouldFilterSteamNews(const std::string &title,
                                              const std::string &content);
            static bool ShouldFilterArtMetadata(const std::string &json_body,
                                                 const std::string &source);
    };

} // namespace sl::menu::net
