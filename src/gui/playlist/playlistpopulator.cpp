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

#include "playlistpopulator.h"

#include "playlistpreset.h"
#include "playlistscriptregistry.h"

#include <utils/crypto.h>

#include <QCryptographicHash>
#include <QTimer>

#include <ranges>

constexpr int TrackPreloadSize = 2000;

namespace Fooyin {
struct PlaylistPopulator::Private
{
    PlaylistPopulator* self;

    PlaylistPreset currentPreset;
    PlaylistColumnList columns;

    std::unique_ptr<PlaylistScriptRegistry> registry;
    ScriptParser parser;

    QString prevBaseHeaderKey;
    QString prevHeaderKey;
    std::vector<QString> prevBaseSubheaderKey;
    std::vector<QString> prevSubheaderKey;

    std::vector<PlaylistContainerItem> subheaders;

    PlaylistItem root;
    PendingData data;
    ContainerKeyMap headers;
    TrackList pendingTracks;

    explicit Private(PlaylistPopulator* self_)
        : self{self_}
        , registry{std::make_unique<PlaylistScriptRegistry>()}
        , parser{registry.get()}
    { }

    void reset()
    {
        data.clear();
        headers.clear();
        prevBaseSubheaderKey.clear();
        prevSubheaderKey.clear();
        prevBaseHeaderKey.clear();
        prevHeaderKey.clear();
    }

    void updateScripts()
    {
        if(!columns.empty()) {
            TextBlockList columnBlocks;
            for(const auto& column : columns) {
                columnBlocks.emplace_back(column.field);
            }
            currentPreset.track.leftText = columnBlocks;
        }
    }

    PlaylistItem* getOrInsertItem(const QString& key, PlaylistItem::ItemType type, const Data& item,
                                  PlaylistItem* parent, const QString& baseKey = {})
    {
        auto [node, inserted] = data.items.try_emplace(key, PlaylistItem{type, item, parent});
        if(inserted) {
            node->second.setBaseKey(baseKey.isEmpty() ? key : baseKey);
            node->second.setKey(key);
        }
        PlaylistItem* child = &node->second;

        if(!child->pending()) {
            child->setPending(true);
            data.nodes[parent->key()].push_back(key);
            if(type != PlaylistItem::Track) {
                data.containerOrder.push_back(key);
            }
        }
        return child;
    }

    void updateContainers()
    {
        for(const auto& [key, container] : headers) {
            container->updateGroupText(&parser);
        }
    }

    void iterateHeader(const Track& track, PlaylistItem*& parent)
    {
        HeaderRow row{currentPreset.header};
        if(!row.isValid()) {
            return;
        }

        auto evaluateBlocks = [this, track](const TextBlockList& presetBlocks, TextBlockList& headerBlocks) -> QString {
            QString key;
            headerBlocks.clear();
            for(const TextBlock& block : presetBlocks) {
                TextBlock headerBlock{block};
                headerBlock.text = parser.evaluate(headerBlock.script, track);
                if(!headerBlock.text.isEmpty()) {
                    headerBlocks.push_back(headerBlock);
                }
                key += headerBlock.text;
            }
            return key;
        };

        auto generateHeaderKey = [this, &row, &evaluateBlocks]() {
            return Utils::generateHash(evaluateBlocks(currentPreset.header.title, row.title),
                                       evaluateBlocks(currentPreset.header.subtitle, row.subtitle),
                                       evaluateBlocks(currentPreset.header.sideText, row.sideText),
                                       evaluateBlocks(currentPreset.header.info, row.info));
        };

        const QString baseKey = generateHeaderKey();
        QString key           = Utils::generateRandomHash();
        if(!prevHeaderKey.isEmpty() && prevBaseHeaderKey == baseKey) {
            key = prevHeaderKey;
        }
        prevBaseHeaderKey = baseKey;
        prevHeaderKey     = key;

        if(!headers.contains(key)) {
            PlaylistContainerItem header;
            header.setTitle(row.title);
            header.setSubtitle(row.subtitle);
            header.setSideText(row.sideText);
            header.setInfo(row.info);
            header.setRowHeight(row.rowHeight);

            auto* headerItem      = getOrInsertItem(key, PlaylistItem::Header, header, parent, baseKey);
            auto& headerContainer = std::get<1>(headerItem->data());
            headers.emplace(key, &headerContainer);
        }
        PlaylistContainerItem* header = headers.at(key);
        header->addTrack(track);
        data.trackParents[track.id()].push_back(key);

        auto* headerItem = &data.items.at(key);
        parent           = headerItem;
    }

