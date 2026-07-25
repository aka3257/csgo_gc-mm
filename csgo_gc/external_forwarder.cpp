#include "external_forwarder.h"
#include "platform.h"
#include "gc_const.h"

#include <curl/curl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// Server_v2.js protocol:
//   request:  { "msgType": <uint32>, "steamid": "<uint64_t with steam id>" "data": "<full GC message hex>" }
//   response: { "msgType": <uint32 with protobuf bit>, "data": "<protobuf payload hex>" }
// sendProto() returns only the protobuf body, so we wrap it in the GC header here.

static std::string g_serverUrl = "http://176.196.110.121:3257/gc";

void SetExternalServerUrl(const std::string &url)
{
    g_serverUrl = url;
}

static void AppendUint32LE(std::vector<uint8_t> &buffer, uint32_t value)
{
    buffer.push_back(static_cast<uint8_t>(value & 0xff));
    buffer.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    buffer.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    buffer.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
}

static bool ReadUint32LE(const std::vector<uint8_t> &buffer, size_t offset, uint32_t &value)
{
    if (offset + 4 > buffer.size())
    {
        return false;
    }

    value = buffer[offset]
        | (static_cast<uint32_t>(buffer[offset + 1]) << 8)
        | (static_cast<uint32_t>(buffer[offset + 2]) << 16)
        | (static_cast<uint32_t>(buffer[offset + 3]) << 24);
    return true;
}

static bool LooksLikeFullGCMessage(uint32_t msgType, const std::vector<uint8_t> &data)
{
    if (data.size() < 8)
    {
        return false;
    }

    uint32_t embeddedType = 0;
    if (!ReadUint32LE(data, 0, embeddedType))
    {
        return false;
    }

    const uint32_t maskedType = msgType | ProtobufMask;
    return embeddedType == maskedType || embeddedType == msgType;
}

static bool WrapServerProtobufPayload(uint32_t msgType, std::vector<uint8_t> &payload)
{
    if (payload.empty())
    {
        return false;
    }

    if (LooksLikeFullGCMessage(msgType, payload))
    {
        return true;
    }

    const uint32_t maskedType = msgType | ProtobufMask;

    std::vector<uint8_t> wrapped;
    wrapped.reserve(8 + payload.size());
    AppendUint32LE(wrapped, maskedType);
    AppendUint32LE(wrapped, 0);
    wrapped.insert(wrapped.end(), payload.begin(), payload.end());
    payload = std::move(wrapped);
    return true;
}

static std::string ParseJsonString(const std::string &json, const std::string &key)
{
    const std::string searchKey = "\"" + key + "\"";
    const size_t keyPos = json.find(searchKey);
    if (keyPos == std::string::npos)
    {
        return {};
    }

    size_t valueStart = json.find(':', keyPos);
    if (valueStart == std::string::npos)
    {
        return {};
    }
    valueStart++;

    while (valueStart < json.length() && (json[valueStart] == ' ' || json[valueStart] == '\t'))
    {
        valueStart++;
    }

    if (valueStart >= json.length() || json[valueStart] != '"')
    {
        return {};
    }
    valueStart++;

    const size_t valueEnd = json.find('"', valueStart);
    if (valueEnd == std::string::npos)
    {
        return {};
    }

    return json.substr(valueStart, valueEnd - valueStart);
}

static bool ParseJsonUint32(const std::string &json, const std::string &key, uint32_t &value)
{
    const std::string searchKey = "\"" + key + "\"";
    const size_t keyPos = json.find(searchKey);
    if (keyPos == std::string::npos)
    {
        return false;
    }

    size_t valueStart = json.find(':', keyPos);
    if (valueStart == std::string::npos)
    {
        return false;
    }
    valueStart++;

    while (valueStart < json.length() && (json[valueStart] == ' ' || json[valueStart] == '\t'))
    {
        valueStart++;
    }

    const size_t valueEnd = json.find_first_of(",}", valueStart);
    const std::string number = json.substr(valueStart, valueEnd - valueStart);
    if (number.empty())
    {
        return false;
    }

    char *end = nullptr;
    const unsigned long parsed = strtoul(number.c_str(), &end, 10);
    if (end == number.c_str())
    {
        return false;
    }

    value = static_cast<uint32_t>(parsed);
    return true;
}

static bool HexToBytes(const std::string &hexStr, std::vector<uint8_t> &bytes)
{
    if (hexStr.empty() || (hexStr.length() % 2) != 0)
    {
        return false;
    }

    bytes.clear();
    bytes.reserve(hexStr.length() / 2);

    for (size_t i = 0; i + 1 < hexStr.length(); i += 2)
    {
        const char hexByte[3] = { hexStr[i], hexStr[i + 1], 0 };
        char *endptr = nullptr;
        const unsigned long byte = strtoul(hexByte, &endptr, 16);
        if (endptr != hexByte + 2)
        {
            return false;
        }

        bytes.push_back(static_cast<uint8_t>(byte));
    }

    return true;
}

uint64_t g_lastSteamId = 0;

bool ForwardToExternalServer(uint32_t msgType, const void *data, uint32_t size, ExternalGCResponse &response)
{
    response = {};
    response.msgType = msgType;

    CURL *curl = curl_easy_init();
    if (!curl)
    {
        Platform::Print("Failed to init curl\n");
        return false;
    }

    uint64_t steamId = g_lastSteamId;

    std::string json = "{ \"msgType\": " + std::to_string(msgType) + ", \"steamid\": " + std::to_string(steamId) + ", \"data\": \"";

    const auto *bytes = static_cast<const uint8_t *>(data);
    std::string hexData;
    hexData.reserve(size * 2);
    for (uint32_t i = 0; i < size; i++)
    {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02x", bytes[i]);
        hexData += hex;
    }

    json += hexData + "\" }";

    std::string responseData;
    curl_easy_setopt(curl, CURLOPT_URL, g_serverUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, json.size());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
        +[](void *contents, size_t chunkSize, size_t chunkCount, std::string *out) -> size_t {
            const size_t totalSize = chunkSize * chunkCount;
            out->append(static_cast<char *>(contents), totalSize);
            return totalSize;
        });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseData);

    curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    const CURLcode res = curl_easy_perform(curl);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
    {
        Platform::Print("Curl error: %s\n", curl_easy_strerror(res));
        return false;
    }

    uint32_t responseMsgType = msgType;
    if (!ParseJsonUint32(responseData, "msgType", responseMsgType))
    {
        responseMsgType = msgType;
    }

    std::string hexStr = ParseJsonString(responseData, "data");
    if (hexStr.empty())
    {
        hexStr = ParseJsonString(responseData, "response");
    }

    if (hexStr.empty())
    {
        Platform::Print("External GC response has no data for msg %u\n", msgType);
        return false;
    }

    if (!HexToBytes(hexStr, response.data))
    {
        Platform::Print("Failed to decode external GC response for msg %u\n", msgType);
        return false;
    }

    if (!WrapServerProtobufPayload(responseMsgType, response.data))
    {
        Platform::Print("External GC response for msg %u had empty payload\n", msgType);
        return false;
    }

    response.msgType = responseMsgType | ProtobufMask;
    response.ok = true;
    Platform::Print("External GC response for msg %u -> %u (%zu bytes)\n",
        msgType, response.msgType, response.data.size());
    return true;
}
