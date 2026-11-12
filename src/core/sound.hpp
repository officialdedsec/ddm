// Sounds (Options → General → Sounds; off unless you turn them on): a sound when a download finishes, fails, when a
// queue or every download is done, and when the antivirus finds a threat. Each can be Windows' own sound or a .wav
// you choose. A chosen file is checked (a real, sane WAV of at most 10 MB, from a local drive) and copied into DDM's
// own folder, so playing it never reaches out to a network share or a file that changed afterwards.
#pragma once
#include <string>

namespace ddm {
namespace sound {

enum class Event { Complete, Failed, QueueDone, Threat };
const char* key(Event e);                      // "complete", "failed", "queue", "threat"

// Checks and copies a .wav for an event. false + error (in plain words) when it isn't usable.
bool import(Event e, const std::string& wavPath, std::string& error);
bool hasCustom(Event e);             // a chosen sound is in use, or waiting for Options' OK
// Options: a newly chosen sound waits beside the old one until OK (commit) or Cancel (discard).
void commit();
void discard();
// Plays the event's sound if sounds are on and the event isn't "off" (never blocks, never throws).
void play(Event e);
// Plays what a given choice would sound like ("windows" | "custom"), for the ▶ button in Options.
void preview(Event e, const std::string& mode);

} // namespace sound
} // namespace ddm
