#pragma once
#include <switch.h>
#include <atomic>
#include <string>
#include <vector>

// News for the Deck layout's card row.
//
// Two sources, both keyless:
//
//   Nintendo  the front page of nintendo.com/us/whatsnew. There is no public
//             feed - the page is a Next.js app - so this downloads the page and
//             reads the article records out of the Apollo cache embedded in it.
//             Scraping, so it is written to survive the page changing shape:
//             anything it cannot make sense of is skipped, never guessed at.
//
//   Steam     ISteamNews for the selected game, found by name through the same
//             keyless appid search the cover fetch already uses. A Switch title
//             is not a Steam app, so this is the PC edition's news - close
//             enough to be worth reading, and labelled by source in the card.
//
// One worker at a time, polled from the render loop like every other fetch in
// the menu. Results are cached on the card so a cold boot draws real cards on
// the first frame and the network is only touched when the cache has aged out.

namespace sl::menu::news {

    struct Item {
        std::string kind;      // card label: NEWS / UPDATE / SALE / the Steam feed name
        std::string title;
        std::string summary;   // plain text, tags and BBCode already stripped
        std::string date;      // formatted for display ("27 Aug")
        std::string img;       // sdmc path of the cached art, empty when none
        std::string link;      // where the story lives, shown in the reader
    };

    enum class Source { Nintendo, Steam, Count };

    class Feed {
        public:
            ~Feed();

            // Read the cached lists off the card. Cheap enough for the boot
            // path: two small text files, no network, no images decoded.
            void Init();

            // Ask for a feed. Ignored while a fetch is running, while the cache
            // is still fresh, and - for Steam - when this title has already been
            // looked up this session (including when the lookup found nothing).
            void Request(Source s, u64 app_id = 0, const char *name = nullptr);

            // Reap a finished worker. True on the frame a fetch lands, which is
            // the cue to drop any textures made from the previous list.
            bool Poll();

            bool Busy() const { return m_running; }

            const std::vector<Item> &Get(Source s) const {
                return (s == Source::Steam) ? m_steam : m_nintendo;
            }

            // The title the Steam list belongs to; 0 before the first lookup.
            u64  SteamAppId() const { return m_steam_of; }

            // A fetch ran and came back with nothing. Distinguishes "still
            // loading" from "there is nothing here", which the row needs so it
            // can say so instead of spinning forever.
            bool Tried(Source s) const {
                return (s == Source::Steam) ? m_steam_tried : m_nin_tried;
            }

        private:
            static void Trampoline(void *self);
            void Work();
            void FetchNintendo();
            void FetchSteam();

            std::vector<Item> m_nintendo;
            std::vector<Item> m_steam;
            std::vector<Item> m_pending;       // filled by the worker, swapped in by Poll

            Thread            m_thread {};
            std::atomic<bool> m_done { false };
            bool              m_running = false;

            Source      m_job      = Source::Nintendo;
            u64         m_job_id   = 0;        // title id, for the Steam cache file
            std::string m_job_name;            // its name, for the appid search

            u64  m_steam_of    = 0;
            bool m_nin_tried   = false;
            bool m_steam_tried = false;

            // Titles already looked up on Steam, so a game with no PC edition
            // costs one search rather than one per selection.
            std::vector<u64> m_steam_asked;
    };

} // namespace sl::menu::news
