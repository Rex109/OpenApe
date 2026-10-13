module;
#include <filesystem>
#include <utility>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <stb_image.h>

export module openape.image;

import openape.util;

export namespace oa
{
    enum class PixelFormat { BGRA8, DXT1, DXT3, DXT5 };

    // CPU-side decoded image: BGRA8 pixels, or the raw top mip of a DXT-compressed DDS.
    struct ImageData
    {
        int width = 0, height = 0;
        PixelFormat format = PixelFormat::BGRA8;
        bool hasAlpha = false;
        std::vector<uint8_t> pixels;
        std::string description;   // "1024x1024 DXT5"
    };

    class ImageLoader
    {
    public:
        static bool IsImageFile(const fs::path& p)
        {
            auto e = ToLower(p.extension().string());
            return e == ".tga" || e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".bmp" || e == ".dds" || e == ".iwi" || e == ".psd" || e == ".gif";
        }

        static bool Load(const fs::path& path, ImageData& out, std::string& error)
        {
            std::vector<unsigned char> bytes;
            if (!ReadBinaryFile(path, bytes)) { error = "cannot read " + path.string(); return false; }
            return LoadFromMemory(bytes, ToLower(path.extension().string()), out, error);
        }

        // `ext` selects the format: ".iwi", ".dds", or anything stb_image reads.
        static bool LoadFromMemory(const std::vector<unsigned char>& bytes, const std::string& ext, ImageData& out, std::string& error)
        {
            if (ext == ".dds") return LoadDds(bytes, out, error);
            if (ext == ".iwi") return LoadIwi(bytes, out, error);
            return LoadStb(bytes, out, error);
        }

        // True when an .iwi is a cube map (the sky boxes): six faces, +X -X +Y -Y +Z -Z, stored one after the other.
        static bool IsIwiCube(const std::vector<unsigned char>& b) { return b.size() > 28 && b[0] == 'I' && b[1] == 'W' && b[2] == 'i' && (b[5] & 4); }

        static bool LoadIwiCube(const std::vector<unsigned char>& b, ImageData faces[6], std::string& error)
        {
            if (!IsIwiCube(b)) { error = "not a cube map"; return false; }
            uint8_t fmt = b[4];
            uint16_t w, h;
            std::memcpy(&w, &b[6], 2); std::memcpy(&h, &b[8], 2);
            size_t face = 0;
            PixelFormat pf;
            switch (fmt)
            {
            case 11: face = ((w + 3) / 4) * ((h + 3) / 4) * 8; pf = PixelFormat::DXT1; break;
            case 12: face = ((w + 3) / 4) * ((h + 3) / 4) * 16; pf = PixelFormat::DXT3; break;
            case 13: face = ((w + 3) / 4) * ((h + 3) / 4) * 16; pf = PixelFormat::DXT5; break;
            case 1: case 2: case 3: case 4:
            {
                // uncompressed: ARGB32 / RGB24 / grey+alpha / alpha, converted to BGRA8 like LoadIwi does
                size_t bpp = fmt == 1 ? 4 : fmt == 2 ? 3 : fmt == 3 ? 2 : 1, faceBytes = (size_t)w * h * bpp;
                if (b.size() < 28 + faceBytes * 6) { error = "truncated cube map"; return false; }
                for (int i = 0; i < 6; ++i)
                {
                    const uint8_t* src = b.data() + 28 + faceBytes * i;
                    faces[i].width = w; faces[i].height = h; faces[i].format = PixelFormat::BGRA8;
                    faces[i].pixels.resize((size_t)w * h * 4);
                    for (size_t k = 0; k < (size_t)w * h; ++k)
                    {
                        uint8_t* d = &faces[i].pixels[k * 4];
                        switch (fmt)
                        {
                        case 1: d[0] = src[k * 4]; d[1] = src[k * 4 + 1]; d[2] = src[k * 4 + 2]; d[3] = src[k * 4 + 3]; break;
                        case 2: d[0] = src[k * 3]; d[1] = src[k * 3 + 1]; d[2] = src[k * 3 + 2]; d[3] = 255; break;
                        case 3: d[0] = d[1] = d[2] = src[k * 2]; d[3] = src[k * 2 + 1]; break;
                        default: d[0] = d[1] = d[2] = 255; d[3] = src[k]; break;
                        }
                    }
                }
                return true;
            }
            default: error = "unsupported cube map format " + std::to_string(fmt); return false;
            }
            if (b.size() < 28 + face * 6) { error = "truncated cube map"; return false; }
            for (int i = 0; i < 6; ++i)
            {
                faces[i].width = w; faces[i].height = h; faces[i].format = pf;
                faces[i].pixels.assign(b.begin() + 28 + face * i, b.begin() + 28 + face * (i + 1));
            }
            return true;
        }

