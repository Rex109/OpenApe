module;

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include <stb_image.h>

export module openape.iwd;

import openape.util;

// The game's .iwd files are renamed zip archives. Only the stock archives (iw_00.iwd ... iw_13.iwd) are indexed:
// archives added by mods ("xcommon_*", "jcod4x_*", ...) are ignored on purpose.
export namespace oa
{
    class IwdArchive
    {
    public:
        // `mainDir` is <root>/main.
        void Open(const fs::path& mainDir)
        {
            m_images.clear();
            std::error_code ec;
            std::vector<fs::path> files;
            for (auto& de : fs::directory_iterator(mainDir, ec))
                if (de.is_regular_file() && IsStockArchive(de.path().filename().string())) files.push_back(de.path());
            std::sort(files.begin(), files.end());
            for (auto& f : files) IndexArchive(f);
        }

        bool Available() const { return !m_images.empty(); }
        bool HasImage(const std::string& name) const { return m_images.count(ToLower(name)) != 0; }

        // Names (without extension) of every image ending in `suffix`, e.g. "_ft" for the sky boxes.
        std::vector<std::string> ImagesWithSuffix(const std::string& suffix) const
        {
            std::vector<std::string> r;
            for (auto& [n, e] : m_images) if (EndsWith(n, suffix)) r.push_back(n);
            return r;
        }

        // Reads images/<name>.iwi from the archives. Returns false when it is not in any stock archive.
        bool ReadImage(const std::string& name, std::vector<unsigned char>& out) const
        {
            auto it = m_images.find(ToLower(name));
            if (it == m_images.end()) return false;
            return Extract(it->second, out);
        }

    private:
        struct Entry
        {
            fs::path archive;
            uint32_t headerOffset = 0, compressedSize = 0, size = 0;
            uint16_t method = 0;
        };

        static bool IsStockArchive(const std::string& file)
        {
            std::string f = ToLower(file);
            if (f.size() != 9 || f.compare(0, 3, "iw_") != 0 || !EndsWith(f, ".iwd")) return false;
            return std::isdigit((unsigned char)f[3]) && std::isdigit((unsigned char)f[4]);
        }

        template <class T> static T Rd(const std::vector<unsigned char>& d, size_t o) { T v{}; if (o + sizeof(T) <= d.size()) std::memcpy(&v, &d[o], sizeof(T)); return v; }

        void IndexArchive(const fs::path& file)
        {
            FILE* f = _wfopen(file.c_str(), L"rb");
            if (!f) return;
            _fseeki64(f, 0, SEEK_END);
            int64_t size = _ftelli64(f);
            // end-of-central-directory record: signature 0x06054b50, within the last 64 KB
            int64_t tail = std::min<int64_t>(size, 66000);
            std::vector<unsigned char> buf((size_t)tail);
            _fseeki64(f, size - tail, SEEK_SET);
            if (fread(buf.data(), 1, buf.size(), f) != buf.size()) { fclose(f); return; }
            int64_t eocd = -1;
            for (int64_t i = (int64_t)buf.size() - 22; i >= 0; --i)
                if (Rd<uint32_t>(buf, (size_t)i) == 0x06054b50) { eocd = i; break; }
            if (eocd < 0) { fclose(f); return; }
            uint16_t count = Rd<uint16_t>(buf, (size_t)eocd + 10);
            uint32_t cdSize = Rd<uint32_t>(buf, (size_t)eocd + 12), cdOffset = Rd<uint32_t>(buf, (size_t)eocd + 16);
            std::vector<unsigned char> cd(cdSize);
            _fseeki64(f, cdOffset, SEEK_SET);
            if (fread(cd.data(), 1, cd.size(), f) != cd.size()) { fclose(f); return; }
            fclose(f);

            size_t p = 0;
            for (uint16_t i = 0; i < count && p + 46 <= cd.size(); ++i)
            {
                if (Rd<uint32_t>(cd, p) != 0x02014b50) break;
                Entry e;
                e.archive = file;
                e.method = Rd<uint16_t>(cd, p + 10);
                e.compressedSize = Rd<uint32_t>(cd, p + 20);
                e.size = Rd<uint32_t>(cd, p + 24);
                uint16_t nameLen = Rd<uint16_t>(cd, p + 28), extraLen = Rd<uint16_t>(cd, p + 30), commentLen = Rd<uint16_t>(cd, p + 32);
                e.headerOffset = Rd<uint32_t>(cd, p + 42);
                std::string name((const char*)&cd[p + 46], nameLen);
                p += 46 + nameLen + extraLen + commentLen;
                name = ToLower(name);
                std::replace(name.begin(), name.end(), '\\', '/');
                if (name.rfind("images/", 0) == 0 && EndsWith(name, ".iwi"))
                    m_images.emplace(name.substr(7, name.size() - 7 - 4), std::move(e));   // first archive wins, like the engine's load order
            }
        }

        static bool Extract(const Entry& e, std::vector<unsigned char>& out)
        {
            FILE* f = _wfopen(e.archive.c_str(), L"rb");
            if (!f) return false;
            unsigned char hdr[30];
            _fseeki64(f, e.headerOffset, SEEK_SET);
            if (fread(hdr, 1, 30, f) != 30 || Rd<uint32_t>(std::vector<unsigned char>(hdr, hdr + 30), 0) != 0x04034b50) { fclose(f); return false; }
            uint16_t nameLen, extraLen;
            std::memcpy(&nameLen, hdr + 26, 2); std::memcpy(&extraLen, hdr + 28, 2);
            _fseeki64(f, (int64_t)e.headerOffset + 30 + nameLen + extraLen, SEEK_SET);
            std::vector<unsigned char> data(e.compressedSize);
            bool ok = fread(data.data(), 1, data.size(), f) == data.size();
            fclose(f);
            if (!ok) return false;
            if (e.method == 0) { out = std::move(data); return true; }
            if (e.method != 8) return false;
            int outLen = 0;
            char* inflated = stbi_zlib_decode_noheader_malloc((const char*)data.data(), (int)data.size(), &outLen);
            if (!inflated) return false;
            out.assign((unsigned char*)inflated, (unsigned char*)inflated + outLen);
            free(inflated);
            return true;
        }

        std::map<std::string, Entry> m_images;
    };
}
