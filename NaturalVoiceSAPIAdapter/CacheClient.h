#pragma once
#include <string>
#include <vector>
#include <optional>
#include <Windows.h>

// Result of cache lookup
struct CacheResult {
    bool hit;                      // true if audio found
    std::vector<BYTE> audioData;   // WAV audio bytes (empty if miss)
    bool fallbackAllowed;          // server says fallback TTS is OK
    std::string errorMessage;      // error details if any
};

class CacheClient {
public:
    CacheClient();
    ~CacheClient();

    // Configure server endpoint
    void SetServerUrl(const std::wstring& url);

    // Look up dialogue audio from cache
    // Returns CacheResult with hit status and audio data
    // Note: hash parameter is optional/ignored - server computes hash from text/actor/room
    CacheResult LookupAudio(
        const std::wstring& gameId,
        const std::wstring& text,
        const std::wstring& hash,  // Optional - server computes if empty
        int actorId,
        int roomId
    );

    // Check if server is reachable
    bool IsServerAvailable();

private:
    std::wstring m_serverUrl;
    DWORD m_timeoutMs;

    // HTTP POST implementation using WinHTTP
    std::optional<std::vector<BYTE>> HttpPost(
        const std::wstring& url,
        const std::string& jsonBody,
        int& statusCode
    );
};
