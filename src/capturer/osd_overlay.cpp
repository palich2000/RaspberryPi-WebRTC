//
// Created for the OSD text-overlay feature.
//
#include "osd_overlay.h"

#include <algorithm>
#include <vector>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <poll.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/logging.h"

namespace {

// Rendering parameters (kept in sync with the clock overlay look).
constexpr int kScale = 2;       // integer font scale
constexpr uint8_t kYText = 235; // "white" luma in limited range
constexpr uint8_t kAlpha = 255; // 255 = overwrite, <255 = blend
constexpr int kNeutralUv = 1;   // force U/V=128 in touched pairs -> gray/white text
constexpr int kPad = 10;        // padding inside the background box
constexpr int kGlyphW = 8;      // font cell width
constexpr int kGlyphH = 8;      // font cell height
constexpr int kSpacing = 1;     // inter-glyph spacing in font pixels
constexpr size_t kMaxLineLen = 256;

// Public-domain 8x8 font (font8x8_basic by Daniel Hepper). Bit 0 (LSB) is the
// leftmost column of each 8-pixel row. Non-printable codes are left blank.
const uint8_t kFont8x8[128][8] = {
    [0x20] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // (space)
    [0x21] = {0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00}, // !
    [0x22] = {0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // "
    [0x23] = {0x36, 0x36, 0x7F, 0x36, 0x7F, 0x36, 0x36, 0x00}, // #
    [0x24] = {0x0C, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x0C, 0x00}, // $
    [0x25] = {0x00, 0x63, 0x33, 0x18, 0x0C, 0x66, 0x63, 0x00}, // %
    [0x26] = {0x1C, 0x36, 0x1C, 0x6E, 0x3B, 0x33, 0x6E, 0x00}, // &
    [0x27] = {0x06, 0x06, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00}, // '
    [0x28] = {0x18, 0x0C, 0x06, 0x06, 0x06, 0x0C, 0x18, 0x00}, // (
    [0x29] = {0x06, 0x0C, 0x18, 0x18, 0x18, 0x0C, 0x06, 0x00}, // )
    [0x2A] = {0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00}, // *
    [0x2B] = {0x00, 0x0C, 0x0C, 0x3F, 0x0C, 0x0C, 0x00, 0x00}, // +
    [0x2C] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x06}, // ,
    [0x2D] = {0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x00}, // -
    [0x2E] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00}, // .
    [0x2F] = {0x60, 0x30, 0x18, 0x0C, 0x06, 0x03, 0x01, 0x00}, // /
    [0x30] = {0x3E, 0x63, 0x73, 0x7B, 0x6F, 0x67, 0x3E, 0x00}, // 0
    [0x31] = {0x0C, 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x3F, 0x00}, // 1
    [0x32] = {0x1E, 0x33, 0x30, 0x1C, 0x06, 0x33, 0x3F, 0x00}, // 2
    [0x33] = {0x1E, 0x33, 0x30, 0x1C, 0x30, 0x33, 0x1E, 0x00}, // 3
    [0x34] = {0x38, 0x3C, 0x36, 0x33, 0x7F, 0x30, 0x78, 0x00}, // 4
    [0x35] = {0x3F, 0x03, 0x1F, 0x30, 0x30, 0x33, 0x1E, 0x00}, // 5
    [0x36] = {0x1C, 0x06, 0x03, 0x1F, 0x33, 0x33, 0x1E, 0x00}, // 6
    [0x37] = {0x3F, 0x33, 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x00}, // 7
    [0x38] = {0x1E, 0x33, 0x33, 0x1E, 0x33, 0x33, 0x1E, 0x00}, // 8
    [0x39] = {0x1E, 0x33, 0x33, 0x3E, 0x30, 0x18, 0x0E, 0x00}, // 9
    [0x3A] = {0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x00}, // :
    [0x3B] = {0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x06}, // ;
    [0x3C] = {0x18, 0x0C, 0x06, 0x03, 0x06, 0x0C, 0x18, 0x00}, // <
    [0x3D] = {0x00, 0x00, 0x3F, 0x00, 0x00, 0x3F, 0x00, 0x00}, // =
    [0x3E] = {0x06, 0x0C, 0x18, 0x30, 0x18, 0x0C, 0x06, 0x00}, // >
    [0x3F] = {0x1E, 0x33, 0x30, 0x18, 0x0C, 0x00, 0x0C, 0x00}, // ?
    [0x40] = {0x3E, 0x63, 0x7B, 0x7B, 0x7B, 0x03, 0x1E, 0x00}, // @
    [0x41] = {0x0C, 0x1E, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x00}, // A
    [0x42] = {0x3F, 0x66, 0x66, 0x3E, 0x66, 0x66, 0x3F, 0x00}, // B
    [0x43] = {0x3C, 0x66, 0x03, 0x03, 0x03, 0x66, 0x3C, 0x00}, // C
    [0x44] = {0x1F, 0x36, 0x66, 0x66, 0x66, 0x36, 0x1F, 0x00}, // D
    [0x45] = {0x7F, 0x46, 0x16, 0x1E, 0x16, 0x46, 0x7F, 0x00}, // E
    [0x46] = {0x7F, 0x46, 0x16, 0x1E, 0x16, 0x06, 0x0F, 0x00}, // F
    [0x47] = {0x3C, 0x66, 0x03, 0x03, 0x73, 0x66, 0x7C, 0x00}, // G
    [0x48] = {0x33, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x33, 0x00}, // H
    [0x49] = {0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00}, // I
    [0x4A] = {0x78, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E, 0x00}, // J
    [0x4B] = {0x67, 0x66, 0x36, 0x1E, 0x36, 0x66, 0x67, 0x00}, // K
    [0x4C] = {0x0F, 0x06, 0x06, 0x06, 0x46, 0x66, 0x7F, 0x00}, // L
    [0x4D] = {0x63, 0x77, 0x7F, 0x7F, 0x6B, 0x63, 0x63, 0x00}, // M
    [0x4E] = {0x63, 0x67, 0x6F, 0x7B, 0x73, 0x63, 0x63, 0x00}, // N
    [0x4F] = {0x1C, 0x36, 0x63, 0x63, 0x63, 0x36, 0x1C, 0x00}, // O
    [0x50] = {0x3F, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x0F, 0x00}, // P
    [0x51] = {0x1E, 0x33, 0x33, 0x33, 0x3B, 0x1E, 0x38, 0x00}, // Q
    [0x52] = {0x3F, 0x66, 0x66, 0x3E, 0x36, 0x66, 0x67, 0x00}, // R
    [0x53] = {0x1E, 0x33, 0x07, 0x0E, 0x38, 0x33, 0x1E, 0x00}, // S
    [0x54] = {0x3F, 0x2D, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00}, // T
    [0x55] = {0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x3F, 0x00}, // U
    [0x56] = {0x33, 0x33, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00}, // V
    [0x57] = {0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00}, // W
    [0x58] = {0x63, 0x63, 0x36, 0x1C, 0x1C, 0x36, 0x63, 0x00}, // X
    [0x59] = {0x33, 0x33, 0x33, 0x1E, 0x0C, 0x0C, 0x1E, 0x00}, // Y
    [0x5A] = {0x7F, 0x63, 0x31, 0x18, 0x4C, 0x66, 0x7F, 0x00}, // Z
    [0x5B] = {0x1E, 0x06, 0x06, 0x06, 0x06, 0x06, 0x1E, 0x00}, // [
    [0x5C] = {0x03, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x40, 0x00}, // (backslash)
    [0x5D] = {0x1E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1E, 0x00}, // ]
    [0x5E] = {0x08, 0x1C, 0x36, 0x63, 0x00, 0x00, 0x00, 0x00}, // ^
    [0x5F] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF}, // _
    [0x60] = {0x0C, 0x0C, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00}, // `
    [0x61] = {0x00, 0x00, 0x1E, 0x30, 0x3E, 0x33, 0x6E, 0x00}, // a
    [0x62] = {0x07, 0x06, 0x06, 0x3E, 0x66, 0x66, 0x3B, 0x00}, // b
    [0x63] = {0x00, 0x00, 0x1E, 0x33, 0x03, 0x33, 0x1E, 0x00}, // c
    [0x64] = {0x38, 0x30, 0x30, 0x3E, 0x33, 0x33, 0x6E, 0x00}, // d
    [0x65] = {0x00, 0x00, 0x1E, 0x33, 0x3F, 0x03, 0x1E, 0x00}, // e
    [0x66] = {0x1C, 0x36, 0x06, 0x0F, 0x06, 0x06, 0x0F, 0x00}, // f
    [0x67] = {0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x1F}, // g
    [0x68] = {0x07, 0x06, 0x36, 0x6E, 0x66, 0x66, 0x67, 0x00}, // h
    [0x69] = {0x0C, 0x00, 0x0E, 0x0C, 0x0C, 0x0C, 0x1E, 0x00}, // i
    [0x6A] = {0x30, 0x00, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E}, // j
    [0x6B] = {0x07, 0x06, 0x66, 0x36, 0x1E, 0x36, 0x67, 0x00}, // k
    [0x6C] = {0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00}, // l
    [0x6D] = {0x00, 0x00, 0x33, 0x7F, 0x7F, 0x6B, 0x63, 0x00}, // m
    [0x6E] = {0x00, 0x00, 0x1F, 0x33, 0x33, 0x33, 0x33, 0x00}, // n
    [0x6F] = {0x00, 0x00, 0x1E, 0x33, 0x33, 0x33, 0x1E, 0x00}, // o
    [0x70] = {0x00, 0x00, 0x3B, 0x66, 0x66, 0x3E, 0x06, 0x0F}, // p
    [0x71] = {0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x78}, // q
    [0x72] = {0x00, 0x00, 0x3B, 0x6E, 0x66, 0x06, 0x0F, 0x00}, // r
    [0x73] = {0x00, 0x00, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x00}, // s
    [0x74] = {0x08, 0x0C, 0x3E, 0x0C, 0x0C, 0x2C, 0x18, 0x00}, // t
    [0x75] = {0x00, 0x00, 0x33, 0x33, 0x33, 0x33, 0x6E, 0x00}, // u
    [0x76] = {0x00, 0x00, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00}, // v
    [0x77] = {0x00, 0x00, 0x63, 0x6B, 0x7F, 0x7F, 0x36, 0x00}, // w
    [0x78] = {0x00, 0x00, 0x63, 0x36, 0x1C, 0x36, 0x63, 0x00}, // x
    [0x79] = {0x00, 0x00, 0x33, 0x33, 0x33, 0x3E, 0x30, 0x1F}, // y
    [0x7A] = {0x00, 0x00, 0x3F, 0x19, 0x0C, 0x26, 0x3F, 0x00}, // z
    [0x7B] = {0x38, 0x0C, 0x0C, 0x07, 0x0C, 0x0C, 0x38, 0x00}, // {
    [0x7C] = {0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x00}, // |
    [0x7D] = {0x07, 0x0C, 0x0C, 0x38, 0x0C, 0x0C, 0x07, 0x00}, // }
    [0x7E] = {0x6E, 0x3B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // ~
};

