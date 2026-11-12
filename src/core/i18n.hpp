// DDM in the user's own language. Every text DDM shows passes through tr() where it's drawn (labels, buttons, menus,
// window titles, message boxes, notifications), so the program's code keeps its English text and nothing else changes.
// Language files: "English => Translation" lines (res/lang/<code>.txt, built in), and the user's own in data\lang\.
// {1}, {2}… stand for the changing parts of a sentence (a file's name, a number) and may move in the translation.
#pragma once
#include <string>
#include <vector>

namespace ddm {
namespace i18n {

struct Language {
    std::string code;      // "bn", "pt-BR"
    std::string name;      // in its own language: "বাংলা"
    std::string english;   // "Bengali"
    bool rtl = false;      // Arabic, Urdu, Persian: right to left
};

std::vector<Language> languages();          // English first, then every language found (built in + data\lang)
std::string systemLanguage();               // Windows' display language, when DDM has it ("" = no)
void setLanguage(const std::string& code);  // "" or "en" = English
const std::string& current();
bool rtl();                                 // the current language is written right to left

// The text in the current language; unchanged when there's no translation (user data, file names, English).
std::string tr(const std::string& english);
// For names (files, sites, categories, titles): only one of DDM's own words as a whole ("Video", "Main download
// queue"), never part of one (a file "Video of the trip.mp4" stays as it is).
std::string trName(const std::string& s);

// Statistics for language files: how much of the program a language covers (tests / Options).
struct Coverage { int entries = 0; int templates = 0; };
Coverage coverage();

} // namespace i18n
} // namespace ddm
