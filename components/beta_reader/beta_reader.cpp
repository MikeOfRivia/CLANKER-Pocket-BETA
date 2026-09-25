#include "beta_reader.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>

#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
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

constexpr int kCharsPerLine = 34;
constexpr int kLinesPerPage = 21;
constexpr size_t kMaxBookTextBytes = 4 * 1024 * 1024;

sdmmc_card_t* s_card = nullptr;
bool s_ready = false;
esp_err_t s_last_error = ESP_OK;
std::vector<Book> s_books;
Book s_current;
std::string s_book_text;
std::vector<size_t> s_page_offsets;
int s_page = 0;

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
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

std::string JoinZipPath(const std::string& base, const std::string& relative)
{
    if (relative.empty()) return relative;
    if (relative.front() == '/') return relative.substr(1);
    if (base.empty()) return relative;
    return base + relative;
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

std::string StripHtml(const std::string& html)
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
            const size_t end = html.find(';', i + 1);
            if (end != std::string::npos && end - i <= 12) {
                AppendEntityDecoded(&out, html.substr(i + 1, end - i - 1));
                i = end + 1;
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
    mz_zip_archive zip = {};
    if (!mz_zip_reader_init_file(&zip, path.c_str(), 0)) {
        ESP_LOGW(kTag, "Unable to open EPUB zip: %s", path.c_str());
        return false;
    }

    std::string container;
    if (!ZipExtractText(&zip, "META-INF/container.xml", &container)) {
        mz_zip_reader_end(&zip);
        ESP_LOGW(kTag, "EPUB missing META-INF/container.xml");
        return false;
    }

    std::string opf_path;
    const size_t rootfile = container.find("<rootfile");
    if (rootfile != std::string::npos) {
        const size_t close = container.find('>', rootfile);
        if (close != std::string::npos) {
            opf_path = XmlAttr(container.substr(rootfile, close - rootfile + 1), "full-path");
        }
    }
    if (opf_path.empty()) {
        mz_zip_reader_end(&zip);
        ESP_LOGW(kTag, "EPUB container did not identify package document");
        return false;
    }

    std::string opf;
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

    out->clear();
    out->reserve(256 * 1024);
    for (const std::string& chapter_path : spine) {
        std::string chapter;
        if (!ZipExtractText(&zip, chapter_path, &chapter)) continue;
        chapter = StripHtml(chapter);
        if (chapter.empty()) continue;

        if (!out->empty() && out->back() != '\n') out->push_back('\n');
        out->append(chapter);
        out->append("\n\n");

        if (out->size() > kMaxBookTextBytes) {
            out->resize(kMaxBookTextBytes);
            break;
        }
    }

    mz_zip_reader_end(&zip);
    return !out->empty();
}

void BuildPageIndex()
{
    s_page_offsets.clear();
    s_page_offsets.push_back(0);

    int col = 0;
    int line = 0;
    for (size_t i = 0; i < s_book_text.size(); ++i) {
        const char ch = s_book_text[i];
        if (ch == '\r') continue;
        if (ch == '\n') {
            ++line;
            col = 0;
        } else {
            ++col;
            if (col >= kCharsPerLine) {
                ++line;
                col = 0;
            }
        }

        if (line >= kLinesPerPage && i + 1 < s_book_text.size()) {
            s_page_offsets.push_back(i + 1);
            line = 0;
            col = 0;
        }
    }
}

std::vector<std::string> PageLines(size_t start, size_t end)
{
    std::vector<std::string> lines;
    std::string line;
    line.reserve(kCharsPerLine);

    for (size_t i = start; i < end && lines.size() < kLinesPerPage; ++i) {
        const char ch = s_book_text[i];
        if (ch == '\r') continue;
        if (ch == '\n') {
            lines.push_back(line);
            line.clear();
            continue;
        }
        line.push_back(ch);
        if (static_cast<int>(line.size()) >= kCharsPerLine) {
            lines.push_back(line);
            line.clear();
        }
    }

    if (!line.empty() && lines.size() < kLinesPerPage) lines.push_back(line);
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
    if (!s_ready) return false;

    auto scan = [](const char* folder) {
        DIR* dir = opendir(folder);
        if (!dir) return;

        while (dirent* entry = readdir(dir)) {
            if (entry->d_name[0] == '.') continue;
            const std::string name(entry->d_name);
            if (!IsSupportedBook(name)) continue;

            Book book;
            book.path = std::string(folder) + "/" + name;
            book.name = DisplayNameFromPath(name);

            struct stat st = {};
            if (stat(book.path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                book.size_bytes = static_cast<uint32_t>(st.st_size);

                const bool duplicate = std::any_of(
                    s_books.begin(), s_books.end(),
                    [&](const Book& existing) { return existing.path == book.path; });
                if (!duplicate) s_books.push_back(book);
            }
        }
        closedir(dir);
    };

    scan(kMount);
    scan(kBooksDir);

    std::sort(s_books.begin(), s_books.end(), [](const Book& a, const Book& b) {
        return Lower(a.name) < Lower(b.name);
    });

    ESP_LOGI(kTag, "Library contains %u books",
             static_cast<unsigned>(s_books.size()));
    return true;
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

    struct stat st = {};
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;

    std::string text;
    const bool epub = EndsWith(path, ".epub");
    const bool ok = epub ? ReadEpub(path, &text) : ReadTxt(path, &text);
    if (!ok || text.empty()) return false;

    Book book;
    book.path = path;
    book.name = DisplayNameFromPath(path);
    book.size_bytes = static_cast<uint32_t>(st.st_size);

    s_current = book;
    s_book_text = std::move(text);
    s_page = 0;
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

}  // namespace beta_reader
