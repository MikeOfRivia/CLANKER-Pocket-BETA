#include "beta_reader.h"
#include "reader_font.h"
#include "stb_image.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <functional>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <utility>
#include <vector>

#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "full_miniz.h"
#include "sdmmc_cmd.h"

namespace beta_reader {
namespace {

constexpr const char* kTag = "BetaReader";
constexpr const char* kMount = "/sdcard";
constexpr const char* kBooksDir = "/sdcard/books";

constexpr int kSdD0 = 15;
constexpr int kSdD1 = 7;
constexpr int kSdD2 = 8;
constexpr int kSdD3 = 18;
constexpr int kSdClk = 16;
constexpr int kSdCmd = 17;

constexpr size_t kMaxBookTextBytes = 4 * 1024 * 1024;
constexpr size_t kMaxEpubImageBytes = 16 * 1024 * 1024;
constexpr int kReaderImageMaxWidth = 424;
constexpr int kReaderImageMaxHeight = 630;
constexpr char kPageBreakMarker = '\f';
constexpr char kImageMarkerStart = 0x1e;
constexpr char kImageMarkerEnd = 0x1f;
constexpr const char* kImageMarkerLabel = "IMG:";

sdmmc_card_t* s_card = nullptr;
bool s_ready = false;
esp_err_t s_last_error = ESP_OK;
std::vector<Book> s_books;
int s_scanned_file_count = 0;
Book s_current;
std::string s_book_text;
std::vector<size_t> s_page_offsets;
int s_page = 0;
PageImage s_image_cache;
std::string s_image_cache_path;
std::unordered_map<std::string, std::pair<int, int>> s_image_dimensions;

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::string NormalizeBookKey(const std::string& name)
{
    std::string key;
    key.reserve(name.size());

    bool pending_space = false;
    for (unsigned char ch : name) {
        if (std::isspace(ch)) {
            pending_space = !key.empty();
            continue;
        }

        if (pending_space) {
            key.push_back(' ');
            pending_space = false;
        }
        key.push_back(static_cast<char>(std::tolower(ch)));
    }

    return key;
}

void LogMemory(const char* stage)
{
    const size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t internal_largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t psram_largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    ESP_LOGI(kTag,
             "MEM %s internal=%u largest=%u psram=%u largest=%u",
             stage ? stage : "?",
             static_cast<unsigned>(internal_free),
             static_cast<unsigned>(internal_largest),
             static_cast<unsigned>(psram_free),
             static_cast<unsigned>(psram_largest));
}

bool EndsWith(const std::string& value, const std::string& suffix)
{
    if (value.size() < suffix.size()) return false;
    return Lower(value.substr(value.size() - suffix.size())) == Lower(suffix);
}

bool IsSupportedBook(const std::string& name)
{
    return EndsWith(name, ".txt") || EndsWith(name, ".epub");
}

std::string DisplayNameFromPath(const std::string& path)
{
    const size_t slash = path.find_last_of('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (EndsWith(name, ".txt")) name.resize(name.size() - 4);
    else if (EndsWith(name, ".epub")) name.resize(name.size() - 5);
    return name;
}

std::string DirName(const std::string& path)
{
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

std::string PercentDecodePath(const std::string& value)
{
    auto hex = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };

    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const int hi = hex(value[i + 1]);
            const int lo = hex(value[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(value[i]);
    }
    return out;
}

std::string JoinZipPath(const std::string& base, const std::string& relative)
{
    if (relative.empty()) return {};

    std::string clean = relative;
    const size_t cut = clean.find_first_of("?#");
    if (cut != std::string::npos) clean.resize(cut);
    clean = PercentDecodePath(clean);
    if (clean.empty()) return {};

    std::string combined;
    if (clean.front() == '/') combined = clean.substr(1);
    else combined = base + clean;

    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= combined.size()) {
        const size_t slash = combined.find('/', start);
        const size_t end = slash == std::string::npos ? combined.size() : slash;
        const std::string part = combined.substr(start, end - start);
        if (part.empty() || part == ".") {
            // no-op
        } else if (part == "..") {
            if (!parts.empty()) parts.pop_back();
        } else {
            parts.push_back(part);
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }

    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out.push_back('/');
        out.append(parts[i]);
    }
    return out;
}

std::string XmlAttr(const std::string& tag, const char* attr)
{
    const std::string needle = std::string(attr) + "=";
    size_t p = tag.find(needle);
    if (p == std::string::npos) return {};
    p += needle.size();
    while (p < tag.size() && std::isspace(static_cast<unsigned char>(tag[p]))) ++p;
    if (p >= tag.size() || (tag[p] != '"' && tag[p] != '\'')) return {};
    const char quote = tag[p++];
    const size_t end = tag.find(quote, p);
    if (end == std::string::npos) return {};
    return tag.substr(p, end - p);
}

std::string XmlAttrInsensitive(const std::string& tag, const char* attr)
{
    const std::string low = Lower(tag);
    const std::string name = Lower(attr);
    size_t p = 0;
    while ((p = low.find(name, p)) != std::string::npos) {
        const bool left_ok =
            p == 0 || std::isspace(static_cast<unsigned char>(low[p - 1]));
        size_t q = p + name.size();
        while (q < low.size() && std::isspace(static_cast<unsigned char>(low[q]))) ++q;
        if (!left_ok || q >= low.size() || low[q] != '=') {
            p += name.size();
            continue;
        }
        ++q;
        while (q < tag.size() && std::isspace(static_cast<unsigned char>(tag[q]))) ++q;
        if (q >= tag.size() || (tag[q] != '"' && tag[q] != '\'')) return {};
        const char quote = tag[q++];
        const size_t end = tag.find(quote, q);
        if (end == std::string::npos) return {};
        return tag.substr(q, end - q);
    }
    return {};
}

bool ZipExtractText(mz_zip_archive* zip, const std::string& name, std::string* out)
{
    if (!zip || !out) return false;
    const int index = mz_zip_reader_locate_file(zip, name.c_str(), nullptr, 0);
    if (index < 0) return false;

    size_t size = 0;
    void* data = mz_zip_reader_extract_to_heap(zip, static_cast<mz_uint>(index), &size, 0);
    if (!data) return false;
    if (size > kMaxBookTextBytes) {
        mz_free(data);
        return false;
    }

    out->assign(static_cast<const char*>(data), size);
    mz_free(data);
    return true;
}

void AppendEntityDecoded(std::string* out, const std::string& entity)
{
    if (entity == "amp") out->push_back('&');
    else if (entity == "lt") out->push_back('<');
    else if (entity == "gt") out->push_back('>');
    else if (entity == "quot") out->push_back('"');
    else if (entity == "apos" || entity == "#39") out->push_back('\'');
    else if (entity == "nbsp" || entity == "#160") out->push_back(' ');
    else out->push_back(' ');
}

std::string NormalizeUtf8Punctuation(const std::string& input)
{
    std::string out;
    out.reserve(input.size());

    for (size_t i = 0; i < input.size();) {
        const unsigned char c = static_cast<unsigned char>(input[i]);
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
            ++i;
            continue;
        }

        if (i + 2 < input.size() &&
            static_cast<unsigned char>(input[i]) == 0xE2 &&
            static_cast<unsigned char>(input[i + 1]) == 0x80) {
            const unsigned char t = static_cast<unsigned char>(input[i + 2]);
            if (t == 0x98 || t == 0x99) out.push_back('\'');
            else if (t == 0x9C || t == 0x9D) out.push_back('"');
            else if (t == 0x93 || t == 0x94) out.push_back('-');
            else if (t == 0xA6) out.append("...");
            else out.push_back(' ');
            i += 3;
            continue;
        }

        // The current Pocket display font is ASCII. Preserve word separation for
        // characters we cannot render yet.
        out.push_back(' ');
        ++i;
        while (i < input.size() &&
               (static_cast<unsigned char>(input[i]) & 0xC0) == 0x80) {
            ++i;
        }
    }
    return out;
}

void AppendImageMarker(std::string* out, const std::string& chapter_path,
                       const std::string& src)
{
    if (!out || src.empty()) return;

    const std::string path = JoinZipPath(DirName(chapter_path), src);
    const std::string low = Lower(path);
    if (!(EndsWith(low, ".png") || EndsWith(low, ".jpg") ||
          EndsWith(low, ".jpeg"))) {
        return;
    }

    if (!out->empty() && out->back() != '\n') out->push_back('\n');
    out->push_back(kImageMarkerStart);
    out->append(kImageMarkerLabel);
    out->append(path);
    out->push_back(kImageMarkerEnd);
    out->push_back('\n');
}

std::string StripHtml(const std::string& html, const std::string& chapter_path)
{
    std::string out;
    out.reserve(html.size());

    bool in_tag = false;
    std::string tag;
    for (size_t i = 0; i < html.size();) {
        const char ch = html[i];

        if (!in_tag && ch == '<') {
            in_tag = true;
            tag.clear();
            ++i;
            continue;
        }

        if (in_tag) {
            if (ch == '>') {
                const std::string low = Lower(tag);

                if (low.rfind("img", 0) == 0) {
                    AppendImageMarker(&out, chapter_path,
                                      XmlAttrInsensitive(tag, "src"));
                } else if (low.rfind("image", 0) == 0) {
                    std::string href = XmlAttrInsensitive(tag, "href");
                    if (href.empty()) {
                        href = XmlAttrInsensitive(tag, "xlink:href");
                    }
                    AppendImageMarker(&out, chapter_path, href);
                }

                if (low.rfind("br", 0) == 0 || low.rfind("/p", 0) == 0 ||
                    low.rfind("/div", 0) == 0 || low.rfind("/li", 0) == 0 ||
                    low.rfind("/h1", 0) == 0 || low.rfind("/h2", 0) == 0 ||
                    low.rfind("/h3", 0) == 0 || low.rfind("/blockquote", 0) == 0) {
                    if (out.empty() || out.back() != '\n') out.push_back('\n');
                }
                in_tag = false;
            } else {
                tag.push_back(ch);
            }
            ++i;
            continue;
        }

        if (ch == '&') {
            const size_t entity_end = html.find(';', i + 1);
            if (entity_end != std::string::npos && entity_end - i <= 12) {
                AppendEntityDecoded(
                    &out, html.substr(i + 1, entity_end - i - 1));
                i = entity_end + 1;
                continue;
            }
        }

        if (ch == '\r') {
            ++i;
            continue;
        }

        out.push_back(ch);
        ++i;
    }

    out = NormalizeUtf8Punctuation(out);

    std::string clean;
    clean.reserve(out.size());
    bool last_space = false;
    int blank_lines = 0;
    for (char ch : out) {
        if (ch == '\n') {
            while (!clean.empty() && clean.back() == ' ') clean.pop_back();
            if (!clean.empty() && clean.back() != '\n') {
                clean.push_back('\n');
                blank_lines = 1;
            } else if (!clean.empty() && blank_lines < 2) {
                clean.push_back('\n');
                ++blank_lines;
            }
            last_space = false;
        } else if (std::isspace(static_cast<unsigned char>(ch))) {
            if (!last_space && !clean.empty() && clean.back() != '\n') {
                clean.push_back(' ');
                last_space = true;
            }
        } else {
            clean.push_back(ch);
            last_space = false;
            blank_lines = 0;
        }
    }
    return clean;
}

bool StartsChapterSection(const std::string& text)
{
    size_t pos = 0;
    int lines_checked = 0;

    while (pos < text.size() && lines_checked < 5) {
        const size_t end = text.find('\n', pos);
        const size_t line_end = end == std::string::npos ? text.size() : end;
        std::string line = text.substr(pos, line_end - pos);

        while (!line.empty() &&
               std::isspace(static_cast<unsigned char>(line.front()))) {
            line.erase(line.begin());
        }
        while (!line.empty() &&
               std::isspace(static_cast<unsigned char>(line.back()))) {
            line.pop_back();
        }

        if (!line.empty() && line.front() != kImageMarkerStart) {
            ++lines_checked;
            const std::string low = Lower(line);
            if (low == "chapter" || low.rfind("chapter ", 0) == 0 ||
                low == "prologue" || low == "epilogue" ||
                low.rfind("part ", 0) == 0 || low.rfind("book ", 0) == 0) {
                return true;
            }
        }

        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return false;
}


bool ReadTxt(const std::string& path, std::string* out)
{
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return false;

    std::fseek(fp, 0, SEEK_END);
    const long size = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (size < 0 || static_cast<size_t>(size) > kMaxBookTextBytes) {
        std::fclose(fp);
        return false;
    }

    out->resize(static_cast<size_t>(size));
    const size_t got = std::fread(out->data(), 1, out->size(), fp);
    std::fclose(fp);
    out->resize(got);
    *out = NormalizeUtf8Punctuation(*out);
    return true;
}

bool ReadEpub(const std::string& path, std::string* out)
{
    ESP_LOGI(kTag, "EPUB open begin: %s", path.c_str());
    LogMemory("before zip open");

    mz_zip_archive zip = {};
    if (!mz_zip_reader_init_file(&zip, path.c_str(), 0)) {
        ESP_LOGW(kTag, "Unable to open EPUB zip: %s", path.c_str());
        return false;
    }

    ESP_LOGI(kTag, "EPUB zip open OK");
    LogMemory("after zip open");

    std::string container;
    ESP_LOGI(kTag, "EPUB extracting container.xml");
    if (!ZipExtractText(&zip, "META-INF/container.xml", &container)) {
        mz_zip_reader_end(&zip);
        ESP_LOGW(kTag, "EPUB missing META-INF/container.xml");
        return false;
    }

    std::string opf_path;

    // Be tolerant of container.xml variations. The EPUB spec normally uses
    // <rootfile full-path="...">, but real-world files may vary in tag case,
    // namespace prefixing, or formatting.
    const std::string container_lower = Lower(container);
    size_t rootfile_name = container_lower.find("rootfile");
    if (rootfile_name != std::string::npos) {
        const size_t tag_start = container_lower.rfind('<', rootfile_name);
        const size_t tag_end = container_lower.find('>', rootfile_name);
        if (tag_start != std::string::npos && tag_end != std::string::npos &&
            tag_end > tag_start) {
            const std::string tag =
                container.substr(tag_start, tag_end - tag_start + 1);
            opf_path = XmlAttr(tag, "full-path");

            // Attribute names are XML case-sensitive, but accept common
            // non-conforming capitalization rather than rejecting the book.
            if (opf_path.empty()) {
                const std::string tag_lower = Lower(tag);
                const size_t attr_pos = tag_lower.find("full-path");
                if (attr_pos != std::string::npos) {
                    const size_t eq = tag.find('=', attr_pos);
                    if (eq != std::string::npos) {
                        size_t q = eq + 1;
                        while (q < tag.size() &&
                               std::isspace(static_cast<unsigned char>(tag[q]))) {
                            ++q;
                        }
                        if (q < tag.size() && (tag[q] == '"' || tag[q] == '\'')) {
                            const char quote = tag[q++];
                            const size_t end = tag.find(quote, q);
                            if (end != std::string::npos) {
                                opf_path = tag.substr(q, end - q);
                            }
                        }
                    }
                }
            }
        }
    }

    if (opf_path.empty()) {
        // Last-resort compatibility path: scan the archive for a package
        // document instead of crashing/rejecting an otherwise readable EPUB.
        const mz_uint count = mz_zip_reader_get_num_files(&zip);
        for (mz_uint i = 0; i < count; ++i) {
            mz_zip_archive_file_stat st = {};
            if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
            const std::string name = st.m_filename;
            if (EndsWith(name, ".opf")) {
                opf_path = name;
                ESP_LOGW(kTag,
                         "container.xml package lookup failed; using OPF fallback: %s",
                         opf_path.c_str());
                break;
            }
        }
    }

    if (opf_path.empty()) {
        mz_zip_reader_end(&zip);
        ESP_LOGW(kTag, "EPUB contains no discoverable package document");
        return false;
    }

    ESP_LOGI(kTag, "EPUB package path: %s", opf_path.c_str());

    std::string opf;
    ESP_LOGI(kTag, "EPUB extracting package");
    if (!ZipExtractText(&zip, opf_path, &opf)) {
        mz_zip_reader_end(&zip);
        ESP_LOGW(kTag, "Could not extract EPUB package: %s", opf_path.c_str());
        return false;
    }

    const std::string base = DirName(opf_path);
    std::unordered_map<std::string, std::string> manifest;

    size_t p = 0;
    while ((p = opf.find("<item", p)) != std::string::npos) {
        const size_t end = opf.find('>', p);
        if (end == std::string::npos) break;
        const std::string tag = opf.substr(p, end - p + 1);
        const std::string id = XmlAttr(tag, "id");
        const std::string href = XmlAttr(tag, "href");
        const std::string media = Lower(XmlAttr(tag, "media-type"));
        if (!id.empty() && !href.empty() &&
            (media.find("xhtml") != std::string::npos ||
             EndsWith(href, ".html") || EndsWith(href, ".htm") || EndsWith(href, ".xhtml"))) {
            manifest[id] = JoinZipPath(base, href);
        }
        p = end + 1;
    }

    std::vector<std::string> spine;
    p = 0;
    while ((p = opf.find("<itemref", p)) != std::string::npos) {
        const size_t end = opf.find('>', p);
        if (end == std::string::npos) break;
        const std::string idref = XmlAttr(opf.substr(p, end - p + 1), "idref");
        const auto it = manifest.find(idref);
        if (it != manifest.end()) spine.push_back(it->second);
        p = end + 1;
    }

    if (spine.empty()) {
        // Fallback for odd but simple EPUBs: use HTML/XHTML files in archive order.
        const mz_uint count = mz_zip_reader_get_num_files(&zip);
        for (mz_uint i = 0; i < count; ++i) {
            mz_zip_archive_file_stat st = {};
            if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
            const std::string name = st.m_filename;
            if (EndsWith(name, ".html") || EndsWith(name, ".htm") || EndsWith(name, ".xhtml")) {
                spine.push_back(name);
            }
        }
    }

    ESP_LOGI(kTag, "EPUB spine entries: %u",
             static_cast<unsigned>(spine.size()));
    LogMemory("before chapter loop");

    out->clear();
    out->reserve(256 * 1024);
    for (size_t chapter_index = 0; chapter_index < spine.size(); ++chapter_index) {
        const std::string& chapter_path = spine[chapter_index];
        ESP_LOGI(kTag, "EPUB chapter %u/%u extract: %s",
                 static_cast<unsigned>(chapter_index + 1),
                 static_cast<unsigned>(spine.size()),
                 chapter_path.c_str());

        std::string chapter;
        if (!ZipExtractText(&zip, chapter_path, &chapter)) {
            ESP_LOGW(kTag, "EPUB chapter extract failed: %s", chapter_path.c_str());
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        ESP_LOGI(kTag, "EPUB chapter compressed->text bytes: %u",
                 static_cast<unsigned>(chapter.size()));
        LogMemory("after chapter extract");

        chapter = StripHtml(chapter, chapter_path);
        ESP_LOGI(kTag, "EPUB chapter cleaned bytes: %u",
                 static_cast<unsigned>(chapter.size()));
        LogMemory("after chapter clean");

        if (!chapter.empty()) {
            if (!out->empty() && StartsChapterSection(chapter)) {
                while (!out->empty() &&
                       (out->back() == '\n' || out->back() == '\r' ||
                        out->back() == ' ')) {
                    out->pop_back();
                }
                out->push_back(kPageBreakMarker);
                ESP_LOGI(kTag, "Page break before chapter-like section: %s",
                         chapter_path.c_str());
            } else if (!out->empty() && out->back() != '\n') {
                out->push_back('\n');
            }

            out->append(chapter);
            out->append("\n\n");
        }

        ESP_LOGI(kTag, "EPUB accumulated bytes: %u",
                 static_cast<unsigned>(out->size()));
        LogMemory("after chapter append");

        if (out->size() > kMaxBookTextBytes) {
            ESP_LOGW(kTag, "EPUB text hit %u byte cap; truncating",
                     static_cast<unsigned>(kMaxBookTextBytes));
            out->resize(kMaxBookTextBytes);
            break;
        }

        // Give the system a scheduling point between potentially expensive
        // decompression/HTML-cleanup passes.
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    mz_zip_reader_end(&zip);
    ESP_LOGI(kTag, "EPUB read complete: %u text bytes",
             static_cast<unsigned>(out->size()));
    LogMemory("after epub read");
    return !out->empty();
}

bool ParseImageMarkerAt(size_t pos, std::string* path, size_t* next)
{
    if (pos >= s_book_text.size() || s_book_text[pos] != kImageMarkerStart) {
        return false;
    }

    const size_t label_start = pos + 1;
    const size_t label_len = std::strlen(kImageMarkerLabel);
    if (label_start + label_len > s_book_text.size() ||
        s_book_text.compare(label_start, label_len, kImageMarkerLabel) != 0) {
        return false;
    }

    const size_t path_start = label_start + label_len;
    const size_t end = s_book_text.find(kImageMarkerEnd, path_start);
    if (end == std::string::npos) return false;

    if (path) *path = s_book_text.substr(path_start, end - path_start);
    if (next) *next = end + 1;
    return true;
}

size_t SkipFlowWhitespace(size_t pos)
{
    while (pos < s_book_text.size()) {
        const char ch = s_book_text[pos];
        if (ch != ' ' && ch != '\n' && ch != '\r' && ch != '\t') break;
        ++pos;
    }
    return pos;
}

bool ExtractCurrentEpubFile(const std::string& zip_path, void** data, size_t* size)
{
    if (!data || !size || s_current.path.empty() ||
        !EndsWith(s_current.path, ".epub")) {
        return false;
    }

    *data = nullptr;
    *size = 0;

    mz_zip_archive zip = {};
    if (!mz_zip_reader_init_file(&zip, s_current.path.c_str(), 0)) {
        ESP_LOGW(kTag, "Image: unable to reopen EPUB");
        return false;
    }

    const int index =
        mz_zip_reader_locate_file(&zip, zip_path.c_str(), nullptr, 0);
    if (index < 0) {
        ESP_LOGW(kTag, "Image missing from EPUB: %s", zip_path.c_str());
        mz_zip_reader_end(&zip);
        return false;
    }

    void* extracted =
        mz_zip_reader_extract_to_heap(&zip, static_cast<mz_uint>(index), size, 0);
    mz_zip_reader_end(&zip);

    if (!extracted) return false;
    if (*size == 0 || *size > kMaxEpubImageBytes) {
        ESP_LOGW(kTag, "Image rejected (%u bytes): %s",
                 static_cast<unsigned>(*size), zip_path.c_str());
        mz_free(extracted);
        *size = 0;
        return false;
    }

    *data = extracted;
    return true;
}

std::pair<int, int> FitReaderImage(int source_width, int source_height)
{
    if (source_width <= 0 || source_height <= 0) return {0, 0};

    int width = source_width;
    int height = source_height;

    // Do not enlarge small illustrations; only shrink images that would exceed
    // the readable page area.
    if (width > kReaderImageMaxWidth) {
        height = std::max(
            1, static_cast<int>(
                   (static_cast<int64_t>(height) * kReaderImageMaxWidth) /
                   width));
        width = kReaderImageMaxWidth;
    }
    if (height > kReaderImageMaxHeight) {
        width = std::max(
            1, static_cast<int>(
                   (static_cast<int64_t>(width) * kReaderImageMaxHeight) /
                   height));
        height = kReaderImageMaxHeight;
    }
    return {width, height};
}

std::pair<int, int> ReaderImageDimensions(const std::string& image_path)
{
    const auto found = s_image_dimensions.find(image_path);
    if (found != s_image_dimensions.end()) return found->second;

    void* bytes = nullptr;
    size_t byte_count = 0;
    if (!ExtractCurrentEpubFile(image_path, &bytes, &byte_count)) return {0, 0};

    int source_width = 0;
    int source_height = 0;
    int channels = 0;
    const int ok = stbi_info_from_memory(
        static_cast<const stbi_uc*>(bytes), static_cast<int>(byte_count),
        &source_width, &source_height, &channels);
    mz_free(bytes);

    if (!ok) {
        ESP_LOGW(kTag, "Unsupported/corrupt EPUB image: %s", image_path.c_str());
        return {0, 0};
    }

    const auto fitted = FitReaderImage(source_width, source_height);
    s_image_dimensions[image_path] = fitted;
    ESP_LOGI(kTag, "Image layout %s: %dx%d -> %dx%d",
             image_path.c_str(), source_width, source_height,
             fitted.first, fitted.second);
    return fitted;
}

int ReaderImageLineUnits(const std::string& image_path)
{
    const auto dims = ReaderImageDimensions(image_path);
    if (dims.first <= 0 || dims.second <= 0) return 1;

    constexpr int kImageVerticalMargin = 10;
    const int block_height = dims.second + kImageVerticalMargin;
    return std::clamp(
        (block_height + kReaderLineHeightPx - 1) / kReaderLineHeightPx,
        1, kReaderLinesPerPage);
}

bool DecodeReaderImage(const std::string& image_path, PageImage* out)
{
    if (!out) return false;
    *out = {};

    const auto dims = ReaderImageDimensions(image_path);
    if (dims.first <= 0 || dims.second <= 0) return false;

    void* bytes = nullptr;
    size_t byte_count = 0;
    if (!ExtractCurrentEpubFile(image_path, &bytes, &byte_count)) return false;

    int source_width = 0;
    int source_height = 0;
    int channels = 0;
    stbi_uc* rgba = stbi_load_from_memory(
        static_cast<const stbi_uc*>(bytes), static_cast<int>(byte_count),
        &source_width, &source_height, &channels, 4);
    mz_free(bytes);

    if (!rgba || source_width <= 0 || source_height <= 0) {
        if (rgba) stbi_image_free(rgba);
        ESP_LOGW(kTag, "Image decode failed: %s", image_path.c_str());
        return false;
    }

    out->width = dims.first;
    out->height = dims.second;
    const size_t pixel_count =
        static_cast<size_t>(out->width) * static_cast<size_t>(out->height);
    out->bitmap.assign((pixel_count + 7) / 8, 0);

    static constexpr uint8_t kBayer4[4][4] = {
        { 0,  8,  2, 10},
        {12,  4, 14,  6},
        { 3, 11,  1,  9},
        {15,  7, 13,  5},
    };

    for (int y = 0; y < out->height; ++y) {
        const int sy = std::min(
            source_height - 1,
            static_cast<int>(
                (static_cast<int64_t>(y) * source_height) / out->height));
        for (int x = 0; x < out->width; ++x) {
            const int sx = std::min(
                source_width - 1,
                static_cast<int>(
                    (static_cast<int64_t>(x) * source_width) / out->width));
            const size_t src =
                (static_cast<size_t>(sy) * source_width + sx) * 4;

            const int r = rgba[src + 0];
            const int g = rgba[src + 1];
            const int b = rgba[src + 2];
            const int a = rgba[src + 3];
            int gray = (77 * r + 150 * g + 29 * b) >> 8;
            gray = (gray * a + 255 * (255 - a)) / 255;

            const int threshold =
                static_cast<int>(kBayer4[y & 3][x & 3]) * 16 + 8;
            if (gray < threshold) {
                const size_t bit =
                    static_cast<size_t>(y) * out->width + x;
                out->bitmap[bit >> 3] |=
                    static_cast<uint8_t>(0x80u >> (bit & 7));
            }
        }
    }

    stbi_image_free(rgba);
    return true;
}

size_t BuildWrappedLine(size_t start, std::string* line)
{
    line->clear();
    const size_t size = s_book_text.size();
    if (start >= size) return size;

    size_t pos = start;
    while (pos < size && s_book_text[pos] == '\r') ++pos;
    if (pos >= size) return size;

    // A paragraph break becomes a real blank line. This gives prose breathing
    // room instead of crushing every paragraph into a continuous wall of text.
    if (s_book_text[pos] == '\n') return pos + 1;

    while (pos < size && s_book_text[pos] == ' ') ++pos;

    int line_width = 0;
    while (pos < size && s_book_text[pos] != '\n' &&
           s_book_text[pos] != kPageBreakMarker &&
           s_book_text[pos] != kImageMarkerStart) {
        const size_t word_start = pos;
        int word_width = 0;
        while (pos < size && s_book_text[pos] != ' ' &&
               s_book_text[pos] != '\n' && s_book_text[pos] != '\r' &&
               s_book_text[pos] != kPageBreakMarker &&
               s_book_text[pos] != kImageMarkerStart) {
            word_width += ReaderFontAdvance(s_book_text[pos]);
            ++pos;
        }
        const size_t word_end = pos;

        const int gap = line->empty() ? 0 : ReaderFontAdvance(' ');
        if (line_width + gap + word_width <= kReaderTextWidthPx) {
            if (!line->empty()) {
                line->push_back(' ');
                line_width += gap;
            }
            line->append(s_book_text, word_start, word_end - word_start);
            line_width += word_width;
        } else if (!line->empty()) {
            return word_start;
        } else {
            // A single unbroken token wider than the page gets split at the
            // last glyph that fits, rather than overflowing the framebuffer.
            size_t cut = word_start;
            while (cut < word_end) {
                const int advance = ReaderFontAdvance(s_book_text[cut]);
                if (cut > word_start && line_width + advance > kReaderTextWidthPx) break;
                line->push_back(s_book_text[cut]);
                line_width += advance;
                ++cut;
            }
            return cut > word_start ? cut : word_start + 1;
        }

        while (pos < size && s_book_text[pos] == ' ') ++pos;
        while (pos < size && s_book_text[pos] == '\r') ++pos;
    }

    // Deliberately leave a trailing newline for the next call so it renders as
    // a blank paragraph spacer line.
    return pos;
}

void BuildPageIndex()
{
    ESP_LOGI(kTag, "Pagination begin: %u text bytes",
             static_cast<unsigned>(s_book_text.size()));
    LogMemory("before pagination");

    s_page_offsets.clear();
    s_page_offsets.push_back(0);

    size_t pos = 0;
    int line_count = 0;
    std::string line;
    line.reserve(64);

    auto add_page_start = [&](size_t offset) {
        if (offset >= s_book_text.size()) return;
        if (s_page_offsets.empty() || s_page_offsets.back() != offset) {
            s_page_offsets.push_back(offset);
        }
    };

    while (pos < s_book_text.size()) {
        if (s_book_text[pos] == kPageBreakMarker) {
            const size_t next = SkipFlowWhitespace(pos + 1);
            if (next >= s_book_text.size()) {
                pos = next;
                break;
            }

            // A chapter marker means the following content starts a fresh page.
            // If the marker itself is already the page start, just move that
            // start past the control byte rather than creating a blank page.
            if (line_count == 0 && !s_page_offsets.empty() &&
                s_page_offsets.back() == pos) {
                s_page_offsets.back() = next;
            } else {
                add_page_start(next);
            }
            pos = next;
            line_count = 0;
            continue;
        }

        std::string image_path;
        size_t image_next = pos;
        if (ParseImageMarkerAt(pos, &image_path, &image_next)) {
            const int units = ReaderImageLineUnits(image_path);

            // Keep the entire illustration together. If it will not fit in
            // the remaining vertical space, start it at the top of a page.
            if (line_count > 0 && line_count + units > kReaderLinesPerPage) {
                add_page_start(pos);
                line_count = 0;
                continue;
            }

            pos = SkipFlowWhitespace(image_next);
            line_count += units;
            if (line_count >= kReaderLinesPerPage &&
                pos < s_book_text.size()) {
                add_page_start(pos);
                line_count = 0;
            }
            continue;
        }

        const size_t next = BuildWrappedLine(pos, &line);
        if (next <= pos) {
            ESP_LOGW(kTag, "Pagination made no progress at byte %u",
                     static_cast<unsigned>(pos));
            ++pos;
        } else {
            pos = next;
        }

        ++line_count;
        if (line_count >= kReaderLinesPerPage && pos < s_book_text.size()) {
            add_page_start(pos);
            line_count = 0;
        }
    }

    // A trailing control marker can move the sole page start to EOF.
    if (!s_page_offsets.empty() &&
        s_page_offsets.back() >= s_book_text.size() &&
        s_page_offsets.size() > 1) {
        s_page_offsets.pop_back();
    }

    ESP_LOGI(kTag, "Pagination complete: %u pages",
             static_cast<unsigned>(s_page_offsets.size()));
    LogMemory("after pagination");
}

std::vector<PageItem> PageItems(size_t start, size_t end)
{
    std::vector<PageItem> items;
    items.reserve(kReaderLinesPerPage);

    size_t pos = start;
    int used_units = 0;

    while (pos < end && used_units < kReaderLinesPerPage) {
        if (s_book_text[pos] == kPageBreakMarker) {
            pos = SkipFlowWhitespace(pos + 1);
            continue;
        }

        std::string image_path;
        size_t image_next = pos;
        if (ParseImageMarkerAt(pos, &image_path, &image_next)) {
            const auto dims = ReaderImageDimensions(image_path);
            const int units = ReaderImageLineUnits(image_path);

            PageItem item;
            item.image = true;
            item.content = image_path;
            item.image_width = dims.first;
            item.image_height = dims.second;
            item.line_units = units;
            items.push_back(std::move(item));

            used_units += units;
            pos = SkipFlowWhitespace(image_next);
            continue;
        }

        std::string line;
        const size_t next = BuildWrappedLine(pos, &line);
        if (next <= pos) break;

        PageItem item;
        item.image = false;
        item.content = std::move(line);
        item.line_units = 1;
        items.push_back(std::move(item));

        ++used_units;
        pos = next;
    }

    return items;
}

std::vector<std::string> PageLines(size_t start, size_t end)
{
    std::vector<std::string> lines;
    const auto items = PageItems(start, end);
    for (const auto& item : items) {
        if (!item.image) lines.push_back(item.content);
    }
    return lines;
}

}  // namespace

esp_err_t Init()
{
    if (s_ready) return ESP_OK;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 5;
    mount_config.allocation_unit_size = 32 * 1024;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    auto try_mount = [&](int width) -> esp_err_t {
        sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
        slot.width = width;
        slot.clk = static_cast<gpio_num_t>(kSdClk);
        slot.cmd = static_cast<gpio_num_t>(kSdCmd);
        slot.d0 = static_cast<gpio_num_t>(kSdD0);
        slot.d1 = static_cast<gpio_num_t>(kSdD1);
        slot.d2 = static_cast<gpio_num_t>(kSdD2);
        slot.d3 = static_cast<gpio_num_t>(kSdD3);
        return esp_vfs_fat_sdmmc_mount(kMount, &host, &slot, &mount_config, &s_card);
    };

    esp_err_t err = try_mount(4);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "4-bit SD mount failed: %s; retrying 1-bit",
                 esp_err_to_name(err));
        s_card = nullptr;
        err = try_mount(1);
    }

    s_last_error = err;
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "SD mount failed: %s", esp_err_to_name(err));
        s_ready = false;
        return err;
    }

