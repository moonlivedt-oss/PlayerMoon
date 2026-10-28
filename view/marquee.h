#pragma once
// Бегущая строка (перенос класса Marquee из макета).
// Едет, только если текст шире поля и музыка играет: пауза `pause`, затем сдвиг со скоростью
// `speed` px/с, бесшовный повтор через промежуток `gap`. На паузе быстро докручивает круг до
// шва и останавливается - после этого кадры не нужны.

namespace view {

class Marquee {
public:
    // textWidth - ширина текста, boxWidth - ширина поля (DIP)
    void Measure(float textWidth, float boxWidth, float gap, double pause, bool reduce);
    // Шаг анимации. Возвращает true, если нужен следующий кадр.
    bool Step(double dt, bool playing, float speed);
    // При старте воспроизведения - снова выдержать паузу перед движением
    void OnPlay() {
        if (offset_ == 0) hold_ = pause_;
    }

    bool Active() const { return active_; }  // текст не помещается - нужна маска краёв и вторая копия
    bool Moving() const { return offset_ > 0; }
    float Offset() const { return offset_; }
    float Distance() const { return distance_; }  // ширина текста + промежуток

private:
    bool active_ = false;
    float offset_ = 0;
    float distance_ = 0;
    double hold_ = 0;
    double pause_ = 1.6;
};

}  // namespace view
