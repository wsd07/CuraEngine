// Copyright (c) 2024 UltiMaker
// CuraEngine is released under the terms of the AGPLv3 or higher

#include "settings/HeightParameterGraph.h"

#include <algorithm>
#include <spdlog/spdlog.h>

namespace cura
{

double HeightParameterGraph::getParameter(const coord_t height, const double default_parameter) const
{
    if (data_.size() == 0)
    {
        return default_parameter;
    }
    if (data_.size() == 1)
    {
        return data_.front().parameter_;
    }
    
    // 低于最低高度：使用最低点的参数值
    if (height < data_.front().height_)
    {
        spdlog::debug("Height {:.2f}mm below minimum {:.2f}mm, using minimum parameter {}", 
                     INT2MM(height), INT2MM(data_.front().height_), data_.front().parameter_);
        return data_.front().parameter_;
    }
    
    // 在数据点之间进行线性插值
    const Datum* last_datum = &data_.front();
    for (unsigned int datum_idx = 1; datum_idx < data_.size(); datum_idx++)
    {
        const Datum& datum = data_[datum_idx];
        if (datum.height_ >= height)
        {
            // 线性插值计算
            double interpolated_parameter = last_datum->parameter_ + 
                (datum.parameter_ - last_datum->parameter_) * 
                (height - last_datum->height_) / 
                (datum.height_ - last_datum->height_);
            
            spdlog::debug("Height {:.2f}mm interpolated between {:.2f}mm and {:.2f}mm, parameter: {}", 
                         INT2MM(height), INT2MM(last_datum->height_), INT2MM(datum.height_), interpolated_parameter);
            return interpolated_parameter;
        }
        last_datum = &datum;
    }

    // 高于最高高度：使用最高点的参数值
    spdlog::debug("Height {:.2f}mm above maximum {:.2f}mm, using maximum parameter {}", 
                 INT2MM(height), INT2MM(data_.back().height_), data_.back().parameter_);
    return data_.back().parameter_;
}

bool HeightParameterGraph::isEmpty() const
{
    return data_.empty();
}

bool HeightRangeList::isInRange(const coord_t height) const
{
    for (const Range& range : ranges_)
    {
        if (range.contains(height))
        {
            return true;
        }
    }
    return false;
}

bool HeightRangeList::isEmpty() const
{
    return ranges_.empty();
}

void HeightRangeList::addRange(const coord_t min_height, const coord_t max_height)
{
    ranges_.emplace_back(min_height, max_height);
}

void HeightRangeList::sortRanges()
{
    std::sort(ranges_.begin(), ranges_.end(),
              [](const Range& a, const Range& b) {
                  return a.min_height_ < b.min_height_;
              });
}

} // namespace cura
