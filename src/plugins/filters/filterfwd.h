/*
 * Fooyin
 * Copyright © 2023, Luke Taylor <LukeT1@proton.me>
 *
 * Fooyin is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Fooyin is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Fooyin.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#pragma once

#include <QApplication>
#include <QColor>
#include <QDataStream>
#include <QFont>
#include <QPalette>

namespace Fooyin::Filters {
struct FilterOptions
{
    bool fontChanged{false};
    QFont font;

    bool colourChanged{false};
    QColor colour;

    int rowHeight{25};

    FilterOptions()
        : colour{QApplication::palette().text().color()}
    { }

    friend QDataStream& operator<<(QDataStream& stream, const FilterOptions& options)
    {
        stream << options.fontChanged;
        if(options.fontChanged) {
            stream << options.font;
        }
        stream << options.colourChanged;
        if(options.colourChanged) {
            stream << options.colour;
        }
        stream << options.rowHeight;
        return stream;
    }

    friend QDataStream& operator>>(QDataStream& stream, FilterOptions& options)
    {
        stream >> options.fontChanged;
        if(options.fontChanged) {
            stream >> options.font;
        }
        stream >> options.colourChanged;
        if(options.colourChanged) {
            stream >> options.colour;
        }
        stream >> options.rowHeight;
        return stream;
    }
};

struct FilterColumn
{
    int id{-1};
    int index{-1};
    bool isDefault{false};
    QString name;
    QString field;

    bool operator==(const FilterColumn& other) const
    {
        return std::tie(id, index, name, field) == std::tie(other.id, other.index, other.name, other.field);
    }

    [[nodiscard]] bool isValid() const
    {
        return id >= 0 && !name.isEmpty() && !field.isEmpty();
    }

    friend QDataStream& operator<<(QDataStream& stream, const FilterColumn& column)
    {
        stream << column.id;
        stream << column.index;
        stream << column.name;
        stream << column.field;
        return stream;
    }

    friend QDataStream& operator>>(QDataStream& stream, FilterColumn& column)
    {
        stream >> column.id;
        stream >> column.index;
        stream >> column.name;
        stream >> column.field;
        return stream;
    }
};
using FilterColumnList = std::vector<FilterColumn>;
using ColumnIds        = std::vector<int>;
} // namespace Fooyin::Filters

Q_DECLARE_METATYPE(Fooyin::Filters::FilterOptions)
