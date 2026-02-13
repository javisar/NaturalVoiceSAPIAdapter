#pragma once
#include <string>
#include <vector>
#include <optional>
#include <ntstatus.h>
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
    enum class SourceMode {
        Endpoint,
        Disk,
    };

    CacheClient();
    ~CacheClient();

    // Configure cache source
    void SetSourceMode(SourceMode mode)
    {
        m_sourceMode = mode;
    }

    // Configure server endpoint
    void SetServerUrl(const std::wstring& url);

    // Configure disk lookup base path
    void SetAudioBasePath(const std::wstring& basePath)
    {
        m_audioBasePath = basePath;
    }

    // Look up dialogue audio from cache
    // Returns CacheResult with hit status and audio data
    // Note: endpoint mode computes hash from text/actor/room; disk mode requires hash from marker metadata
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
    static constexpr DWORD kDefaultTimeoutMs = 3000;

    SourceMode m_sourceMode;
    std::wstring m_serverUrl;
    std::wstring m_audioBasePath;
    DWORD m_timeoutMs;

    // HTTP POST implementation using WinHTTP
    std::optional<std::vector<BYTE>> HttpPost(
        const std::wstring& url,
        const std::string& jsonBody,
        int& statusCode
    );
};
