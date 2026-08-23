#include "external_forwarder.h"
#include "platform.h"
#include "gc_const.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#endif

// Формат сообщения от GC (как в gc_message.cpp):
// [4 байта: тип с битом] + [4 байта: размер заголовка] + [заголовок] + [payload]

bool SplitGCMessagesSimple(const std::vector<uint8_t> &data, std::vector<std::vector<uint8_t>> &messages) {
    size_t pos = 0;
    
    while (pos < data.size()) {
        if (pos + 4 > data.size()) break;
        
        uint32_t msgType = 0;
        memcpy(&msgType, data.data() + pos, 4);
        
        // Валидные типы: либо с битом (>= 0x80000000), либо < 10000
        bool isValidType = ((msgType & 0x80000000) != 0) || (msgType < 10000);
        
        if (!isValidType) {
            // Если тип невалидный — пропускаем байт и пробуем снова
            pos++;
            continue;
        }
        
        // Ищем конец сообщения
        size_t end = pos + 4;
        while (end < data.size()) {
            uint32_t nextType = 0;
            memcpy(&nextType, data.data() + end, 4);
            
            // Если нашли следующий валидный тип — это конец текущего
            if (((nextType & 0x80000000) != 0) || (nextType < 10000)) {
                break;
            }
            end++;
        }
        
        // Если не нашли конец — берем всё до конца
        if (end == pos + 4) {
            end = data.size();
        }
        
        messages.push_back(std::vector<uint8_t>(data.begin() + pos, data.begin() + end));
        pos = end;
    }
    
    return !messages.empty();
}

bool SplitGCMessages(const std::vector<uint8_t> &data, std::vector<std::vector<uint8_t>> &messages) {
    size_t pos = 0;
    
    Platform::Print("[Split] Total data size: %zu bytes\n", data.size());
    
    while (pos < data.size()) {
        // Проверяем, что есть хотя бы 4 байта для длины
        if (pos + 4 > data.size()) {
            Platform::Print("[Split] Not enough data for length at pos %zu\n", pos);
            break;
        }
        
        uint32_t msgLen = 0;
        memcpy(&msgLen, data.data() + pos, 4);
        
        Platform::Print("[Split] msgLen = %u at pos %zu\n", msgLen, pos);
        
        // Проверяем длину
        if (msgLen == 0 || msgLen > 1024 * 1024) {
            Platform::Print("[Split] Invalid msgLen: %u\n", msgLen);
            // Если длина невалидная — возможно, это не префикс, а просто данные
            // Пробуем искать следующий валидный msgType
            break;
        }
        
        // Проверяем, что данных хватает
        if (pos + 4 + msgLen > data.size()) {
            Platform::Print("[Split] Not enough data: need %zu, have %zu\n", 
                pos + 4 + msgLen, data.size());
            // Если не хватает — возможно, это последнее сообщение
            // Добавляем остаток как сообщение
            if (pos < data.size()) {
                messages.push_back(std::vector<uint8_t>(data.begin() + pos, data.end()));
            }
            break;
        }
        
        // Копируем сообщение (без префикса длины)
        std::vector<uint8_t> msg(data.begin() + pos + 4, data.begin() + pos + 4 + msgLen);
        messages.push_back(std::move(msg));
        
        Platform::Print("[Split] Added message %zu, size: %u\n", messages.size(), msgLen);
        
        pos += 4 + msgLen;
    }
    
    // Если не удалось разделить — пробуем простой метод
    if (messages.empty()) {
        Platform::Print("[Split] Failed to split by length, trying simple method\n");
        return SplitGCMessagesSimple(data, messages);
    }
    
    return !messages.empty();
}



static std::string g_serverHost = "176.196.110.121";
static int g_serverPort = 3257;

void SetExternalServerUrl(const std::string &url) {
    // Парсим URL, чтобы извлечь хост и порт
    size_t start = url.find("://");
    if (start == std::string::npos) return;
    start += 3;

    size_t end = url.find(':', start);
    if (end != std::string::npos) {
        g_serverHost = url.substr(start, end - start);
        g_serverPort = std::stoi(url.substr(end + 1));
    }
}

static bool ConnectToServer(int &sock) {
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        Platform::Print("Socket creation failed\n");
        return false;
    }

    struct sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(g_serverPort);

    if (inet_pton(AF_INET, g_serverHost.c_str(), &serverAddr.sin_addr) <= 0) {
        struct hostent *he = gethostbyname(g_serverHost.c_str());
        if (!he) {
            Platform::Print("Failed to resolve hostname\n");
            closesocket(sock);
            return false;
        }
        memcpy(&serverAddr.sin_addr, he->h_addr_list[0], he->h_length);
    }

    if (connect(sock, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        Platform::Print("Connection to %s:%d failed\n", g_serverHost.c_str(), g_serverPort);
        closesocket(sock);
        return false;
    }

    return true;
}

