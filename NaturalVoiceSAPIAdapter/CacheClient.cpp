#include "pch.h"
#include "CacheClient.h"
#include "Logger.h"
#include "StrUtils.h"
#include <winhttp.h>
#include <sstream>
#include <iomanip>

#pragma comment(lib, "winhttp.lib")

CacheClient::CacheClient()
    : m_serverUrl(L"http://localhost:8880/serving/audio")
    , m_timeoutMs(CacheClient::kDefaultTimeoutMs)
{
}

CacheClient::~CacheClient()
{
}

void CacheClient::SetServerUrl(const std::wstring& url)
{
    m_serverUrl = url;
}

// Helper function to escape JSON strings
static std::string JsonEscape(const std::wstring& str)
{
    std::string utf8 = WStringToUTF8(str);
    std::string escaped;
    escaped.reserve(utf8.size());

    for (char ch : utf8)
    {
        switch (ch)
        {
        case '"':  escaped.append("\\\""); break;
        case '\\': escaped.append("\\\\"); break;
        case '\b': escaped.append("\\b"); break;
        case '\f': escaped.append("\\f"); break;
        case '\n': escaped.append("\\n"); break;
        case '\r': escaped.append("\\r"); break;
        case '\t': escaped.append("\\t"); break;
        default:
            if (ch >= 0 && ch < 32)
            {
                // Control characters - escape as \uXXXX
                escaped.append("\\u00");
                char hex[3];
                sprintf_s(hex, "%02x", (unsigned char)ch);
                escaped.append(hex);
            }
            else
            {
                escaped.push_back(ch);
            }
            break;
        }
    }

    return escaped;
}

std::optional<std::vector<BYTE>> CacheClient::HttpPost(
    const std::wstring& url,
    const std::string& jsonBody,
    int& statusCode)
{
    statusCode = 0;

    // Parse URL to extract host, port, path
    URL_COMPONENTS urlComp = { 0 };
    urlComp.dwStructSize = sizeof(urlComp);

    WCHAR hostName[256] = { 0 };
    WCHAR urlPath[1024] = { 0 };

    urlComp.lpszHostName = hostName;
    urlComp.dwHostNameLength = sizeof(hostName) / sizeof(hostName[0]);
    urlComp.lpszUrlPath = urlPath;
    urlComp.dwUrlPathLength = sizeof(urlPath) / sizeof(urlPath[0]);

    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &urlComp))
    {
        LogErr("CacheClient: Failed to parse URL: {}", url);
        return std::nullopt;
    }

    // Initialize WinHTTP
    HINTERNET hSession = WinHttpOpen(
        L"NaturalVoiceSAPIAdapter/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);

    if (!hSession)
    {
        LogDebug("CacheClient: WinHttpOpen failed: {}", GetLastError());
        return std::nullopt;
    }

    // Set timeout
    WinHttpSetTimeouts(hSession, m_timeoutMs, m_timeoutMs, m_timeoutMs, m_timeoutMs);

    // Connect to server
    HINTERNET hConnect = WinHttpConnect(
        hSession,
        hostName,
        urlComp.nPort,
        0);

    if (!hConnect)
    {
        LogDebug("CacheClient: WinHttpConnect failed: {}", GetLastError());
        WinHttpCloseHandle(hSession);
        return std::nullopt;
    }

    // Create request
    DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect,
        L"POST",
        urlPath,
        NULL,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        flags);

    if (!hRequest)
    {
        LogDebug("CacheClient: WinHttpOpenRequest failed: {}", GetLastError());
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return std::nullopt;
    }

    // Set Content-Type header
    std::wstring headers = L"Content-Type: application/json\r\n";
    WinHttpAddRequestHeaders(
        hRequest,
        headers.c_str(),
        (DWORD)-1,
        WINHTTP_ADDREQ_FLAG_ADD);

    // Send request
    BOOL result = WinHttpSendRequest(
        hRequest,
        WINHTTP_NO_ADDITIONAL_HEADERS,
        0,
        (LPVOID)jsonBody.c_str(),
        (DWORD)jsonBody.size(),
        (DWORD)jsonBody.size(),
        0);

    if (!result)
    {
        LogDebug("CacheClient: WinHttpSendRequest failed: {}", GetLastError());
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return std::nullopt;
    }

    // Receive response
    result = WinHttpReceiveResponse(hRequest, NULL);
    if (!result)
    {
        LogDebug("CacheClient: WinHttpReceiveResponse failed: {}", GetLastError());
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return std::nullopt;
    }

    // Get status code
    DWORD statusCodeSize = sizeof(statusCode);
    WinHttpQueryHeaders(
        hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &statusCode,
        &statusCodeSize,
        WINHTTP_NO_HEADER_INDEX);

    // Read response body
    std::vector<BYTE> responseData;
    DWORD bytesAvailable = 0;

    do
    {
        bytesAvailable = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &bytesAvailable))
        {
            LogDebug("CacheClient: WinHttpQueryDataAvailable failed: {}", GetLastError());
            break;
        }

        if (bytesAvailable > 0)
        {
            std::vector<BYTE> buffer(bytesAvailable);
            DWORD bytesRead = 0;

            if (WinHttpReadData(hRequest, buffer.data(), bytesAvailable, &bytesRead))
            {
                responseData.insert(responseData.end(), buffer.begin(), buffer.begin() + bytesRead);
            }
            else
            {
                LogDebug("CacheClient: WinHttpReadData failed: {}", GetLastError());
                break;
            }
        }
    } while (bytesAvailable > 0);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return responseData;
}

