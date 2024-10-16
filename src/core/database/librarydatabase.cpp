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

#include "librarydatabase.h"

#include "databasequery.h"

namespace Fooyin {
LibraryDatabase::LibraryDatabase(const QString& connectionName)
    : DatabaseModule{connectionName}
{ }

bool LibraryDatabase::getAllLibraries(LibraryInfoMap& libraries)
{
    const QString query = QStringLiteral("SELECT LibraryID, Name, Path FROM Libraries;");

    DatabaseQuery q(this);
    q.prepareQuery(query);

    if(!q.execQuery()) {
        q.error(QStringLiteral("Cannot fetch all libraries"));
        return false;
    }

    while(q.next()) {
        const int id       = q.value(0).toInt();
        const QString name = q.value(1).toString();
        const QString path = q.value(2).toString();

        libraries.emplace(id, LibraryInfo{name, path, id});
    }
    return true;
}

int LibraryDatabase::insertLibrary(const QString& path, const QString& name)
{
    if(name.isEmpty() || path.isEmpty()) {
        return -1;
    }

    auto q = insert(QStringLiteral("Libraries"), {{QStringLiteral("Name"), name}, {QStringLiteral("Path"), path}},
                    QString{QStringLiteral("Cannot insert library (name: %1, path: %2)")}.arg(name, path));

    return (q.hasError()) ? -1 : q.lastInsertId().toInt();
}

bool LibraryDatabase::removeLibrary(int id)
{
    auto q = remove(QStringLiteral("Libraries"), {{QStringLiteral("LibraryID"), QString::number(id)}},
                    QStringLiteral("Cannot remove library ") + QString::number(id));
    return !q.hasError();
}

bool LibraryDatabase::renameLibrary(int id, const QString& name)
{
    if(name.isEmpty()) {
        return false;
    }

    auto q = update(QStringLiteral("Libraries"), {{QStringLiteral("Name"), name}},
                    {QStringLiteral("LibraryID"), QString::number(id)},
                    QStringLiteral("Cannot update library ") + QString::number(id));
    return !q.hasError();
}
} // namespace Fooyin
