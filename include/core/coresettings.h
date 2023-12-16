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

#include "fycore_export.h"

#include <utils/settings/settingtypes.h>

#include <QObject>

namespace Fooyin {
class SettingsManager;
class SortingRegistry;

namespace Settings::Core {
Q_NAMESPACE_EXPORT(FYCORE_EXPORT)
enum Settings : uint32_t
{
    Version             = 1 | SettingsType::String,
    FirstRun            = 2 | SettingsType::Bool,
    PlayMode            = 3 | SettingsType::Int,
    AutoRefresh         = 4 | SettingsType::Bool,
    LibrarySorting      = 5 | SettingsType::ByteArray,
    LibrarySortScript   = 6 | SettingsType::String,
    ActivePlaylistId    = 7 | SettingsType::Int,
    AudioOutput         = 8 | SettingsType::String,
    OutputVolume        = 9 | SettingsType::Double,
    RewindPreviousTrack = 10 | SettingsType::Bool,
};
Q_ENUM_NS(Settings)
} // namespace Settings::Core

class FYCORE_EXPORT CoreSettings
{
public:
    explicit CoreSettings(SettingsManager* settingsManager);
    ~CoreSettings();

    [[nodiscard]] SortingRegistry* sortingRegistry() const;

private:
    SettingsManager* m_settings;
    std::unique_ptr<SortingRegistry> m_sortingRegistry;
};
} // namespace Fooyin
