#ifndef BETA_READER_H_
#define BETA_READER_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "esp_err.h"

namespace beta_reader {

struct Book {
    std::string name;
    std::string path;
    uint32_t size_bytes = 0;
};

struct PageImage {
    int width = 0;
    int height = 0;
    // Packed 1-bit pixels, row-major, MSB first. A set bit means black ink.
    std::vector<uint8_t> bitmap;
};

struct PageItem {
    bool image = false;
    // Text for a text row, or the EPUB-internal image path for an image block.
    std::string content;
    int image_width = 0;
    int image_height = 0;
    int line_units = 1;
};

esp_err_t Init();
bool Ready();
esp_err_t LastError();
bool RefreshLibrary();
int ScannedFileCount();
const std::vector<Book>& Books();

bool OpenBook(size_t index);
bool OpenPath(const std::string& path);
void CloseBook();
bool HasOpenBook();

const std::string& CurrentName();
const std::string& CurrentPath();
int PageCount();
int CurrentPage();
void SetPage(int page);
void Repaginate();
std::vector<std::string> CurrentPageLines();
std::vector<PageItem> CurrentPageItems();
const PageImage* LoadPageImage(const std::string& image_path);

}  // namespace beta_reader

#endif
