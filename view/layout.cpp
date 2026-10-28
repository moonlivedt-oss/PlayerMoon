#include "view/layout.h"

#include <algorithm>

namespace view {

namespace {

constexpr float kTbButton = 44;  // кнопки заголовка 44x40

// Общая часть: заголовок окна с кнопками справа
void ArrangeTitleBar(const theme::Material& m, float width, Frame& f) {
    const float h = m.size.titlebar;
    f[Elem::TitleBar] = {0, 0, width, h};
    f[Elem::Brand] = {14, (h - 16) / 2, 100, 16};
    f[Elem::TbClose] = {width - kTbButton, 0, kTbButton, h};
    f[Elem::TbMinimize] = {width - 2 * kTbButton, 0, kTbButton, h};
    f[Elem::TbMini] = {width - 3 * kTbButton, 0, kTbButton, h};
    f[Elem::TbSettings] = {width - 4 * kTbButton, 0, kTbButton, h};
}

// Плейлист сбоку: панель справа от основной области (playlist = side)
void ArrangeSidePanel(const theme::Material& m, float textScale, float x, float top, float width, float height,
                      Frame& f) {
    f[Elem::Panel] = {x, top, width, height};
    // Отступы как в макете: сверху 4, заголовок 6/12/10/20, линия слева 1 px
    const float headTop = top + 4 + 6;
    const float headingH = m.font.fsHeading * textScale * 1.25f;
    f[Elem::PanelTitle] = {x + 1 + 20, headTop, width - 160, headingH};
    f[Elem::PanelCount] = {x + width - 12 - 110, headTop, 110, headingH};
    const float listTop = top + 4 + 6 + headingH + 10;
    f[Elem::List] = {x + 1 + 10, listTop, width - 1 - 20, top + height - listTop - 12};
    f.rowHeight = m.size.row;
}

}  // namespace

Rect Frame::Row(int i, float scroll) const {
    const Rect& list = (*this)[Elem::List];
    return {list.x, list.y + static_cast<float>(i) * rowHeight - scroll, list.w, rowHeight};
}

// ---------------- «Классика» ----------------
// Колонка по центру: обложка, текст, визуализатор-полоса, прогресс, кнопки, громкость внизу.
// Числа - из CSS макета (mockup/LAYOUTS.md, раздел «Классика»).
class ClassicLayout final : public ILayout {
public:
    std::string_view Id() const override { return "classic"; }

    Size WindowSize(const LayoutInput& in, bool playlistOpen) const override {
        const auto& g = in.geometry;
        return {g.winW + (playlistOpen ? g.playlistW : 0), g.winH};
    }

    void Arrange(const LayoutInput& in, bool playlistOpen, Frame& f) const override {
        const theme::Material& m = in.theme->material;
        const auto& g = in.geometry;
        const float ts = in.textScale;
        ArrangeTitleBar(m, WindowSize(in, playlistOpen).w, f);

        const float pad = g.padding;
        const float top = m.size.titlebar;
        const float left = pad, width = g.winW - 2 * pad;
        f[Elem::Main] = {0, top, g.winW, g.winH - top};

        float y = top + 4;
        f[Elem::Cover] = {(g.winW - g.cover) / 2, y + (g.art - g.cover) / 2, g.cover, g.cover};
        y += g.art + 22;
        const float titleH = TextMetrics::TitleLine(g.fsTitle * ts);
        const float artistH = TextMetrics::SubLine(m.font.fsArtist * ts);
        const float albumH = TextMetrics::SubLine(m.font.fsAlbum * ts);
        f[Elem::Title] = {left, y, width, titleH};
        y += titleH + 3;
        f[Elem::Artist] = {left, y, width, artistH};
        y += artistH + 3;
        f[Elem::Album] = {left, y, width, albumH};
        y += albumH + 14;
        f[Elem::Viz] = {left, y, width, g.vizH};
        y += g.vizH + 12;
        f[Elem::Seek] = {left, y, width, 20};
        y += 20 + 2;
        const float timeW = 60;
        f[Elem::TimeCur] = {left, y, timeW, TextMetrics::kTimesHeight};
        f[Elem::TimeDur] = {left + width - timeW, y, timeW, TextMetrics::kTimesHeight};
        f[Elem::Rate] = {left + width / 2 - 24, y, 48, TextMetrics::kTimesHeight};
        y += TextMetrics::kTimesHeight + 10;

        // Кнопки с равными промежутками (justify-content: space-between)
        const float play = g.play, btn = g.btn;
        const float gap = (width - (4 * btn + play)) / 4;
        const float rowCenter = y + play / 2;
        float x = left;
        const Elem order[] = {Elem::Shuffle, Elem::Prev, Elem::Play, Elem::Next, Elem::Repeat};
        for (Elem e : order) {
            const float s = e == Elem::Play ? play : btn;
            f[e] = {x, rowCenter - s / 2, s, s};
            x += s + gap;
        }

        // Нижний ряд прижат к низу: громкость слева, плейлист справа
        const float extrasY = g.winH - pad - btn;
        f[Elem::VolBtn] = {left, extrasY, btn, btn};
        f[Elem::PlBtn] = {left + width - btn, extrasY, btn, btn};
        // Блок громкости: flex 1 с max-width 220, делит место с растяжкой справа поровну
        const float volBlock = std::min(220.f, (width - btn - 2 * 8) / 2);
        f[Elem::Volume] = {left + btn + 6, extrasY + (btn - 20) / 2, volBlock - btn - 6, 20};

        ArrangeSidePanel(m, ts, g.winW, top, g.playlistW, g.winH - top, f);
    }
};

std::unique_ptr<ILayout> MakeLayout(std::string_view id) {
    // Этап 1: только «Классика». Новая раскладка = новый класс и строка здесь.
    (void)id;
    return std::make_unique<ClassicLayout>();
}

void ArrangeMini(const theme::Material& m, float textScale, Frame& f) {
    // Поля: слева 14, справа 10, снизу 2; промежуток 12; всё по центру по вертикали
    const float h = kMiniHeight, w = kMiniWidth;
    const float centerY = (h - 2) / 2;
    const float cover = m.size.miniCover;
    f[Elem::MiniCover] = {14, centerY - cover / 2, cover, cover};
    const float expand = 34, play = 44;
    f[Elem::MiniExpand] = {w - 10 - expand, centerY - expand / 2, expand, expand};
    f[Elem::MiniPlay] = {f[Elem::MiniExpand].x - 12 - play, centerY - play / 2, play, play};
    const float textX = 14 + cover + 12;
    const float titleH = TextMetrics::RowTitleLine(m.font.fsMiniTitle * textScale);
    const float artistH = 12 * 1.25f * textScale;
    const float blockH = titleH + 2 + artistH;
    const float textW = f[Elem::MiniPlay].x - 12 - textX;
    f[Elem::MiniTitle] = {textX, centerY - blockH / 2, textW, titleH};
    f[Elem::MiniArtist] = {textX, centerY - blockH / 2 + titleH + 2, textW, artistH};
    f[Elem::MiniSeek] = {0, h - 11, w, 18};
}

}  // namespace view
