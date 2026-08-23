#pragma once

#include <cstdint>
#include <string>
#include <vector>

extern uint64_t g_lastSteamId;

struct ExternalGCResponse
{
    uint32_t msgType{};
    std::vector<uint8_t> data;
    bool ok{};
};

void SetExternalServerUrl(const std::string &url);
bool ForwardToExternalServer(uint32_t msgType, const void* data, uint32_t size, ExternalGCResponse &response);
bool SplitGCMessages(const std::vector<uint8_t> &data, std::vector<std::vector<uint8_t>> &messages);

