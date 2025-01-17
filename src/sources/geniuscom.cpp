#include "stdafx.h"
#include <cctype>

#include "cJSON.h"

#include "logging.h"
#include "lyric_source.h"
#include "tag_util.h"

static const GUID src_guid = { 0xb4cf497f, 0xd2c, 0x45ff, { 0xaa, 0x46, 0xf1, 0x45, 0xa7, 0xf, 0x90, 0x14 } };

constexpr int resultLimit = 3;

class GeniusComSource : public LyricSourceRemote
{
    const GUID& id() const final { return src_guid; }
    std::tstring_view friendly_name() const final { return _T("Genius.com"); }

    std::vector<LyricDataRaw> search(const LyricSearchParams& params, abort_callback& abort) final;
    bool lookup(LyricDataRaw& data, abort_callback& abort) final;
};
static const LyricSourceFactory<GeniusComSource> src_factory;

static std::string remove_chars_for_url(const std::string_view input)
{
    std::string transliterated = from_tstring(normalise_utf8(to_tstring(input)));

    std::string output;
    output.reserve(transliterated.length() + 3); // We add a bit to allow for one or two & or @ replacements without re-allocation
    for(char c : transliterated)
    {
        if(pfc::char_is_ascii_alphanumeric(c))
        {
            output += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        else if((c == ' ') || (c == '-'))
        {
            output += '-';
        }
        else if(c == '&')
        {
            output += "and";
        }
        else if(c == '@')
        {
            output += "at";
        }
    }

    return output;
}

std::vector<LyricDataRaw> GeniusComSource::search(const LyricSearchParams& params, abort_callback& abort)
{
    auto request = http_client::get()->create_request("GET");
    
    request->add_header("Authorization: Bearer ZTejoT_ojOEasIkT9WrMBhBQOz6eYKK5QULCMECmOhvwqjRZ6WbpamFe3geHnvp3"); //anonymous Android app token

    std::string url = "https://api.genius.com/search?q=";
    url += remove_chars_for_url(params.artist);
    url += ' ';
    url += remove_chars_for_url(params.title);

    pfc::string8 content;
    try
    {
        file_ptr response_file = request->run(url.c_str(), abort);
        response_file->read_string_raw(content, abort);
        // NOTE: We're assuming here that the response is encoded in UTF-8 
    }
    catch(const std::exception& e)
    {
        LOG_WARN("Failed to download genius.com page %s: %s", url.c_str(), e.what());
        return {};
    }

    std::vector<LyricDataRaw> lyrics;

    {   // Parser gets its own scope
        const cJSON* searchResponse = NULL;
        const cJSON* searchHits = NULL;
        const cJSON* searchHit = NULL;
        const cJSON* searchResult = NULL;
        const cJSON* searchPath = NULL;

        cJSON* searchRes = cJSON_Parse(content.c_str());
        if (searchRes == NULL) {
            LOG_WARN("Failed to download genius.com page %s: %s", url.c_str(), "Failed to parse JSON");
            cJSON_Delete(searchRes);
            return {};
        }

        searchResponse = cJSON_GetObjectItemCaseSensitive(searchRes, "response");
        searchHits = cJSON_GetObjectItemCaseSensitive(searchResponse, "hits");

        if (cJSON_GetArraySize(searchHits) == 0) {
            LOG_INFO("Failed to download genius.com page: No hits on search");
            cJSON_Delete(searchRes);
            return {};
        }

        int results = 0;
        cJSON_ArrayForEach(searchHit, searchHits) {
            results++;
            if (results > resultLimit) {
                continue;
            }

            searchResult = cJSON_GetObjectItemCaseSensitive(searchHit, "result");
            searchPath = cJSON_GetObjectItemCaseSensitive(searchResult, "api_path");

            url = "https://api.genius.com" + std::string(searchPath->valuestring) + "?text_format=plain";

            LOG_INFO("Page %s retrieved", url.c_str());
            std::string lyric_text;

            try
            {
                file_ptr response_file = request->run(url.c_str(), abort);
                response_file->read_string_raw(content, abort);
                // NOTE: We're assuming here that the response is encoded in UTF-8 
            }
            catch (const std::exception& e)
            {
                LOG_WARN("Failed to download genius.com page %s: %s", url.c_str(), e.what());
                return {};
            }

            LOG_INFO("Successfully retrieved lyrics from %s", url.c_str());

            LyricDataRaw result = {};
            result.source_id = id();
            result.source_path = url;
            result.artist = params.artist;
            result.album = params.album;
            result.title = params.title;
            result.type = LyricType::Unsynced;

            {   // Parser gets its own scope
                const cJSON* songResponse = NULL;
                const cJSON* songSong = NULL;
                const cJSON* songLyrics = NULL;
                const cJSON* songLyricsPlain = NULL;
                const cJSON* songTitle = NULL;
                const cJSON* songAlbum = NULL;
                const cJSON* songAlbumName = NULL;
                const cJSON* songAlbumArtist = NULL;

                cJSON* songRes = cJSON_Parse(content.c_str());
                if (songRes == NULL) {
                    LOG_WARN("Failed to download genius.com page %s: %s", url.c_str(), "Failed to parse JSON");
                    cJSON_Delete(songRes);
                    return {};
                }

                songResponse = cJSON_GetObjectItemCaseSensitive(songRes, "response");
                songSong = cJSON_GetObjectItemCaseSensitive(songResponse, "song");
                songLyrics = cJSON_GetObjectItemCaseSensitive(songSong, "lyrics");
                songLyricsPlain = cJSON_GetObjectItemCaseSensitive(songLyrics, "plain");
                songTitle = cJSON_GetObjectItemCaseSensitive(songSong, "title");
                songAlbum = cJSON_GetObjectItemCaseSensitive(songSong, "album");

                if (songLyricsPlain->valuestring == NULL) {
                    LOG_WARN("Failed to download from genius.com page: No lyrics data!");
                    cJSON_Delete(songRes);
                    return {};
                }

                if (songTitle->valuestring) { result.title = songTitle->valuestring; };

                if (songAlbum->valuestring) {
                    songAlbumName = cJSON_GetObjectItemCaseSensitive(songAlbum, "name");
                    songAlbumArtist = cJSON_GetObjectItemCaseSensitive(songAlbum, "primary_artist_names");

                    if (songAlbumName->valuestring) { result.title = songAlbumName->valuestring; };
                    if (songAlbumArtist->valuestring) { result.title = songAlbumArtist->valuestring; };
                }

                result.text_bytes = string_to_raw_bytes(std::string_view(songLyricsPlain->valuestring));
                lyrics.push_back(result);

                cJSON_Delete(songRes);
            }
        }

        cJSON_Delete(searchRes);
    }

    return lyrics;
}

bool GeniusComSource::lookup(LyricDataRaw& /*data*/, abort_callback& /*abort*/)
{
    LOG_ERROR("We should never need to do a lookup of the %s source", friendly_name().data());
    assert(false);
    return false;
}