CacheResult CacheClient::LookupAudio(
    const std::wstring& gameId,
    const std::wstring& text,
    const std::wstring& hash,
    int actorId,
    int roomId)
{
    CacheResult result;
    result.hit = false;
    result.fallbackAllowed = true;  // Default to allowing fallback

    try
    {
        // Build JSON body
        // Server computes hash from (text, actor_id, room_id), so we pass empty hash
        std::ostringstream json;
        json << "{"
             << "\"text\":\"" << JsonEscape(text) << "\","
             << "\"hash\":\"\","  // Empty - server computes
             << "\"game_id\":\"" << JsonEscape(gameId) << "\","
             << "\"actor_id\":" << actorId << ","
             << "\"room_id\":" << roomId
             << "}";

        std::string jsonBody = json.str();

        // Send HTTP POST
        int statusCode = 0;
        auto response = HttpPost(m_serverUrl, jsonBody, statusCode);

        if (!response.has_value())
        {
            // Network error or timeout
            result.errorMessage = "Server unavailable or timeout";
            LogDebug("CacheClient: Lookup failed - {}", result.errorMessage);
            return result;
        }

        if (statusCode == 200)
        {
            // Cache hit - response is WAV audio data
            result.hit = true;
            result.audioData = std::move(response.value());
            LogDebug("CacheClient: Cache HIT - received {} bytes", result.audioData.size());
            return result;
        }
        else if (statusCode == 404)
        {
            // Cache miss - parse JSON response for fallback_allowed
            std::string responseStr(response.value().begin(), response.value().end());

            // Simple JSON parsing for fallback_allowed field
            // Look for "fallback_allowed":true or "fallback_allowed":false
            size_t pos = responseStr.find("\"fallback_allowed\"");
            if (pos != std::string::npos)
            {
                size_t colonPos = responseStr.find(':', pos);
                if (colonPos != std::string::npos)
                {
                    size_t truePos = responseStr.find("true", colonPos);
                    size_t falsePos = responseStr.find("false", colonPos);

                    // Check which comes first after the colon
                    if (truePos != std::string::npos &&
                        (falsePos == std::string::npos || truePos < falsePos))
                    {
                        result.fallbackAllowed = true;
                    }
                    else if (falsePos != std::string::npos)
                    {
                        result.fallbackAllowed = false;
                    }
                }
            }

            LogDebug("CacheClient: Cache MISS - fallback_allowed={}", result.fallbackAllowed);
            return result;
        }
        else
        {
            // Other error
            result.errorMessage = "Server returned status " + std::to_string(statusCode);
            LogDebug("CacheClient: Lookup failed - {}", result.errorMessage);
            return result;
        }
    }
    catch (const std::exception& ex)
    {
        result.errorMessage = ex.what();
        LogErr("CacheClient: Exception during lookup: {}", ex);
        return result;
    }
}

bool CacheClient::IsServerAvailable()
{
    // Quick availability check - try to connect with short timeout
    try
    {
        // Simple empty POST to check connectivity
        int statusCode = 0;
        auto response = HttpPost(m_serverUrl, "{}", statusCode);

        // Any response (even error) means server is reachable
        return response.has_value();
    }
    catch (...)
    {
        return false;
    }
}