inline int clampi(int v, int lo, int hi) {
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

// Fill a rectangle black (Y=0, U=V=128) in a packed YUV422 buffer.
void fill_rect_black(uint8_t *buf, int width, int height, int stride, int x, int y, int w, int h,
                     yuv422_fmt_t fmt) {
    if (!buf || width <= 0 || height <= 0 || stride <= 0 || w <= 0 || h <= 0)
        return;

    int x0 = clampi(x, 0, width);
    int y0 = clampi(y, 0, height);
    int x1 = clampi(x + w, 0, width);
    int y1 = clampi(y + h, 0, height);
    if (x1 <= x0 || y1 <= y0)
        return;

    int ax0 = x0 & ~1;
    int ax1 = (x1 + 1) & ~1;
    if (ax1 > width)
        ax1 = width;

    for (int yy = y0; yy < y1; yy++) {
        uint8_t *row = buf + yy * stride;
        for (int xx = ax0; xx < ax1; xx += 2) {
            uint8_t *p = row + xx * 2; // 4 bytes per 2 pixels
            if (fmt == FMT_YUYV) {     // [Y0 U Y1 V]
                p[0] = 0;
                p[1] = 128;
                p[2] = 0;
                p[3] = 128;
            } else { // UYVY: [U Y0 V Y1]
                p[0] = 128;
                p[1] = 0;
                p[2] = 128;
                p[3] = 0;
            }
        }
    }
}

// Set / blend one pixel luma (Y) at (x,y) in a packed YUV422 buffer.
inline void put_or_blend_Y(uint8_t *base, int stride, int x, int y, yuv422_fmt_t fmt, uint8_t Y,
                           uint8_t alpha, int neutral_uv) {
    uint8_t *row = base + y * stride;
    int pair_off = (x & ~1) * 2; // 4 bytes per pair
    int yoff;
    if (fmt == FMT_YUYV) {
        yoff = pair_off + (x & 1 ? 2 : 0);
    } else {
        yoff = pair_off + (x & 1 ? 3 : 1);
    }

    if (alpha == 255) {
        row[yoff] = Y;
    } else {
        int Yo = row[yoff];
        row[yoff] = (uint8_t)(((255 - alpha) * Yo + alpha * (int)Y + 127) / 255);
    }

    if (neutral_uv) {
        if (fmt == FMT_YUYV) {
            row[pair_off + 1] = 128;
            row[pair_off + 3] = 128;
        } else {
            row[pair_off + 0] = 128;
            row[pair_off + 2] = 128;
        }
    }
}

void draw_glyph(uint8_t *img, int w, int h, int stride, int x0, int y0, yuv422_fmt_t fmt, char ch,
                int scale) {
    const uint8_t *g = kFont8x8[(unsigned char)ch & 0x7F];
    for (int ry = 0; ry < kGlyphH; ry++) {
        uint8_t bits = g[ry];
        for (int rx = 0; rx < kGlyphW; rx++) {
            if (!((bits >> rx) & 1)) // bit 0 (LSB) = leftmost column
                continue;
            int px = x0 + rx * scale;
            int py = y0 + ry * scale;
            for (int sy = 0; sy < scale; sy++) {
                int yy = py + sy;
                if ((unsigned)yy >= (unsigned)h)
                    continue;
                for (int sx = 0; sx < scale; sx++) {
                    int xx = px + sx;
                    if ((unsigned)xx >= (unsigned)w)
                        continue;
                    put_or_blend_Y(img, stride, xx, yy, fmt, kYText, kAlpha, kNeutralUv);
                }
            }
        }
    }
}

// Read the first line of `path`, trimmed of trailing CR/LF. Returns false on
// any I/O error or empty content.
bool read_first_line(const std::string &path, std::string &out) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    char buf[kMaxLineLen];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return false;
    buf[n] = '\0';
    // Cut at the first newline.
    char *nl = strchr(buf, '\n');
    if (nl)
        *nl = '\0';
    // Trim trailing CR / whitespace.
    std::string s(buf);
    while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    if (s.empty())
        return false;
    out = std::move(s);
    return true;
}

