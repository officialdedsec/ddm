// The download list: File name, Source (site icon + name), Size, Status, Time left, Transfer rate, Last try, Description.
#pragma once
#include "widgets.hpp"
#include "../core/model.hpp"
#include <functional>
#include <set>
#include <map>

namespace ddm {
namespace ui {

class DownloadList : public Wnd {
public:
    bool create(HWND parent);
    void setItems(std::vector<Task> items);     // keeps selection and scroll
    std::vector<std::string> selection() const;
    std::string focusedId() const;
    std::function<void(const std::string&)> onOpen;          // double-click / Enter
    std::function<void(POINT)> onContext;                     // right-click (screen point)
    std::function<void()> onDelete;                          // Del
    std::function<void()> onSelectionChanged;

    struct Column { std::string key, title; int width; bool right; };
    std::vector<Column> cols;
    std::string sortKey = "added";
    bool sortDesc = true;
    void saveLayout();
    const std::vector<Task>& items() const { return rows; }   // in display order
    void selectId(const std::string& id);                     // select one download and scroll to it
    // Removing downloads: the rows turn to dust (when it's on), vanish at once, and the rows below slide up into the
    // gap after a moment. Call it just before the downloads are removed.
    void dustRemove(const std::vector<std::string>& ids);
    void cancelDust() { vanishing.clear(); slideRows.clear(); if (h) KillTimer(h, 0x5D1); redraw(); }   // Undo: the rows are back

protected:
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override;

private:
    std::vector<Task> rows;
    std::set<std::string> selected;
    std::string anchor, focus;
    int scrollY = 0, rowH = 38, headH = 30;
    int hoverRow = -1, dragCol = -1, dragStartX = 0, dragStartW = 0;
    std::set<std::string> vanishing;                 // rows being removed: not drawn
    std::map<std::string, int> slideRows;             // rows below them: how many rows they still have to move up
    DWORD slideStart = 0;
    int slideHold = 0, slideMs = 300;
    float slideLeft() const;                          // 1 → 0 while the rows move up
    void sortRows();
    int rowAt(int y) const;
    int colEdgeAt(int x) const;
    int colAt(int x) const;
    void paint(HDC dc, RECT r);
    void paintRow(HDC dc, const Task& t, int y, int width, int index);
    void ensureVisible(int row);
    void updateScroll();
    int contentH() const { return static_cast<int>(rows.size()) * rowH; }
    void selectOnly(const std::string& id) { selected.clear(); selected.insert(id); anchor = focus = id; }
};

std::string statusText(const Task& t);
std::string iconFor(const Task& t);
COLORREF iconColorFor(const Task& t);

} // namespace ui
} // namespace ddm