    (void)mkdir(kBooksDir, 0775);
    s_ready = true;
    s_last_error = ESP_OK;
    ESP_LOGI(kTag, "SD mounted; book folder is %s", kBooksDir);
    RefreshLibrary();
    return ESP_OK;
}

bool Ready()
{
    return s_ready;
}

esp_err_t LastError()
{
    return s_last_error;
}

bool RefreshLibrary()
{
    s_books.clear();
    s_scanned_file_count = 0;
    if (!s_ready) return false;

    std::function<void(const std::string&, int)> scan =
        [&](const std::string& folder, int depth) {
            if (depth > 6) return;

            DIR* dir = opendir(folder.c_str());
            if (!dir) return;

            while (dirent* entry = readdir(dir)) {
                if (entry->d_name[0] == '.') continue;

                const std::string name(entry->d_name);
                const std::string path = folder + "/" + name;

                struct stat st = {};
                const bool stat_ok = stat(path.c_str(), &st) == 0;

                // Trust the filename extension for book discovery. Some FAT/exFAT
                // directory entries can have incomplete metadata even though fopen()
                // works perfectly.
                if (IsSupportedBook(name)) {
                    ++s_scanned_file_count;

                    Book book;
                    book.path = path;
                    book.name = DisplayNameFromPath(name);
                    book.size_bytes =
                        stat_ok ? static_cast<uint32_t>(st.st_size) : 0;

                    const std::string normalized_path = Lower(book.path);
                    const std::string normalized_name = NormalizeBookKey(book.name);
                    const auto duplicate = std::find_if(
                        s_books.begin(), s_books.end(),
                        [&](const Book& existing) {
                            return Lower(existing.path) == normalized_path ||
                                   NormalizeBookKey(existing.name) == normalized_name;
                        });

                    if (duplicate == s_books.end()) {
                        s_books.push_back(book);
                    } else {
                        ESP_LOGI(kTag,
                                 "Skipping duplicate library title '%s': %s (kept %s)",
                                 book.name.c_str(),
                                 book.path.c_str(),
                                 duplicate->path.c_str());
                    }
                    continue;
                }

                if (stat_ok && S_ISDIR(st.st_mode)) {
                    // Skip common metadata trash; recurse through ordinary folders.
                    const std::string low = Lower(name);
                    if (low == "system volume information" ||
                        low == "$recycle.bin" ||
                        low == "lost.dir") {
                        continue;
                    }
                    scan(path, depth + 1);
                    continue;
                }

                if (stat_ok && S_ISREG(st.st_mode)) {
                    ++s_scanned_file_count;
                }
            }

            closedir(dir);
        };

    scan(kMount, 0);
    // Always scan the known reader folder directly. Do not depend on FAT/exFAT
    // stat() metadata from the root directory to discover a folder we create.
    scan(kBooksDir, 0);

    std::sort(s_books.begin(), s_books.end(), [](const Book& a, const Book& b) {
        return Lower(a.name) < Lower(b.name);
    });

    ESP_LOGI(kTag, "Scanned %d files; library contains %u books",
             s_scanned_file_count, static_cast<unsigned>(s_books.size()));
    return true;
}