// Natural-order comparison: digit runs compare by numeric value, so "osd2"
// sorts before "osd10". Non-digit runs compare byte-by-byte.
bool natural_less(const std::string &a, const std::string &b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        unsigned char ca = a[i], cb = b[j];
        if (isdigit(ca) && isdigit(cb)) {
            // Skip leading zeros, then compare digit-run length, then digits.
            size_t si = i, sj = j;
            while (si < a.size() && a[si] == '0')
                si++;
            while (sj < b.size() && b[sj] == '0')
                sj++;
            size_t ei = si, ej = sj;
            while (ei < a.size() && isdigit((unsigned char)a[ei]))
                ei++;
            while (ej < b.size() && isdigit((unsigned char)b[ej]))
                ej++;
            size_t la = ei - si, lb = ej - sj;
            if (la != lb)
                return la < lb; // fewer significant digits = smaller number
            int cmp = a.compare(si, la, b, sj, lb);
            if (cmp != 0)
                return cmp < 0;
            // Equal numeric value; fewer leading zeros first for a stable order.
            if ((si - i) != (sj - j))
                return (si - i) < (sj - j);
            i = ei;
            j = ej;
            continue;
        }
        if (ca != cb)
            return ca < cb;
        i++;
        j++;
    }
    return (a.size() - i) < (b.size() - j);
}