static bool SendData(int sock, const void *data, size_t size) {
    const uint8_t *bytes = static_cast<const uint8_t*>(data);
    size_t totalSent = 0;
    while (totalSent < size) {
        int sent = send(sock, reinterpret_cast<const char*>(bytes + totalSent), size - totalSent, 0);
        if (sent <= 0) {
            Platform::Print("Send failed\n");
            return false;
        }
        totalSent += sent;
    }
    return true;
}

static bool ReceiveData(int sock, std::vector<uint8_t> &buffer) {
    char recvBuffer[4096];
    while (true) {
        int received = recv(sock, recvBuffer, sizeof(recvBuffer), 0);
        if (received <= 0) {
            break;
        }
        buffer.insert(buffer.end(), recvBuffer, recvBuffer + received);
        Platform::Print("[RECV] Read %d bytes\n", received);
    }
    return !buffer.empty();
}

static bool HexToBytes(const std::string &hexStr, std::vector<uint8_t> &bytes) {
    if (hexStr.empty() || (hexStr.length() % 2) != 0) return false;

    bytes.clear();
    bytes.reserve(hexStr.length() / 2);

    for (size_t i = 0; i + 1 < hexStr.length(); i += 2) {
        char hexByte[3] = { hexStr[i], hexStr[i + 1], 0 };
        char *endptr = nullptr;
        unsigned long byte = strtoul(hexByte, &endptr, 16);
        if (endptr != hexByte + 2) return false;
        bytes.push_back(static_cast<uint8_t>(byte));
    }
    return true;
}

static std::string BytesToHex(const std::vector<uint8_t> &bytes) {
    static const char hexChars[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(bytes.size() * 2);
    for (uint8_t byte : bytes) {
        hex.push_back(hexChars[(byte >> 4) & 0x0F]);
        hex.push_back(hexChars[byte & 0x0F]);
    }
    return hex;
}

uint64_t g_lastSteamId = 0;

static bool IsMsgTypeStart(const uint8_t *data, size_t size, size_t pos) {
    if (pos + 4 > size) return false;
    // Проверяем, что первый байт имеет старший бит (0x80), т.е. msgType >= 0x80000000
    // Это не 100% надёжно, но для GC-сообщений работает.
    uint32_t val = 0;
    for (int i = 0; i < 4; i++) {
        val |= (static_cast<uint32_t>(data[pos + i]) << (i * 8));
    }
    return (val & 0x80000000) != 0;
}

bool ForwardToExternalServer(uint32_t msgType, const void *data, uint32_t size, ExternalGCResponse &response) {
    response = {};
    response.msgType = msgType;

    int sock;
    if (!ConnectToServer(sock)) {
        return false;
    }

    // Формируем пакет: [steamid 8 байт] + [msgType 4 байта] + [protobuf-данные]
    uint64_t steamId = g_lastSteamId;
    const uint8_t *bytes = static_cast<const uint8_t*>(data);

    std::vector<uint8_t> packet;
    packet.reserve(8 + 4 + size);

    // SteamID (little-endian)
    for (int i = 0; i < 8; i++) {
        packet.push_back(static_cast<uint8_t>((steamId >> (i * 8)) & 0xFF));
    }

    // MsgType (little-endian)
    for (int i = 0; i < 4; i++) {
        packet.push_back(static_cast<uint8_t>((msgType >> (i * 8)) & 0xFF));
    }

    // Данные
    packet.insert(packet.end(), bytes, bytes + size);

    // Отправляем
    if (!SendData(sock, packet.data(), packet.size())) {
        closesocket(sock);
        return false;
    }

    // Закрываем сокет для записи
#ifdef _WIN32
    shutdown(sock, SD_SEND);
#else
    shutdown(sock, SHUT_WR);
#endif

    std::vector<uint8_t> allData;
    if (!ReceiveData(sock, allData)) {
        Platform::Print("Failed to receive response\n");
        closesocket(sock);
        return false;
    }

    Platform::Print("[FORWARDER] Received %zu bytes from JS server\n", allData.size());

    // ✅ РАЗБИВАЕМ ОТВЕТ НА ОТДЕЛЬНЫЕ СООБЩЕНИЯ
    std::vector<std::vector<uint8_t>> messages;
    if (!SplitGCMessages(allData, messages)) {
        Platform::Print("[FORWARDER] Failed to split messages, treating as single\n");
        // Если не получилось разделить — отправляем как есть
        response.data = std::move(allData);
        response.msgType = msgType;
        response.ok = true;
        closesocket(sock);
        return true;
    }

    Platform::Print("[FORWARDER] Split into %zu messages\n", messages.size());

    // ✅ ПРОСТО КОПИРУЕМ ВСЁ КАК ЕСТЬ (НЕ РАЗБИВАЕМ И НЕ ОБЪЕДИНЯЕМ)
    response.data = std::move(allData);
    response.msgType = msgType;
    response.ok = true;

    closesocket(sock);
    return true;
}

static bool ReadUint32LE(const uint8_t *data, size_t size, size_t pos, uint32_t &value) {
    if (pos + 4 > size) return false;
    value = data[pos] | (data[pos+1] << 8) | (data[pos+2] << 16) | (data[pos+3] << 24);
    return true;
}