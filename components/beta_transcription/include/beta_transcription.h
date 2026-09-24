#ifndef BETA_TRANSCRIPTION_H_
#define BETA_TRANSCRIPTION_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace beta_transcription {

struct Result {
    bool success = false;
    int http_status = 0;
    std::string transcript;
    std::string error_code;
    std::string error_message;
};

bool HasApiKey();
Result TranscribePcm16(const int16_t* samples, size_t sample_count, uint32_t sample_rate_hz);

}  // namespace beta_transcription

#endif
