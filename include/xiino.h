#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Firmware port of OpenXiino e9ef506. Upstream's colour-only image path and
// HTML parser behaviour are retained after bounded source compaction.
// Oversized pages are shortened; optional image failures use placeholders.
// See third_party/openxiino.
struct XiinoRequest {
    bool dataserver = false;
    std::string url, colour_depth, grayscale_depth, width, encoding;
};
XiinoRequest xiino_request(const std::string &target);
const std::string &xiino_padding();
std::string xiino_resolve(const std::string &base, const std::string &reference);

struct XiinoResource {
    std::string body, url, content_type;
    bool truncated = false;
};
// Bounded streaming source compaction, before retaining a modern page in RAM.
// Keep renderer attributes and text; discard scripts, styles and comments.
class XiinoSourceFilter {
public:
    XiinoSourceFilter(XiinoResource &page, size_t limit);
    void feed(const char *bytes, size_t size);
    void finish(bool source_limit = false);
    bool full() const { return page_.truncated; }
private:
    enum State { Text, Tag, Comment, Raw, RawEnd };
    XiinoResource &page_;
    size_t limit_, raw_match_ = 0;
    State state_ = Text;
    std::string token_, raw_;
    char quote_ = 0;
    unsigned comment_dashes_ = 0;
    bool oversized_ = false, whitespace_ = false;
    unsigned preformatted_ = 0;
    bool append(const std::string &text);
    void tag();
};
using XiinoFetch = std::function<bool(const std::string &, size_t, XiinoResource &, std::string &)>;
using XiinoSink = std::function<bool(const char *, size_t)>;
struct XiinoLimits {
    size_t page_bytes = 48000;
    size_t image_bytes = 64000;
    size_t image_pixels = 153 * 1024;
    std::function<void(const char *)> checkpoint;
};

// Produces HTML only; the HTTP adapter adds the binary DataServer padding.
bool xiino_convert(const XiinoResource &page, const XiinoFetch &fetch,
                   const XiinoLimits &limits, std::string &html, std::string &problem);
bool xiino_convert_stream(const XiinoResource &page,const XiinoFetch &fetch,
                          const XiinoLimits &limits,const XiinoSink &sink,std::string &problem);
std::string xiino_builtin(const XiinoRequest &request, const std::string &headers);

// Exact nearest OpenXiino palette colour, with the original tie ordering.
uint8_t xiino_palette_nearest(uint8_t r, uint8_t g, uint8_t b);

// Mode 9 uses palette indices, row lookbacks and run-length control codes.
std::string xiino_mode9(const std::vector<uint8_t> &pixels, unsigned width, unsigned height);
bool xiino_image(const std::string &file, unsigned slot, const XiinoLimits &limits,
                 std::string &img, std::string &ebd, std::string &problem);
std::string xiino_image_diagnostics();
#ifdef XIINO_TEST_HEAP
void xiino_image_test_heap(size_t available, size_t largest);
#endif
