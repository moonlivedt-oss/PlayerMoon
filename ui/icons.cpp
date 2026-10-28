#include "ui/icons.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>

#include "view/svg_arc.h"

namespace ui {

namespace {

// Чтение чисел из пути SVG: "-.6", ".7.7" (это два числа), "1e-3", флаги дуг "01"
class PathReader {
public:
    explicit PathReader(std::string_view s) : s_(s) {}

    void SkipSeparators() {
        while (pos_ < s_.size() && (std::isspace(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == ',')) ++pos_;
    }
    bool AtEnd() {
        SkipSeparators();
        return pos_ >= s_.size();
    }
    bool AtCommand() {
        SkipSeparators();
        return pos_ < s_.size() && std::isalpha(static_cast<unsigned char>(s_[pos_])) && s_[pos_] != 'e' &&
               s_[pos_] != 'E';
    }
    char Command() { return s_[pos_++]; }

    bool Number(float& out) {
        SkipSeparators();
        const size_t start = pos_;
        if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+')) ++pos_;
        bool dot = false, digits = false;
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if (std::isdigit(static_cast<unsigned char>(c))) { digits = true; ++pos_; }
            else if (c == '.' && !dot) { dot = true; ++pos_; }
            else break;
        }
        if (digits && pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            size_t p = pos_ + 1;
            if (p < s_.size() && (s_[p] == '-' || s_[p] == '+')) ++p;
            if (p < s_.size() && std::isdigit(static_cast<unsigned char>(s_[p]))) {
                pos_ = p;
                while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) ++pos_;
            }
        }
        if (!digits) {
            pos_ = start;
            return false;
        }
        out = std::strtof(std::string(s_.substr(start, pos_ - start)).c_str(), nullptr);
        return true;
    }

    // Флаг дуги - одна цифра 0/1, может идти без разделителя: "a.7.7 0 01 1.1-.6"
    bool Flag(bool& out) {
        SkipSeparators();
        if (pos_ < s_.size() && (s_[pos_] == '0' || s_[pos_] == '1')) {
            out = s_[pos_++] == '1';
            return true;
        }
        return false;
    }

private:
    std::string_view s_;
    size_t pos_ = 0;
};

}  // namespace

ComPtr<ID2D1PathGeometry> ParseSvgPath(ID2D1Factory1* factory, std::string_view d) {
    ComPtr<ID2D1PathGeometry> geo;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(factory->CreatePathGeometry(&geo)) || FAILED(geo->Open(&sink))) return nullptr;
    sink->SetFillMode(D2D1_FILL_MODE_WINDING);

    PathReader r(d);
    D2D1_POINT_2F cur{0, 0}, start{0, 0}, lastCtrl{0, 0};
    char cmd = 0, prevCmd = 0;
    bool open = false;
    auto beginAt = [&](D2D1_POINT_2F p) {
        if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->BeginFigure(p, D2D1_FIGURE_BEGIN_FILLED);
        open = true;
        start = p;
    };
    while (!r.AtEnd()) {
        if (r.AtCommand()) cmd = r.Command();
        else if (cmd == 0) break;
        const bool rel = std::islower(static_cast<unsigned char>(cmd));
        const char up = static_cast<char>(std::toupper(static_cast<unsigned char>(cmd)));
        const float ox = rel ? cur.x : 0, oy = rel ? cur.y : 0;
        float a = 0, b = 0, c = 0, e = 0, f = 0, g = 0;
        switch (up) {
            case 'M':
                if (!r.Number(a) || !r.Number(b)) return nullptr;
                cur = {ox + a, oy + b};
                beginAt(cur);
                cmd = rel ? 'l' : 'L';  // следующие пары после M - это L
                break;
            case 'L':
                if (!r.Number(a) || !r.Number(b)) return nullptr;
                cur = {ox + a, oy + b};
                sink->AddLine(cur);
                break;
            case 'H':
                if (!r.Number(a)) return nullptr;
                cur.x = (rel ? cur.x : 0) + a;
                sink->AddLine(cur);
                break;
            case 'V':
                if (!r.Number(a)) return nullptr;
                cur.y = (rel ? cur.y : 0) + a;
                sink->AddLine(cur);
                break;
            case 'C': {
                if (!r.Number(a) || !r.Number(b) || !r.Number(c) || !r.Number(e) || !r.Number(f) || !r.Number(g))
                    return nullptr;
                const D2D1_POINT_2F p1{ox + a, oy + b}, p2{ox + c, oy + e}, p3{ox + f, oy + g};
                sink->AddBezier({p1, p2, p3});
                lastCtrl = p2;
                cur = p3;
                break;
            }
            case 'S': {
                if (!r.Number(c) || !r.Number(e) || !r.Number(f) || !r.Number(g)) return nullptr;
                const char pu = static_cast<char>(std::toupper(static_cast<unsigned char>(prevCmd)));
                const D2D1_POINT_2F p1 = (pu == 'C' || pu == 'S') ? D2D1_POINT_2F{2 * cur.x - lastCtrl.x,
                                                                                   2 * cur.y - lastCtrl.y}
                                                                  : cur;
                const D2D1_POINT_2F p2{ox + c, oy + e}, p3{ox + f, oy + g};
                sink->AddBezier({p1, p2, p3});
                lastCtrl = p2;
                cur = p3;
                break;
            }
            case 'Q': {
                if (!r.Number(a) || !r.Number(b) || !r.Number(f) || !r.Number(g)) return nullptr;
                const D2D1_POINT_2F p1{ox + a, oy + b}, p2{ox + f, oy + g};
                sink->AddQuadraticBezier({p1, p2});
                lastCtrl = p1;
                cur = p2;
                break;
            }
            case 'A': {
                float rx = 0, ry = 0, rot = 0;
                bool large = false, sweep = false;
                if (!r.Number(rx) || !r.Number(ry) || !r.Number(rot) || !r.Flag(large) || !r.Flag(sweep) ||
                    !r.Number(f) || !r.Number(g))
                    return nullptr;
                const D2D1_POINT_2F end{ox + f, oy + g};
                // Дуга - кривыми Безье (view/svg_arc.h): одинаково в любой реализации D2D
                for (const view::Cubic& q : view::ArcToCubics({cur.x, cur.y}, rx, ry, rot, large, sweep,
                                                              {end.x, end.y}))
                    sink->AddBezier({{q[0].x, q[0].y}, {q[1].x, q[1].y}, {q[2].x, q[2].y}});
                cur = end;
                break;
            }
            case 'Z':
                if (open) sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                open = false;
                cur = start;
                break;
            default: return nullptr;
        }
        prevCmd = cmd;
    }
    if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (FAILED(sink->Close())) return nullptr;
    return geo;
}