// Split a path glob "/dir/name*" into directory ("/dir", default ".") and
// basename pattern ("name*", default "*").
void split_pattern(const std::string &pattern, std::string &dir, std::string &glob) {
    auto slash = pattern.find_last_of('/');
    if (slash == std::string::npos) {
        dir = ".";
        glob = pattern.empty() ? "*" : pattern;
    } else {
        dir = pattern.substr(0, slash);
        if (dir.empty())
            dir = "/";
        glob = pattern.substr(slash + 1);
        if (glob.empty())
            glob = "*";
    }
}

} // namespace

OsdOverlay::OsdOverlay(const std::string &pattern) : running_(true) {
    stop_pipe_[0] = stop_pipe_[1] = -1;
    split_pattern(pattern, dir_, glob_);
    if (pipe2(stop_pipe_, O_CLOEXEC) != 0) {
        ERROR_PRINT("OSD: pipe2 failed: %s", strerror(errno));
        running_ = false;
        return;
    }
    INFO_PRINT("OSD overlay enabled: watching '%s' for files matching '%s'", dir_.c_str(),
               glob_.c_str());
    thread_ = std::thread(&OsdOverlay::WatchLoop, this);
}

OsdOverlay::~OsdOverlay() {
    running_ = false;
    if (stop_pipe_[1] >= 0) {
        char c = 0;
        ssize_t r = write(stop_pipe_[1], &c, 1); // unblock poll()
        (void)r;
    }
    if (thread_.joinable())
        thread_.join();
    if (stop_pipe_[0] >= 0)
        close(stop_pipe_[0]);
    if (stop_pipe_[1] >= 0)
        close(stop_pipe_[1]);
}

