/*
 * Fooyin
 * Copyright 2022-2023, Luke Taylor <LukeT1@proton.me>
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

#include "gui/fywidget.h"

#include <map>

#include <QDebug>

namespace Gui::Widgets {
class WidgetFactory : public QObject
{
    Q_OBJECT

    using Instantiator = std::function<FyWidget*()>;

    struct FactoryWidget
    {
        QString name;
        Instantiator instantiator;
        QStringList subMenus;
    };

protected:
    using FactoryWidgets = std::map<QString, FactoryWidget>;
    FactoryWidgets widgets;

public:
    template <typename T, typename Factory>
    bool registerClass(const QString& name, Factory factory, const QStringList& subMenus = {})
    {
        static_assert(std::is_base_of<FyWidget, T>::value, "Class must derive from the factory's base class");

        if(widgets.count(name)) {
            qDebug() << ("Subclass already registered");
            return false;
        }

        FactoryWidget fw;
        fw.name         = name;
        fw.instantiator = factory;
        fw.subMenus     = subMenus;

        widgets.emplace(name, fw);
        return true;
    }

    [[nodiscard]] FyWidget* make(const QString& name) const
    {
        if(!widgets.count(name)) {
            return nullptr;
        }

        auto widget = widgets.at(name);
        if(!widget.instantiator) {
            return nullptr;
        }
        return widget.instantiator();
    }

    [[nodiscard]] FactoryWidgets registeredWidgets() const
    {
        return widgets;
    }
};
} // namespace Gui::Widgets