bool IconSet::Init(ID2D1Factory1* factory) {
    icons_.assign(static_cast<size_t>(IconId::Count), Icon{});
    struct LayerDef {
        const char* d;
        bool solid;
        IconPart part;
    };
    auto add = [&](IconId id, float box, std::initializer_list<LayerDef> layers) {
        Icon& icon = icons_[static_cast<size_t>(id)];
        icon.viewBox = box;
        for (const auto& l : layers) {
            IconLayer layer;
            layer.geometry = ParseSvgPath(factory, l.d);
            layer.solid = l.solid;
            layer.part = l.part;
            if (layer.geometry) icon.layers.push_back(std::move(layer));
        }
    };
    const IconPart B = IconPart::Base;
    // Пути - из mockup/index.html, без изменений
    add(IconId::Shuffle, 24,
        {{"M3 7h3.2c2 0 3.3 1 4.3 2.7l3 4.6c1 1.7 2.3 2.7 4.3 2.7H21", false, B},
         {"M3 17h3.2c1.4 0 2.4-.5 3.3-1.5M14.5 8.5c.9-1 1.9-1.5 3.3-1.5H21", false, B},
         {"M18 4l3 3-3 3M18 14l3 3-3 3", false, B}});
    add(IconId::Prev, 24,
        {{"M17.4 6.3 9.6 11.4a.7.7 0 0 0 0 1.2l7.8 5.1a.7.7 0 0 0 1.1-.6V6.9a.7.7 0 0 0-1.1-.6Z", true, B},
         {"M6.5 6.5v11", false, B}});
    add(IconId::Next, 24,
        {{"M6.6 6.3l7.8 5.1a.7.7 0 0 1 0 1.2l-7.8 5.1a.7.7 0 0 1-1.1-.6V6.9a.7.7 0 0 1 1.1-.6Z", true, B},
         {"M17.5 6.5v11", false, B}});
    add(IconId::Repeat, 24,
        {{"M4 11.5V10a3.5 3.5 0 0 1 3.5-3.5H20", false, B},
         {"M17 3.5l3 3-3 3", false, B},
         {"M20 12.5V14a3.5 3.5 0 0 1-3.5 3.5H4", false, B},
         {"M7 20.5l-3-3 3-3", false, B},
         {"M11.2 10.6l1.3-.9v4.6", false, IconPart::RepeatOne}});
    add(IconId::Volume, 24,
        {{"M4.5 9.3h2.8l4-3.4a.6.6 0 0 1 1 .5v11.2a.6.6 0 0 1-1 .5l-4-3.4H4.5a1 1 0 0 1-1-1V10.3a1 1 0 0 1 1-1Z", true,
          B},
         {"M15.5 9.5a3.6 3.6 0 0 1 0 5", false, IconPart::Wave1},
         {"M18.2 7a7.2 7.2 0 0 1 0 10", false, IconPart::Wave2},
         {"M16.5 10l4 4M20.5 10l-4 4", false, IconPart::Mute}});
    add(IconId::Playlist, 24,
        {{"M4 6.5h12M4 11.5h12M4 16.5h7", false, B},
         {"M15 14.2v5.6a.5.5 0 0 0 .8.4l4-2.8a.5.5 0 0 0 0-.8l-4-2.8a.5.5 0 0 0-.8.4Z", true, B}});
    add(IconId::Expand, 24, {{"M14 4h6v6M20 4l-6.5 6.5M10 20H4v-6M4 20l6.5-6.5", false, B}});
    // Кнопки заголовка - сетка 16x16, обводка 1.4
    add(IconId::Settings, 16,
        {{"M10.2 8a2.2 2.2 0 1 1-4.4 0a2.2 2.2 0 1 1 4.4 0Z", false, B},
         {"M8 1.8v1.6M8 12.6v1.6M1.8 8h1.6M12.6 8h1.6M3.6 3.6l1.1 1.1M11.3 11.3l1.1 1.1M3.6 12.4l1.1-1.1M11.3 "
          "4.7l1.1-1.1",
          false, B}});
    add(IconId::MiniMode, 16,
        {{"M3.5 3h9a1.5 1.5 0 0 1 1.5 1.5v7a1.5 1.5 0 0 1-1.5 1.5h-9A1.5 1.5 0 0 1 2 11.5v-7A1.5 1.5 0 0 1 3.5 3Z",
          false, B},
         {"M5 10h6", false, B}});
    add(IconId::Minimize, 16, {{"M3.5 8h9", false, B}});
    add(IconId::Close, 16, {{"M4 4l8 8M12 4l-8 8", false, B}});
    add(IconId::Moon, 24, {{"M15.2 3.2a9 9 0 1 0 5.6 15.1 7.4 7.4 0 0 1-5.6-15.1Z", true, B}});
    return true;
}

}  // namespace ui
