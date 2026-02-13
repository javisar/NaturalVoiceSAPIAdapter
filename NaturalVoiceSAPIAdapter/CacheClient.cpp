#include "pch.h"
#include "CacheClient.h"
#include "Logger.h"
#include "StrUtils.h"
#include <winhttp.h>
#include <bcrypt.h>
#include <sstream>
#include <iomanip>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

CacheClient::CacheClient()
    : m_sourceMode(SourceMode::Endpoint)
    , m_serverUrl(L"http://localhost:8880/serving/audio")
    , m_audioBasePath(L"data/dialogues")
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

static std::wstring BuildDialogueWavPath(
    const std::wstring& basePath,
    const std::wstring& gameId,
    int actorId,
    int roomId,
    const std::wstring& hash)
{
    const wchar_t sep =
#ifdef _WIN32
        L'\\';
#else
        L'/';
#endif

    std::wstringstream fileName;
    fileName << std::setfill(L'0') << std::setw(4) << actorId
        << L"_" << std::setfill(L'0') << std::setw(4) << roomId
        << L"_" << hash << L".wav";

    std::wstring path = basePath;
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/')
        path.push_back(sep);

    path.append(gameId);
    path.push_back(sep);
    path.append(fileName.str());

    return path;
}


static std::wstring BuildDialogueWavPath2(
    const std::wstring& basePath,
    const std::wstring& gameId,
    int actorId,
    int roomId,
    const std::wstring& hash)
{
    std::wstringstream fileName;
    fileName << std::setfill(L'0') << std::setw(4) << actorId
             << L"_" << std::setfill(L'0') << std::setw(4) << roomId
             << L"_" << hash << L".wav";

    std::wstring path = basePath;
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/')
    {
        path.push_back(L'\\');
    }

    path.append(gameId);
    path.push_back(L'\\');
    path.append(fileName.str());
    return path;
}

static bool ReadAllBytes(const std::wstring& path, std::vector<BYTE>& data, DWORD& errorCode)
{
    errorCode = ERROR_SUCCESS;

    HANDLE fileHandle = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (fileHandle == INVALID_HANDLE_VALUE)
    {
        errorCode = GetLastError();
        return false;
    }

    LARGE_INTEGER fileSize = {};
    if (!GetFileSizeEx(fileHandle, &fileSize))
    {
        errorCode = GetLastError();
        CloseHandle(fileHandle);
        return false;
    }

    if (fileSize.QuadPart <= 0 || fileSize.QuadPart > static_cast<LONGLONG>(MAXDWORD))
    {
        errorCode = ERROR_INVALID_DATA;
        CloseHandle(fileHandle);
        return false;
    }

    DWORD bytesToRead = static_cast<DWORD>(fileSize.QuadPart);
    data.resize(bytesToRead);

    DWORD bytesRead = 0;
    BOOL readOk = ReadFile(fileHandle, data.data(), bytesToRead, &bytesRead, nullptr);
    CloseHandle(fileHandle);

    if (!readOk)
    {
        errorCode = GetLastError();
        data.clear();
        return false;
    }

    if (bytesRead == 0)
    {
        errorCode = ERROR_INVALID_DATA;
        data.clear();
        return false;
    }

    if (bytesRead < bytesToRead)
    {
        data.resize(bytesRead);
    }

    return true;
}

static std::wstring ComputeServerDialogueHash(
    const std::wstring& text,
    int actorId,
    int roomId)
{
    const std::string utf8Text = WStringToUTF8(text);
    const std::string hashInput = utf8Text + "|" + std::to_string(actorId) + "|" + std::to_string(roomId);
    std::vector<BYTE> inputBuffer(hashInput.begin(), hashInput.end());

    BCRYPT_ALG_HANDLE algHandle = nullptr;
    BCRYPT_HASH_HANDLE hashHandle = nullptr;
    DWORD objectLength = 0;
    DWORD hashLength = 0;
    DWORD bytesRead = 0;

    std::vector<BYTE> objectBuffer;
    std::vector<BYTE> hashBuffer;

    if (BCryptOpenAlgorithmProvider(&algHandle, BCRYPT_MD5_ALGORITHM, nullptr, 0) != STATUS_SUCCESS)
        return L"";

    if (BCryptGetProperty(
        algHandle,
        BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&objectLength),
        sizeof(objectLength),
        &bytesRead,
        0) != STATUS_SUCCESS)
    {
        BCryptCloseAlgorithmProvider(algHandle, 0);
        return L"";
    }

    if (BCryptGetProperty(
        algHandle,
        BCRYPT_HASH_LENGTH,
        reinterpret_cast<PUCHAR>(&hashLength),
        sizeof(hashLength),
        &bytesRead,
        0) != STATUS_SUCCESS)
    {
        BCryptCloseAlgorithmProvider(algHandle, 0);
        return L"";
    }

    objectBuffer.resize(objectLength);
    hashBuffer.resize(hashLength);

    if (BCryptCreateHash(
        algHandle,
        &hashHandle,
        objectBuffer.data(),
        objectLength,
        nullptr,
        0,
        0) != STATUS_SUCCESS)
    {
        BCryptCloseAlgorithmProvider(algHandle, 0);
        return L"";
    }

    if (BCryptHashData(
        hashHandle,
        inputBuffer.data(),
        static_cast<ULONG>(inputBuffer.size()),
        0) != STATUS_SUCCESS)
    {
        BCryptDestroyHash(hashHandle);
        BCryptCloseAlgorithmProvider(algHandle, 0);
        return L"";
    }

    if (BCryptFinishHash(hashHandle, hashBuffer.data(), hashLength, 0) != STATUS_SUCCESS)
    {
        BCryptDestroyHash(hashHandle);
        BCryptCloseAlgorithmProvider(algHandle, 0);
        return L"";
    }

    BCryptDestroyHash(hashHandle);
    BCryptCloseAlgorithmProvider(algHandle, 0);

    std::wstringstream digest;
    digest << std::hex << std::setfill(L'0');
    for (BYTE b : hashBuffer)
        digest << std::setw(2) << static_cast<unsigned int>(b);

    return digest.str();
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
        if (m_sourceMode == SourceMode::Disk)
        {
            std::wstring effectiveHash = ComputeServerDialogueHash(text, actorId, roomId);
            if (effectiveHash.empty())
            {
                effectiveHash = hash;
                LogWarn("CacheClient: Failed to compute MD5 hash, falling back to marker hash");
            }

            if (effectiveHash.empty())
            {
                LogDebug("CacheClient: Disk lookup skipped - missing hash");
                return result;
            }

            if (!hash.empty() && hash != effectiveHash)
            {
                LogInfo("CacheClient: Marker hash differs from server contract hash; using MD5 contract hash");
            }

            std::wstring audioPath = BuildDialogueWavPath(m_audioBasePath, gameId, actorId, roomId, effectiveHash);
            DWORD fileError = ERROR_SUCCESS;
            std::vector<BYTE> audioData;
            if (!ReadAllBytes(audioPath, audioData, fileError))
            {
                if (fileError == ERROR_FILE_NOT_FOUND || fileError == ERROR_PATH_NOT_FOUND)
                {
                    LogDebug("CacheClient: Disk cache MISS - file not found '{}'", audioPath);
                    return result;
                }

                result.errorMessage = "Disk read failed: " + std::to_string(fileError);
                LogErr("CacheClient: Disk lookup error '{}' (code={})", audioPath, fileError);
                return result;
            }

            result.hit = true;
            result.audioData = std::move(audioData);
            LogDebug("CacheClient: Disk cache HIT '{}' ({} bytes)", audioPath, result.audioData.size());
            return result;
        }

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
