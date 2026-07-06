#include <sl/menu/net/Http.hpp>
#include <curl/curl.h>

namespace sl::menu::net {

    namespace {
        bool g_inited = false;

        size_t WriteCb(char *ptr, size_t size, size_t nmemb, void *userdata) {
            auto *out = static_cast<std::string *>(userdata);
            const size_t n = size * nmemb;
            // Cap the buffer so a runaway response can't exhaust the applet heap.
            if (out->size() + n > 256 * 1024) return 0;
            out->append(ptr, n);
            return n;
        }
    }

    void GlobalInit() {
        if (g_inited) return;
        if (curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK)
            g_inited = true;
    }

    void GlobalExit() {
        if (!g_inited) return;
        curl_global_cleanup();
        g_inited = false;
    }

    bool Get(const char *url, std::string &out, long timeout_s) {
        out.clear();
        CURL *curl = curl_easy_init();
        if (!curl) return false;

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_s);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeout_s);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "sLaunch/0.1");
        // No CA bundle on the Switch -> skip verification.
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

        const CURLcode rc = curl_easy_perform(curl);
        long http = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
        curl_easy_cleanup(curl);

        return rc == CURLE_OK && http >= 200 && http < 300;
    }

} // namespace sl::menu::net