int ScannedFileCount()
{
    return s_scanned_file_count;
}

const std::vector<Book>& Books()
{
    return s_books;
}

bool OpenBook(size_t index)
{
    if (index >= s_books.size()) return false;
    return OpenPath(s_books[index].path);
}

bool OpenPath(const std::string& path)
{
    if (!s_ready || path.empty()) return false;

    ESP_LOGI(kTag, "OpenPath begin: %s", path.c_str());
    LogMemory("open begin");

    struct stat st = {};
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        ESP_LOGW(kTag, "OpenPath stat failed or not a regular file: %s", path.c_str());
        return false;
    }

    ESP_LOGI(kTag, "OpenPath file size: %u",
             static_cast<unsigned>(st.st_size));

    std::string text;
    const bool epub = EndsWith(path, ".epub");
    const bool ok = epub ? ReadEpub(path, &text) : ReadTxt(path, &text);
    if (!ok || text.empty()) {
        ESP_LOGW(kTag, "OpenPath reader failed: %s", path.c_str());
        LogMemory("open failed");
        return false;
    }

    Book book;
    book.path = path;
    book.name = DisplayNameFromPath(path);
    book.size_bytes = static_cast<uint32_t>(st.st_size);

    s_current = book;
    s_book_text = std::move(text);
    s_page = 0;
    s_image_dimensions.clear();
    s_image_cache = {};
    s_image_cache_path.clear();
    BuildPageIndex();

    ESP_LOGI(kTag, "Opened %s: %u source bytes, %u text bytes, %u pages",
             s_current.name.c_str(),
             static_cast<unsigned>(s_current.size_bytes),
             static_cast<unsigned>(s_book_text.size()),
             static_cast<unsigned>(s_page_offsets.size()));
    return !s_page_offsets.empty();
}

