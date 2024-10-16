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

#include "librarytreepopulator.h"

#include <core/constants.h>
#include <core/scripting/scriptparser.h>
#include <core/scripting/scriptregistry.h>

#include <utils/crypto.h>

constexpr int InitialBatchSize = 3000;
constexpr int BatchSize        = 4000;

namespace Fooyin {
struct LibraryTreePopulator::Private
{
    LibraryTreePopulator* self;

    ScriptRegistry registry;
    ScriptParser parser;

    QString currentGrouping;
    ParsedScript script;

    LibraryTreeItem root;
    PendingTreeData data;
    TrackList pendingTracks;

    explicit Private(LibraryTreePopulator* self_)
        : self{self_}
        , parser{&registry}
        , data{}
    { }

    LibraryTreeItem* getOrInsertItem(const QString& key, const LibraryTreeItem* parent, const QString& title, int level)
    {
        auto [node, inserted] = data.items.try_emplace(key, LibraryTreeItem{title, nullptr, level});
        if(inserted) {
            node->second.setKey(key);
        }
        LibraryTreeItem* child = &node->second;

        if(!child->pending()) {
            child->setPending(true);
            data.nodes[parent->key()].push_back(key);
        }
        return child;
    }

    void iterateTrack(const Track& track)
    {
        LibraryTreeItem* parent = &root;

        const QString field = parser.evaluate(script, track);
        if(field.isNull()) {
            return;
        }

        const QStringList values = field.split(QStringLiteral("\037"), Qt::SkipEmptyParts);
        for(const QString& value : values) {
            if(value.isNull()) {
                continue;
            }
            const QStringList items = value.split(QStringLiteral("||"));

            for(int level{0}; const QString& item : items) {
                const QString title = item.trimmed();
                const QString key   = Utils::generateHash(parent->key(), title);

                auto* node = getOrInsertItem(key, parent, title, level);

                node->addTrack(track);
                data.trackParents[track.id()].push_back(node->key());

                parent = node;
                ++level;
            }
        }
    }

    void runBatch(int size)
    {
        if(size <= 0) {
            return;
        }

        auto tracksBatch = std::ranges::views::take(pendingTracks, size);

        for(const Track& track : tracksBatch) {
            if(!self->mayRun()) {
                return;
            }

            if(track.enabled() && track.libraryId() >= 0) {
                iterateTrack(track);
            }
        }

        if(!self->mayRun()) {
            return;
        }

        emit self->populated(data);

        auto tracksToKeep = std::ranges::views::drop(pendingTracks, size);
        TrackList tempTracks;
        std::ranges::copy(tracksToKeep, std::back_inserter(tempTracks));
        pendingTracks = std::move(tempTracks);

        data.clear();

        const auto remaining = static_cast<int>(pendingTracks.size());
        runBatch(std::min(remaining, BatchSize));

        if(remaining == 0) {
            emit self->finished();
        }
    }
};

LibraryTreePopulator::LibraryTreePopulator(QObject* parent)
    : Worker{parent}
    , p{std::make_unique<Private>(this)}
{ }

LibraryTreePopulator::~LibraryTreePopulator() = default;

void LibraryTreePopulator::run(const QString& grouping, const TrackList& tracks)
{
    setState(Running);

    p->data.clear();

    if(std::exchange(p->currentGrouping, grouping) != grouping) {
        p->script = p->parser.parse(p->currentGrouping);
    }

    p->pendingTracks = tracks;
    p->runBatch(InitialBatchSize);

    setState(Idle);
}
} // namespace Fooyin

#include "moc_librarytreepopulator.cpp"
