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
    int received;
    while ((received = recv(sock, recvBuffer, sizeof(recvBuffer), 0)) > 0) {
        buffer.insert(buffer.end(), reinterpret_cast<uint8_t*>(recvBuffer),
                      reinterpret_cast<uint8_t*>(recvBuffer + received));
    }
    return received == 0;
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

    // Формируем бинарный пакет: steamid (8 байт) + msgType (4 байта) + Protobuf-данные
    uint64_t steamId = g_lastSteamId;
    const uint8_t *bytes = static_cast<const uint8_t*>(data);

    std::vector<uint8_t> packet;
    packet.reserve(8 + 4 + size);

    // Записываем steamid (8 байт, little-endian)
    for (int i = 0; i < 8; i++) {
        packet.push_back(static_cast<uint8_t>((steamId >> (i * 8)) & 0xFF));
    }

    // Записываем msgType (4 байта, little-endian)
    for (int i = 0; i < 4; i++) {
        packet.push_back(static_cast<uint8_t>((msgType >> (i * 8)) & 0xFF));
    }

    // Записываем данные
    packet.insert(packet.end(), bytes, bytes + size);

    if (!SendData(sock, packet.data(), packet.size())) {
        closesocket(sock);
        return false;
    }

    // Получаем ответ от сервера (все данные)
    std::vector<uint8_t> allData;
    if (!ReceiveData(sock, allData)) {
        Platform::Print("Failed to receive response\n");
        closesocket(sock);
        return false;
    }

    closesocket(sock);

    if (allData.empty()) {
        Platform::Print("External GC response is empty\n");
        return false;
    }

    // Разбираем все сообщения, идущие подряд
    std::vector<uint8_t> combinedResponse;
    size_t pos = 0;
    size_t totalSize = allData.size();

    while (pos < totalSize) {
        // Ищем начало следующего сообщения
        if (pos + 4 > totalSize) break; // недостаточно данных для msgType

        size_t start = pos;
        // Пропускаем msgType (4 байта)
        pos += 4;

        // Ищем следующее сообщение, начиная с текущей позиции
        bool foundNext = false;
        while (pos < totalSize) {
            if (IsMsgTypeStart(allData.data(), totalSize, pos)) {
                foundNext = true;
                break;
            }
            pos++;
        }

        if (!foundNext) {
            // Если следующего сообщения нет, берём всё до конца
            pos = totalSize;
        }

        // Копируем сообщение от start до pos (включая msgType)
        combinedResponse.insert(combinedResponse.end(),
                                allData.begin() + start,
                                allData.begin() + pos);
    }

    // Теперь в combinedResponse лежат все сообщения, склеенные в правильном порядке
    response.data = std::move(combinedResponse);

    // Устанавливаем msgType первого сообщения (если есть)
    if (!response.data.empty() && response.data.size() >= 4) {
        uint32_t firstMsgType = 0;
        for (int i = 0; i < 4; i++) {
            firstMsgType |= (static_cast<uint32_t>(response.data[i]) << (i * 8));
        }
        response.msgType = firstMsgType;
    } else {
        response.msgType = msgType; // fallback
    }

    response.ok = true;

    size_t msgCount = 0;
    // Можно посчитать количество сообщений (опционально)
    for (size_t i = 0; i < combinedResponse.size(); ) {
        if (i + 4 > combinedResponse.size()) break;
        // Проверяем, что это msgType (старший бит)
        if (IsMsgTypeStart(combinedResponse.data(), combinedResponse.size(), i)) {
            msgCount++;
            i += 4;
            // Пропускаем данные до следующего msgType
            while (i < combinedResponse.size()) {
                if (IsMsgTypeStart(combinedResponse.data(), combinedResponse.size(), i)) {
                    break;
                }
                i++;
            }
        } else {
            break;
        }
    }

    Platform::Print("External GC response for msg %u -> %u (%zu bytes, %zu messages)\n",
        msgType, response.msgType, response.data.size(), msgCount);

    return true;
}