        // Decodes the top mip of a DXT1/3/5 image to BGRA8 in place (the shaders' special map formats are built on the CPU).
        static bool ToBgra8(ImageData& img)
        {
            if (img.format == PixelFormat::BGRA8) return true;
            const int w = img.width, h = img.height;
            const size_t blockBytes = img.format == PixelFormat::DXT1 ? 8 : 16;
            const size_t bw = (w + 3) / 4, bh = (h + 3) / 4;
            if (img.pixels.size() < bw * bh * blockBytes) return false;
            std::vector<uint8_t> out((size_t)w * h * 4);
            auto expand565 = [](uint16_t c, uint8_t* rgb) {
                rgb[0] = (uint8_t)(((c >> 11) & 31) * 255 / 31); rgb[1] = (uint8_t)(((c >> 5) & 63) * 255 / 63); rgb[2] = (uint8_t)((c & 31) * 255 / 31);
            };
            for (size_t by = 0; by < bh; ++by)
                for (size_t bx = 0; bx < bw; ++bx)
                {
                    const uint8_t* blk = &img.pixels[(by * bw + bx) * blockBytes];
                    const uint8_t* cb = blk + (blockBytes == 16 ? 8 : 0);
                    uint16_t c0 = (uint16_t)(cb[0] | (cb[1] << 8)), c1 = (uint16_t)(cb[2] | (cb[3] << 8));
                    uint8_t pal[4][4];   // r g b a
                    expand565(c0, pal[0]); expand565(c1, pal[1]); pal[0][3] = pal[1][3] = 255;
                    if (c0 > c1 || blockBytes == 16)
                        for (int k = 0; k < 3; ++k)
                        {
                            pal[2][k] = (uint8_t)((2 * pal[0][k] + pal[1][k]) / 3); pal[3][k] = (uint8_t)((pal[0][k] + 2 * pal[1][k]) / 3);
                        }
                    else
                        for (int k = 0; k < 3; ++k) { pal[2][k] = (uint8_t)((pal[0][k] + pal[1][k]) / 2); pal[3][k] = 0; }
                    pal[2][3] = 255; pal[3][3] = (c0 > c1 || blockBytes == 16) ? 255 : 0;
                    uint32_t idx = cb[4] | (cb[5] << 8) | (cb[6] << 16) | ((uint32_t)cb[7] << 24);
                    uint8_t alpha[16];
                    for (int i = 0; i < 16; ++i) alpha[i] = 255;
                    if (img.format == PixelFormat::DXT3)
                        for (int i = 0; i < 16; ++i) { uint8_t a4 = (blk[i / 2] >> ((i & 1) * 4)) & 15; alpha[i] = (uint8_t)(a4 * 17); }
                    else if (img.format == PixelFormat::DXT5)
                    {
                        uint8_t a0 = blk[0], a1 = blk[1], tab[8];
                        tab[0] = a0; tab[1] = a1;
                        if (a0 > a1) for (int k = 1; k < 7; ++k) tab[k + 1] = (uint8_t)(((7 - k) * a0 + k * a1) / 7);
                        else { for (int k = 1; k < 5; ++k) tab[k + 1] = (uint8_t)(((5 - k) * a0 + k * a1) / 5); tab[6] = 0; tab[7] = 255; }
                        uint64_t bits = 0;
                        for (int k = 0; k < 6; ++k) bits |= (uint64_t)blk[2 + k] << (8 * k);
                        for (int i = 0; i < 16; ++i) alpha[i] = tab[(bits >> (3 * i)) & 7];
                    }
                    for (int py = 0; py < 4; ++py)
                        for (int px = 0; px < 4; ++px)
                        {
                            int x = (int)bx * 4 + px, y = (int)by * 4 + py;
                            if (x >= w || y >= h) continue;
                            const uint8_t* c = pal[(idx >> (2 * (py * 4 + px))) & 3];
                            uint8_t* o = &out[((size_t)y * w + x) * 4];
                            o[0] = c[2]; o[1] = c[1]; o[2] = c[0]; o[3] = alpha[py * 4 + px];
                        }
                }
            img.pixels = std::move(out);
            img.format = PixelFormat::BGRA8;
            return true;
        }

