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

esp_err_t Init();
bool Ready();
bool RefreshLibrary();
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
std::vector<std::string> CurrentPageLines();

}  // namespace beta_reader

#endif
