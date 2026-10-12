module;

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <windows.h>
#include <winhttp.h>

export module openape.update;

import openape.version;

// Asks the GitHub API for the latest release of the project and compares its tag with the version of this build.
export namespace oa
{
    class UpdateChecker
    {
    public:
        enum class State { Idle, Checking, UpToDate, Available, NoRelease, Failed };

        struct Result
        {
            State state = State::Idle;
            std::string latest;   // tag of the latest release without the leading 'v'
            std::string url;      // page of that release
            std::string error;
        };

        ~UpdateChecker() { if (m_worker.joinable()) m_worker.join(); }

        // Starts a check in the background (does nothing while one is running).
        void Start()
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_result.state == State::Checking) return;
                m_result = {};
                m_result.state = State::Checking;
            }
            if (m_worker.joinable()) m_worker.join();
            m_worker = std::thread([this] { Run(); });
        }

        Result Get() const { std::lock_guard<std::mutex> lock(m_mutex); return m_result; }

        // The release page may only be opened when it really is on github.com (the text comes from the network).
        static bool SafeUrl(const std::string& url) { return url.rfind("https://github.com/", 0) == 0; }

    private:
        static constexpr const wchar_t* kHost = L"api.github.com";
        static constexpr const wchar_t* kPath = L"/repos/Rex109/OpenApe/releases/latest";

        void Finish(Result r) { std::lock_guard<std::mutex> lock(m_mutex); m_result = std::move(r); }

        // the string value of a top-level-ish JSON key ("tag_name": "v0.2.0"); the first occurrence is the release's own
        static std::string JsonString(const std::string& json, const std::string& key)
        {
            size_t k = json.find("\"" + key + "\"");
            if (k == std::string::npos) return {};
            size_t q = json.find('"', json.find(':', k) + 1);
            if (q == std::string::npos) return {};
            std::string out;
            for (size_t i = q + 1; i < json.size() && json[i] != '"'; ++i)
            {
                if (json[i] == '\\' && i + 1 < json.size()) ++i;
                out += json[i];
            }
            return out;
        }

        void Run()
        {
            Result r;
            r.state = State::Failed;
            struct Handle { HINTERNET h; ~Handle() { if (h) WinHttpCloseHandle(h); } };
            Handle session{ WinHttpOpen(L"OpenApe-update-check", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0) };
            if (!session.h) { r.error = "cannot start the network session"; Finish(r); return; }
            WinHttpSetTimeouts(session.h, 6000, 6000, 8000, 8000);
            Handle connect{ WinHttpConnect(session.h, kHost, INTERNET_DEFAULT_HTTPS_PORT, 0) };
            if (!connect.h) { r.error = "cannot reach api.github.com"; Finish(r); return; }
            Handle request{ WinHttpOpenRequest(connect.h, L"GET", kPath, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) };
            if (!request.h) { r.error = "cannot create the request"; Finish(r); return; }
            const wchar_t* headers = L"Accept: application/vnd.github+json\r\n";
            if (!WinHttpSendRequest(request.h, headers, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request.h, nullptr))
            { r.error = "no answer from api.github.com (offline?)"; Finish(r); return; }

            DWORD status = 0, size = sizeof(status);
            WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
            std::string body;
            for (;;)
            {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(request.h, &avail) || avail == 0) break;
                std::string chunk(avail, '\0');
                DWORD got = 0;
                if (!WinHttpReadData(request.h, chunk.data(), avail, &got) || got == 0) break;
                body.append(chunk, 0, got);
                if (body.size() > (1u << 20)) break;
            }

            if (status == 404) { r.state = State::NoRelease; Finish(r); return; }
            if (status != 200) { r.error = "GitHub answered HTTP " + std::to_string(status); Finish(r); return; }
            std::string tag = JsonString(body, "tag_name");
            if (tag.empty()) { r.error = "the answer has no release tag"; Finish(r); return; }
            r.latest = (tag[0] == 'v' || tag[0] == 'V') ? tag.substr(1) : tag;
            r.url = JsonString(body, "html_url");
            r.state = version::IsNewer(tag, version::kVersion) ? State::Available : State::UpToDate;
            Finish(r);
        }

        mutable std::mutex m_mutex;
        Result m_result;
        std::thread m_worker;
    };
}