    private:
        static bool LoadStb(const std::vector<unsigned char>& bytes, ImageData& out, std::string& error)
        {
            int w = 0, h = 0, comp = 0;
            unsigned char* px = stbi_load_from_memory(bytes.data(), (int)bytes.size(), &w, &h, &comp, 4);
            if (!px) { error = std::string("unsupported or corrupt image: ") + stbi_failure_reason(); return false; }
            out.width = w; out.height = h; out.format = PixelFormat::BGRA8;
            out.pixels.assign(px, px + (size_t)w * h * 4);
            stbi_image_free(px);
            out.hasAlpha = false;
            for (size_t i = 0; i < out.pixels.size(); i += 4)
            {
                std::swap(out.pixels[i], out.pixels[i + 2]);   // RGBA -> BGRA
                if (out.pixels[i + 3] != 255) out.hasAlpha = true;
            }
            out.description = std::to_string(w) + "x" + std::to_string(h) + (comp == 4 ? " RGBA" : " RGB");
            return true;
        }

        static uint32_t U32(const std::vector<unsigned char>& b, size_t o)
        {
            uint32_t v; std::memcpy(&v, &b[o], 4); return v;
        }

        // Call of Duty 4 ".iwi" (version 6): 28 byte header
        //   "IWi", u8 version, u8 format, u8 flags, u16 width, u16 height, u16 depth, u32 fileSizeForPicmip[4]
        // formats 1 ARGB32, 2 RGB24, 3 GA16, 4 A8, 11 DXT1, 12 DXT3, 13 DXT5; flag 2 = no mipmaps.
        // Mip levels are stored smallest first, so the full-size image is the last block of the file.
        static bool LoadIwi(const std::vector<unsigned char>& b, ImageData& out, std::string& error)
        {
            if (b.size() < 28 || b[0] != 'I' || b[1] != 'W' || b[2] != 'i') { error = "not an IWI file"; return false; }
            if (b[3] != 5 && b[3] != 6) { error = "unsupported IWI version " + std::to_string(b[3]); return false; }
            uint8_t fmt = b[4];
            uint16_t w, h;
            std::memcpy(&w, &b[6], 2); std::memcpy(&h, &b[8], 2);
            if (!w || !h) { error = "empty image"; return false; }
            out.width = w; out.height = h;
            auto levelBytes = [&](size_t lw, size_t lh) -> size_t {
                switch (fmt)
                {
                case 1: return lw * lh * 4;
                case 2: return lw * lh * 3;
                case 3: return lw * lh * 2;
                case 4: return lw * lh;
                case 11: return ((lw + 3) / 4) * ((lh + 3) / 4) * 8;
                case 12: case 13: return ((lw + 3) / 4) * ((lh + 3) / 4) * 16;
                default: return 0;
                }
            };
            size_t size = levelBytes(w, h);
            if (!size) { error = "unsupported IWI format " + std::to_string(fmt); return false; }
            if (b.size() < 28 + size) { error = "truncated IWI"; return false; }
            const uint8_t* top = b.data() + b.size() - size;
            if (b[5] & 2) top = b.data() + 28;   // single level
            const char* name = "";
            if (fmt >= 11)
            {
                out.format = fmt == 11 ? PixelFormat::DXT1 : fmt == 12 ? PixelFormat::DXT3 : PixelFormat::DXT5;
                out.pixels.assign(top, top + size);
                out.hasAlpha = fmt != 11;
                name = fmt == 11 ? "DXT1" : fmt == 12 ? "DXT3" : "DXT5";
            }
            else
            {
                out.format = PixelFormat::BGRA8;
                out.pixels.resize((size_t)w * h * 4);
                for (size_t i = 0; i < (size_t)w * h; ++i)
                {
                    uint8_t* d = &out.pixels[i * 4];
                    switch (fmt)
                    {
                    case 1: d[0] = top[i * 4]; d[1] = top[i * 4 + 1]; d[2] = top[i * 4 + 2]; d[3] = top[i * 4 + 3]; break;
                    case 2: d[0] = top[i * 3]; d[1] = top[i * 3 + 1]; d[2] = top[i * 3 + 2]; d[3] = 255; break;
                    case 3: d[0] = d[1] = d[2] = top[i * 2]; d[3] = top[i * 2 + 1]; break;
                    default: d[0] = d[1] = d[2] = 255; d[3] = top[i]; break;
                    }
                }
                out.hasAlpha = fmt == 1 || fmt == 3 || fmt == 4;
                name = fmt == 1 ? "ARGB32" : fmt == 2 ? "RGB24" : fmt == 3 ? "GA16" : "A8";
            }
            out.description = std::to_string(w) + "x" + std::to_string(h) + " " + name + " (iwi)";
            return true;
        }

