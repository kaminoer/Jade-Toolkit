// Windows command-line adapter for CLIs whose public std::string contract is
// UTF-8. MinGW's narrow argv follows the active C locale and can replace
// characters before main() sees them, so recover the original UTF-16 command
// line and convert each argument explicitly.
#pragma once

#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace jade {

class Utf8Args {
public:
    Utf8Args(int& argc, char**& argv) : argc_(argc), argv_(argv) {
#ifdef _WIN32
        int count = 0;
        wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
        if (wide == nullptr) return;
        storage_.reserve(static_cast<size_t>(count));
        for (int index = 0; index < count; ++index) {
            const int size = WideCharToMultiByte(
                CP_UTF8, 0, wide[index], -1, nullptr, 0, nullptr, nullptr);
            if (size <= 0) {
                storage_.emplace_back();
                continue;
            }
            std::string value(static_cast<size_t>(size), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide[index], -1,
                                &value[0], size, nullptr, nullptr);
            value.resize(static_cast<size_t>(size - 1));
            storage_.push_back(std::move(value));
        }
        LocalFree(wide);
        pointers_.reserve(storage_.size());
        for (std::string& value : storage_) pointers_.push_back(value.data());
        argc_ = static_cast<int>(pointers_.size());
        argv_ = pointers_.data();
#endif
        // Replace main's local argv view for the lifetime of this adapter.
        // Merely retaining the recovered strings here is not sufficient:
        // callers conventionally continue to read their argc/argv variables.
        argc = argc_;
        argv = argv_;
    }

    int argc() const { return argc_; }
    char** argv() const { return argv_; }

private:
    int argc_;
    char** argv_;
    std::vector<std::string> storage_;
    std::vector<char*> pointers_;
};

}  // namespace jade