void CloseBook()
{
    s_current = {};
    s_book_text.clear();
    s_page_offsets.clear();
    s_page = 0;
    s_image_dimensions.clear();
    s_image_cache = {};
    s_image_cache_path.clear();
}

bool HasOpenBook()
{
    return !s_current.path.empty() && !s_page_offsets.empty();
}

const std::string& CurrentName()
{
    return s_current.name;
}

const std::string& CurrentPath()
{
    return s_current.path;
}

int PageCount()
{
    return static_cast<int>(s_page_offsets.size());
}

int CurrentPage()
{
    return s_page;
}

void SetPage(int page)
{
    if (s_page_offsets.empty()) {
        s_page = 0;
        return;
    }
    s_page = std::clamp(page, 0, static_cast<int>(s_page_offsets.size()) - 1);
}

std::vector<std::string> CurrentPageLines()
{
    if (!HasOpenBook()) return {};

    const size_t start = s_page_offsets[static_cast<size_t>(s_page)];
    const size_t end =
        (s_page + 1 < static_cast<int>(s_page_offsets.size()))
            ? s_page_offsets[static_cast<size_t>(s_page + 1)]
            : s_book_text.size();
    return PageLines(start, end);
}

std::vector<PageItem> CurrentPageItems()
{
    if (!HasOpenBook()) return {};

    const size_t start = s_page_offsets[static_cast<size_t>(s_page)];
    const size_t end =
        (s_page + 1 < static_cast<int>(s_page_offsets.size()))
            ? s_page_offsets[static_cast<size_t>(s_page + 1)]
            : s_book_text.size();
    return PageItems(start, end);
}

const PageImage* LoadPageImage(const std::string& image_path)
{
    if (image_path.empty() || !HasOpenBook()) return nullptr;

    if (s_image_cache_path == image_path &&
        s_image_cache.width > 0 && s_image_cache.height > 0 &&
        !s_image_cache.bitmap.empty()) {
        return &s_image_cache;
    }

    PageImage decoded;
    if (!DecodeReaderImage(image_path, &decoded)) {
        s_image_cache = {};
        s_image_cache_path.clear();
        return nullptr;
    }

    s_image_cache = std::move(decoded);
    s_image_cache_path = image_path;
    return &s_image_cache;
}

}  // namespace beta_reader