        static bool LoadDds(const std::vector<unsigned char>& b, ImageData& out, std::string& error)
        {
            if (b.size() < 128 || std::memcmp(b.data(), "DDS ", 4) != 0) { error = "not a DDS file"; return false; }
            uint32_t h = U32(b, 12), w = U32(b, 16);
            uint32_t pfFlags = U32(b, 80), fourCC = U32(b, 84), bits = U32(b, 88);
            uint32_t rMask = U32(b, 92), gMask = U32(b, 96), bMask = U32(b, 100), aMask = U32(b, 104);
            out.width = (int)w; out.height = (int)h;
            const size_t dataOff = 128;
            auto fourcc = [](const char* s) { return (uint32_t)(uint8_t)s[0] | (uint32_t)(uint8_t)s[1] << 8 | (uint32_t)(uint8_t)s[2] << 16 | (uint32_t)(uint8_t)s[3] << 24; };

            if (pfFlags & 4)   // DDPF_FOURCC
            {
                size_t blockBytes; const char* name;
                if (fourCC == fourcc("DXT1")) { out.format = PixelFormat::DXT1; blockBytes = 8; name = "DXT1"; }
                else if (fourCC == fourcc("DXT3")) { out.format = PixelFormat::DXT3; blockBytes = 16; name = "DXT3"; }
                else if (fourCC == fourcc("DXT5")) { out.format = PixelFormat::DXT5; blockBytes = 16; name = "DXT5"; }
                else { error = "unsupported DDS compression"; return false; }
                size_t size = ((w + 3) / 4) * ((h + 3) / 4) * blockBytes;
                if (b.size() < dataOff + size) { error = "truncated DDS"; return false; }
                out.pixels.assign(b.begin() + dataOff, b.begin() + dataOff + size);
                out.hasAlpha = out.format != PixelFormat::DXT1;
                out.description = std::to_string(w) + "x" + std::to_string(h) + " " + name;
                return true;
            }

            if ((pfFlags & 0x40 /*RGB*/) && (bits == 32 || bits == 24))
            {
                size_t bpp = bits / 8, size = (size_t)w * h * bpp;
                if (b.size() < dataOff + size) { error = "truncated DDS"; return false; }
                out.format = PixelFormat::BGRA8;
                out.pixels.resize((size_t)w * h * 4);
                auto shiftOf = [](uint32_t m) { int s = 0; while (m && !(m & 1)) { m >>= 1; ++s; } return s; };
                int rs = shiftOf(rMask), gs = shiftOf(gMask), bs = shiftOf(bMask), as = shiftOf(aMask);
                for (size_t i = 0; i < (size_t)w * h; ++i)
                {
                    uint32_t px = 0; std::memcpy(&px, &b[dataOff + i * bpp], bpp);
                    out.pixels[i * 4 + 0] = (uint8_t)((px & bMask) >> bs);
                    out.pixels[i * 4 + 1] = (uint8_t)((px & gMask) >> gs);
                    out.pixels[i * 4 + 2] = (uint8_t)((px & rMask) >> rs);
                    out.pixels[i * 4 + 3] = (pfFlags & 1) ? (uint8_t)((px & aMask) >> as) : 255;
                }
                out.hasAlpha = (pfFlags & 1) != 0;
                out.description = std::to_string(w) + "x" + std::to_string(h) + (bits == 32 ? " ARGB8" : " RGB8");
                return true;
            }
            error = "unsupported DDS pixel format";
            return false;
        }
    };
}
