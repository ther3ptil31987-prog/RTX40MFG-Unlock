#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace cadence_stats
{
// Fixed-size window of millisecond samples, summarised for the log once full.
// No allocation while sampling; the summary string is built only on emit.
template<size_t N>
class Window
{
public:
    struct Summary { double mean{}, sd{}, min{}, p5{}, p50{}, p95{}, max{}; size_t count{}; };
    // Returns true once N samples are held; Take() then summarises and clears.
    bool Add(double value) noexcept
    {
        if (!std::isfinite(value)) return count_ == N;
        if (count_ < N) values_[count_++] = value;
        return count_ == N;
    }
    size_t Count() const noexcept { return count_; }
    Summary Take() noexcept
    {
        Summary s{};
        if (!count_) return s;
        std::sort(values_.begin(), values_.begin()+count_);
        double sum{}; for (size_t i = 0; i < count_; ++i) sum += values_[i];
        s.mean = sum/double(count_);
        double sq{}; for (size_t i = 0; i < count_; ++i) sq += (values_[i]-s.mean)*(values_[i]-s.mean);
        s.sd = std::sqrt(sq/double(count_));
        auto at = [&](double p) { return values_[size_t(std::lround(p*double(count_-1)))]; };
        s.min = values_[0]; s.p5 = at(0.05); s.p50 = at(0.5); s.p95 = at(0.95); s.max = values_[count_-1];
        s.count = count_; count_ = 0;
        return s;
    }
private:
    std::array<double, N> values_{};
    size_t count_{};
};

template<class Summary>
std::string Json(const char* name, const Summary& s)
{
    char text[256]{};
    snprintf(text, sizeof(text),
        "\"%s\":{\"n\":%zu,\"mean\":%.3f,\"sd\":%.3f,\"min\":%.3f,\"p5\":%.3f,\"p50\":%.3f,\"p95\":%.3f,\"max\":%.3f}",
        name, s.count, s.mean, s.sd, s.min, s.p5, s.p50, s.p95, s.max);
    return text;
}
}