    void iterateSubheaders(const Track& track, PlaylistItem*& parent)
    {
        for(auto& subheader : currentPreset.subHeaders) {
            for(auto& block : subheader.leftText) {
                block.text = parser.evaluate(block.script, track);
            }
            for(auto& block : subheader.rightText) {
                block.text = parser.evaluate(block.script, track);
            }

            PlaylistContainerItem currentContainer;
            currentContainer.setTitle(subheader.leftText);
            currentContainer.setSubtitle(subheader.rightText);
            currentContainer.setRowHeight(subheader.rowHeight);
            subheaders.push_back(currentContainer);
        }

        const int subheaderCount = static_cast<int>(subheaders.size());
        prevSubheaderKey.resize(subheaderCount);
        prevBaseSubheaderKey.resize(subheaderCount);

        auto generateSubheaderKey = [](const PlaylistContainerItem& subheader) {
            QString subheaderKey;
            for(const TextBlock& block : subheader.title()) {
                subheaderKey += block.text;
            }
            for(const TextBlock& block : subheader.subtitle()) {
                subheaderKey += block.text;
            }
            return subheaderKey;
        };

        for(int i{0}; const auto& subheader : subheaders) {
            const QString subheaderKey = generateSubheaderKey(subheader);

            if(subheaderKey.isEmpty()) {
                prevBaseSubheaderKey[i].clear();
                prevSubheaderKey[i].clear();
                continue;
            }

            const QString baseKey = Utils::generateHash(parent->baseKey(), subheaderKey);
            QString key           = Utils::generateRandomHash();
            if(static_cast<int>(prevSubheaderKey.size()) > i && prevBaseSubheaderKey.at(i) == baseKey) {
                key = prevSubheaderKey.at(i);
            }
            prevBaseSubheaderKey[i] = baseKey;
            prevSubheaderKey[i]     = key;

            if(!headers.contains(key)) {
                auto* subheaderItem      = getOrInsertItem(key, PlaylistItem::Subheader, subheader, parent, baseKey);
                auto& subheaderContainer = std::get<1>(subheaderItem->data());
                headers.emplace(key, &subheaderContainer);
            }
            PlaylistContainerItem* subheaderContainer = headers.at(key);
            subheaderContainer->addTrack(track);
            data.trackParents[track.id()].push_back(key);

            auto* subheaderItem = &data.items.at(key);
            if(subheaderItem->parent()->type() != PlaylistItem::Header) {
                subheaderItem->setIndentation(subheaderItem->parent()->indentation() + 20);
            }
            parent = subheaderItem;
            ++i;
        }
        subheaders.clear();
    }

    PlaylistItem* iterateTrack(const Track& track)
    {
        PlaylistItem* parent = &root;

        iterateHeader(track, parent);
        iterateSubheaders(track, parent);

        if(!currentPreset.track.isValid()) {
            return nullptr;
        }

        auto evaluateTrack = [this, &track](const TextBlockList& blocks, TextBlockList& trackRow) {
            for(const TextBlock& block : blocks) {
                TextBlock trackBlock{block};
                trackBlock.text = parser.evaluate(block.script, track);
                trackRow.push_back(trackBlock);
            }
        };

        TrackRow trackRow;
        evaluateTrack(currentPreset.track.leftText, trackRow.leftText);
        evaluateTrack(currentPreset.track.rightText, trackRow.rightText);

        PlaylistTrackItem playlistTrack{trackRow.leftText, trackRow.rightText, track};

        const QString key = Utils::generateHash(parent->key(), track.hash(), QString::number(data.items.size()));

        auto* trackItem = getOrInsertItem(key, PlaylistItem::Track, playlistTrack, parent);
        data.trackParents[track.id()].push_back(key);

        if(parent->type() != PlaylistItem::Header) {
            trackItem->setIndentation(parent->indentation() + 20);
        }
        return trackItem;
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
            iterateTrack(track);
        }

        updateContainers();

        if(!self->mayRun()) {
            return;
        }

        QMetaObject::invokeMethod(self, "populated", Q_ARG(PendingData, data));

        auto tracksToKeep = std::ranges::views::drop(pendingTracks, size);
        TrackList tempTracks;
        std::ranges::copy(tracksToKeep, std::back_inserter(tempTracks));
        pendingTracks = std::move(tempTracks);

        data.nodes.clear();

        const auto remaining = static_cast<int>(pendingTracks.size());
        runBatch(remaining);
    }

    void runTracksGroup(const std::map<int, TrackList>& tracks)
    {
        for(const auto& [index, trackGroup] : tracks) {
            std::vector<QString> trackKeys;

            for(const Track& track : trackGroup) {
                if(!self->mayRun()) {
                    return;
                }
                if(const auto* trackItem = iterateTrack(track)) {
                    trackKeys.push_back(trackItem->key());
                }
            }
            data.indexNodes.emplace(index, trackKeys);
        }

        updateContainers();

        if(!self->mayRun()) {
            return;
        }

        QMetaObject::invokeMethod(self, "populatedTrackGroup", Q_ARG(PendingData, data));
    }
};

PlaylistPopulator::PlaylistPopulator(QObject* parent)
    : Worker{parent}
    , p{std::make_unique<Private>(this)}
{
    qRegisterMetaType<PendingData>();
}

void PlaylistPopulator::run(int playlistId, const PlaylistPreset& preset, const PlaylistColumnList& columns,
                            const TrackList& tracks)
{
    setState(Running);

    p->reset();

    p->data.playlistId = playlistId;
    p->currentPreset   = preset;
    p->columns         = columns;
    p->pendingTracks   = tracks;

    p->updateScripts();
    p->runBatch(TrackPreloadSize);

    setState(Idle);
}

void PlaylistPopulator::runTracks(int playlistId, const PlaylistPreset& preset, const PlaylistColumnList& columns,
                                  const std::map<int, TrackList>& tracks)
{
    setState(Running);

    p->reset();

    p->data.playlistId = playlistId;
    p->currentPreset   = preset;
    p->columns         = columns;

    p->updateScripts();
    p->runTracksGroup(tracks);

    setState(Idle);
}

void PlaylistPopulator::updateHeaders(const ItemList& headers)
{
    setState(Running);

    ItemKeyMap updatedHeaders;

    for(const PlaylistItem& item : headers) {
        PlaylistContainerItem& header = std::get<1>(item.data());
        header.updateGroupText(&p->parser);
        updatedHeaders.emplace(item.key(), item);
    }

    emit headersUpdated(updatedHeaders);

    setState(Idle);
}

PlaylistPopulator::~PlaylistPopulator() = default;
} // namespace Fooyin

#include "moc_playlistpopulator.cpp"
