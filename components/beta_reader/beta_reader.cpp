#include "beta_reader.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
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

sdmmc_card_t* s_card = nullptr;
bool s_ready = false;
std::vector<Book> s_books;
Book s_current;
std::vector<long> s_page_offsets;
int s_page = 0;

bool EndsWithTxt(const std::string& name)
{
    if (name.size() < 4) return false;
    std::string ext = name.substr(name.size() - 4);
    for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return ext == ".txt";
}

std::string DisplayNameFromPath(const std::string& path)
{
    const size_t slash = path.find_last_of('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (EndsWithTxt(name)) name.resize(name.size() - 4);
    return name;
}

bool BuildPageIndex(const std::string& path)
{
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return false;

    s_page_offsets.clear();
    s_page_offsets.push_back(0);

    int col = 0;
    int line = 0;
    int c = 0;
    while ((c = std::fgetc(fp)) != EOF) {
        if (c == '\r') continue;

        if (c == '\n') {
            ++line;
            col = 0;
        } else {
            ++col;
            if (col >= kCharsPerLine) {
                ++line;
                col = 0;
            }
        }

        if (line >= kLinesPerPage) {
            const long next = std::ftell(fp);
            const int probe = std::fgetc(fp);
            if (probe != EOF) {
                s_page_offsets.push_back(next);
                std::fseek(fp, next, SEEK_SET);
            }
            line = 0;
            col = 0;
        }
    }

    std::fclose(fp);
    if (s_page_offsets.empty()) s_page_offsets.push_back(0);
    return true;
}

std::vector<std::string> ReadPage(long start, long end)
{
    std::vector<std::string> lines;
    FILE* fp = std::fopen(s_current.path.c_str(), "rb");
    if (!fp) return lines;

    std::fseek(fp, start, SEEK_SET);
    std::string line;
    int col = 0;

    while (static_cast<int>(lines.size()) < kLinesPerPage) {
        const long pos = std::ftell(fp);
        if (end >= 0 && pos >= end) break;

        const int c = std::fgetc(fp);
        if (c == EOF) break;
        if (c == '\r') continue;

        if (c == '\n') {
            lines.push_back(line);
            line.clear();
            col = 0;
            continue;
        }

        char out = ' ';
        if (c == '\t') {
            out = ' ';
        } else if (c >= 32 && c <= 126) {
            out = static_cast<char>(c);
        } else {
            // Current Pocket font is ASCII-only. Preserve spacing rather than render garbage.
            out = ' ';
        }

        line.push_back(out);
        ++col;
        if (col >= kCharsPerLine) {
            lines.push_back(line);
            line.clear();
            col = 0;
        }
    }

    if (!line.empty() && static_cast<int>(lines.size()) < kLinesPerPage) {
        lines.push_back(line);
    }

    std::fclose(fp);
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

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = static_cast<gpio_num_t>(kSdClk);
    slot.cmd = static_cast<gpio_num_t>(kSdCmd);
    slot.d0 = static_cast<gpio_num_t>(kSdD0);
    slot.d1 = static_cast<gpio_num_t>(kSdD1);
    slot.d2 = static_cast<gpio_num_t>(kSdD2);
    slot.d3 = static_cast<gpio_num_t>(kSdD3);

    const esp_err_t err =
        esp_vfs_fat_sdmmc_mount(kMount, &host, &slot, &mount_config, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "SD mount failed: %s", esp_err_to_name(err));
        s_ready = false;
        return err;
    }

    (void)mkdir(kBooksDir, 0775);
    s_ready = true;
    ESP_LOGI(kTag, "SD mounted; book folder is %s", kBooksDir);
    RefreshLibrary();
    return ESP_OK;
}

bool Ready()
{
    return s_ready;
}

bool RefreshLibrary()
{
    s_books.clear();
    if (!s_ready) return false;

    DIR* dir = opendir(kBooksDir);
    if (!dir) return false;

    while (dirent* entry = readdir(dir)) {
        if (!entry->d_name || entry->d_name[0] == '.') continue;
        const std::string name(entry->d_name);
        if (!EndsWithTxt(name)) continue;

        Book book;
        book.path = std::string(kBooksDir) + "/" + name;
        book.name = DisplayNameFromPath(name);

        struct stat st = {};
        if (stat(book.path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            book.size_bytes = static_cast<uint32_t>(st.st_size);
            s_books.push_back(book);
        }
    }
    closedir(dir);

    std::sort(s_books.begin(), s_books.end(), [](const Book& a, const Book& b) {
        std::string aa = a.name;
        std::string bb = b.name;
        std::transform(aa.begin(), aa.end(), aa.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::transform(bb.begin(), bb.end(), bb.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return aa < bb;
    });

    ESP_LOGI(kTag, "Library contains %u TXT books",
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

    Book book;
    book.path = path;
    book.name = DisplayNameFromPath(path);
    book.size_bytes = static_cast<uint32_t>(st.st_size);

    s_current = book;
    s_page = 0;
    if (!BuildPageIndex(path)) {
        s_current = {};
        s_page_offsets.clear();
        return false;
    }

    ESP_LOGI(kTag, "Opened %s: %u bytes, %u pages",
             s_current.name.c_str(),
             static_cast<unsigned>(s_current.size_bytes),
             static_cast<unsigned>(s_page_offsets.size()));
    return true;
}

void CloseBook()
{
    s_current = {};
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

    const long start = s_page_offsets[static_cast<size_t>(s_page)];
    const long end =
        (s_page + 1 < static_cast<int>(s_page_offsets.size()))
            ? s_page_offsets[static_cast<size_t>(s_page + 1)]
            : -1;
    return ReadPage(start, end);
}

}  // namespace beta_reader
