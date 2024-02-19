// Copyright (c) 2024 The Vacon Authors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include <cmath>
#include <thread>

namespace vacon {

struct Stats {
    double mean;
    double stdev;
    double min;
    double max;
};

class Welford {
    public:
        Welford() = default;

        Welford(Welford&& src)
        {
            count_  = src.count_;
            m2_     = src.m2_;
            mean_   = src.mean_;
            min_    = src.min_;
            max_    = src.max_;
        }

        void Update(double new_value)
        {
            std::lock_guard lock(mutex_);

            ++count_;
            double delta = new_value - mean_;
            mean_ += delta / count_;
            double delta2 = new_value - mean_;
            m2_ += delta * delta2;

            min_ = (min_ > 0.0) ? fmin(min_, new_value) : new_value;
            max_ = fmax(max_, new_value);
        }

        Stats Result()
        {
            std::lock_guard lock(mutex_);

            return Stats {
                .mean   = mean_,
                .stdev  = (count_ > 0.0) ? std::sqrt(m2_ / count_) : 0.0,
                .min    = min_,
                .max    = max_,
            };
        }

        void Reset()
        {
            std::lock_guard lock(mutex_);

            count_  = 0.0;
            m2_     = 0.0;
            mean_   = 0.0;
            min_    = 0.0;
            max_    = 0.0;
        }

    private:
        std::mutex mutex_;

        double count_   = 0.0;
        double m2_      = 0.0;
        double mean_    = 0.0;
        double min_     = 0.0;
        double max_     = 0.0;
};

} // namespace vacon
