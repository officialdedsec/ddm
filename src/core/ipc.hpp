// Local messages between the browser helper (or a second DDM.exe) and the running DDM: one JSON request, one JSON reply,
// over a named pipe that only this Windows user can open and that refuses connections from other computers.
#pragma once
#include "../common/json.hpp"
#include <functional>

namespace ddm {

std::wstring ipcPipeName();
void ipcServe(std::function<Json(const Json&)> handler);   // starts a background thread
void ipcStop();
bool ipcSend(const Json& request, Json& reply, int timeoutMs);

} // namespace ddm
