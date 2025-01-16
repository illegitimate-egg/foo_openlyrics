#include "stdafx.h"
#include <cctype>

#include "cJSON.h"

#include "logging.h"
#include "lyric_source.h"
#include "tag_util.h"

static const GUID src_guid = { 0xb4cf497f, 0xd2c, 0x45ff, { 0xaa, 0x46, 0xf1, 0x45, 0xa7, 0xf, 0x90, 0x14 } };

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

    {   // Parser gets its own scope
        const cJSON* apiResponse = NULL;
        const cJSON* apiHits = NULL;
        const cJSON* apiResult = NULL;
        const cJSON* apiPath = NULL;

        cJSON* apiRes = cJSON_Parse(content.c_str());
        if (apiRes == NULL) {
            LOG_WARN("Failed to download genius.com page %s: %s", url.c_str(), "Failed to parse JSON");
            cJSON_Delete(apiRes);
            return {};
        }

        apiResponse = cJSON_GetObjectItemCaseSensitive(apiRes, "response");
        apiHits = cJSON_GetObjectItemCaseSensitive(apiResponse, "hits");

        if (cJSON_GetArraySize(apiHits) == 0) {
            LOG_INFO("Failed to download genius.com page: No hits on search");
            cJSON_Delete(apiRes);
            return {};
        }

        apiResult = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(apiHits, 0), "result");
        apiPath = cJSON_GetObjectItemCaseSensitive(apiResult, "api_path");

        url = "https://api.genius.com" + std::string(apiPath->valuestring) + "?text_format=plain";

        cJSON_Delete(apiRes);
    }
    
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
        const cJSON* apiResponse = NULL;
        const cJSON* apiSong = NULL;
        const cJSON* apiLyrics = NULL;
        const cJSON* apiLyricsPlain = NULL;

        cJSON* apiRes = cJSON_Parse(content.c_str());
        if (apiRes == NULL) {
            LOG_WARN("Failed to download genius.com page %s: %s", url.c_str(), "Failed to parse JSON");
            cJSON_Delete(apiRes);
            return {};
        }

        apiResponse = cJSON_GetObjectItemCaseSensitive(apiRes, "response");
        apiSong = cJSON_GetObjectItemCaseSensitive(apiResponse, "song");
        apiLyrics = cJSON_GetObjectItemCaseSensitive(apiSong, "lyrics");
        apiLyricsPlain = cJSON_GetObjectItemCaseSensitive(apiLyrics, "plain");

        if (apiLyricsPlain->valuestring == NULL) {
            LOG_WARN("Failed to download from genius.com page: No lyrics data!");
            cJSON_Delete(apiRes);
            return {};
        }
        
        result.text_bytes = string_to_raw_bytes(std::string_view(apiLyricsPlain->valuestring));

        cJSON_Delete(apiRes);
    }

    return {std::move(result)};
}

bool GeniusComSource::lookup(LyricDataRaw& /*data*/, abort_callback& /*abort*/)
{
    LOG_ERROR("We should never need to do a lookup of the %s source", friendly_name().data());
    assert(false);
    return false;
}