void OsdOverlay::Rescan() {
    DIR *d = opendir(dir_.c_str());
    if (!d) {
        // Directory may not exist yet; clear the overlay and wait for it.
        std::lock_guard<std::mutex> lk(mtx_);
        text_.clear();
        return;
    }

    std::vector<std::string> names;
    struct dirent *de;
    while ((de = readdir(d)) != nullptr) {
        if (de->d_name[0] == '.' &&
            (de->d_name[1] == '\0' || (de->d_name[1] == '.' && de->d_name[2] == '\0')))
            continue; // skip . and ..
        if (fnmatch(glob_.c_str(), de->d_name, 0) != 0)
            continue;
        names.emplace_back(de->d_name);
    }
    closedir(d);

    std::sort(names.begin(), names.end(), natural_less);

    std::string composed;
    for (const auto &name : names) {
        std::string path = dir_ + "/" + name;
        struct stat st;
        if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        std::string line;
        if (!read_first_line(path, line))
            continue;
        if (!composed.empty())
            composed += " | ";
        composed += line;
    }

    std::lock_guard<std::mutex> lk(mtx_);
    text_ = std::move(composed);
}

void OsdOverlay::WatchLoop() {
    int ino_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ino_fd < 0) {
        ERROR_PRINT("OSD: inotify_init1 failed: %s", strerror(errno));
        return;
    }

    const uint32_t mask = IN_CLOSE_WRITE | IN_CREATE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE |
                          IN_MODIFY | IN_DELETE_SELF | IN_MOVE_SELF;
    // A missing directory at startup is NOT an error: we keep retrying below and
    // start drawing once it (and its files) appear. `waiting` tracks the state so
    // we log the transition once instead of on every 1s retry.
    int wd = inotify_add_watch(ino_fd, dir_.c_str(), mask);
    bool waiting = (wd < 0);
    if (waiting)
        INFO_PRINT("OSD: directory '%s' not present yet - will start drawing once it appears",
                   dir_.c_str());

    Rescan(); // pick up files that already exist

    char evbuf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    while (running_) {
        struct pollfd pfds[2];
        pfds[0].fd = ino_fd;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        pfds[1].fd = stop_pipe_[0];
        pfds[1].events = POLLIN;
        pfds[1].revents = 0;

        // If the directory is not being watched yet (missing at start or removed),
        // poll with a timeout so we can retry adding the watch.
        int timeout = (wd < 0) ? 1000 : -1;
        int pr = poll(pfds, 2, timeout);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            ERROR_PRINT("OSD: poll failed: %s", strerror(errno));
            break;
        }
        if (!running_)
            break;

        if (pfds[1].revents & POLLIN)
            break; // shutdown signaled

        if (wd < 0) {
            // Retry establishing the watch; if it succeeds, rescan immediately.
            wd = inotify_add_watch(ino_fd, dir_.c_str(), mask);
            if (wd >= 0) {
                if (waiting) {
                    INFO_PRINT("OSD: directory '%s' appeared - overlay active", dir_.c_str());
                    waiting = false;
                }
                Rescan();
            }
            continue;
        }

        if (pfds[0].revents & POLLIN) {
            bool dir_gone = false;
            ssize_t len;
            while ((len = read(ino_fd, evbuf, sizeof(evbuf))) > 0) {
                for (char *p = evbuf; p < evbuf + len;) {
                    auto *ev = reinterpret_cast<struct inotify_event *>(p);
                    if (ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED))
                        dir_gone = true;
                    p += sizeof(struct inotify_event) + ev->len;
                }
            }
            if (dir_gone) {
                inotify_rm_watch(ino_fd, wd);
                wd = -1;
                waiting = true;
                INFO_PRINT("OSD: directory '%s' removed - waiting for it to reappear",
                           dir_.c_str());
            }
            // Recompose once per batch of events (debounced).
            Rescan();
        }
    }

    if (wd >= 0)
        inotify_rm_watch(ino_fd, wd);
    close(ino_fd);
}

void OsdOverlay::Draw(uint8_t *img, int width, int height, int stride, yuv422_fmt_t fmt) {
    std::string text;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (text_.empty())
            return;
        text = text_;
    }

    int cell = (kGlyphW + kSpacing) * kScale; // advance per character
    int text_w = (int)text.size() * cell - kSpacing * kScale;
    if (text_w < 0)
        text_w = 0;
    int text_h = kGlyphH * kScale;

    int box_w = text_w + 2 * kPad;
    int box_h = text_h + 2 * kPad;
    int box_x = width - box_w;
    if (box_x < 0)
        box_x = 0;
    int box_y = 0;

    fill_rect_black(img, width, height, stride, box_x, box_y, box_w, box_h, fmt);

    int x = box_x + kPad;
    int y = box_y + kPad;
    for (char ch : text) {
        draw_glyph(img, width, height, stride, x, y, fmt, ch, kScale);
        x += cell;
    }
}
