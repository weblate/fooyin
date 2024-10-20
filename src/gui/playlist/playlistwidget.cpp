/*
 * Fooyin
 * Copyright © 2022, Luke Taylor <LukeT1@proton.me>
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

#include "playlistwidget.h"

#include "internalguisettings.h"
#include "playlistcontroller.h"
#include "playlistdelegate.h"
#include "playlisthistory.h"
#include "playlistview.h"
#include "playlistwidget_p.h"

#include <core/library/musiclibrary.h>
#include <core/library/tracksort.h>
#include <core/player/playermanager.h>
#include <core/playlist/playlistmanager.h>
#include <gui/guiconstants.h>
#include <gui/guisettings.h>
#include <gui/trackselectioncontroller.h>
#include <utils/actions/actioncontainer.h>
#include <utils/actions/actionmanager.h>
#include <utils/actions/command.h>
#include <utils/async.h>
#include <utils/recursiveselectionmodel.h>
#include <utils/widgets/autoheaderview.h>

#include <QActionGroup>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMenu>
#include <QProgressDialog>
#include <QScrollBar>

#include <QCoro/QCoroCore>

#include <stack>

namespace {
void expandTree(QTreeView* view, QAbstractItemModel* model, const QModelIndex& parent, int first, int last)
{
    while(first <= last) {
        const QModelIndex child = model->index(first, 0, parent);
        view->expand(child);

        const int childCount = model->rowCount(child);
        if(childCount > 0) {
            expandTree(view, model, child, 0, childCount - 1);
        }

        ++first;
    }
}

Fooyin::TrackList getAllTracks(QAbstractItemModel* model, const QModelIndexList& indexes)
{
    Fooyin::TrackList tracks;

    std::stack<QModelIndex> indexStack;
    for(const QModelIndex& index : indexes | std::views::reverse) {
        indexStack.push(index);
    }

    while(!indexStack.empty()) {
        const QModelIndex currentIndex = indexStack.top();
        indexStack.pop();

        while(model->canFetchMore(currentIndex)) {
            model->fetchMore(currentIndex);
        }

        const int rowCount = model->rowCount(currentIndex);
        if(rowCount == 0
           && currentIndex.data(Fooyin::PlaylistItem::Role::Type).toInt() == Fooyin::PlaylistItem::Track) {
            tracks.push_back(currentIndex.data(Fooyin::PlaylistItem::Role::ItemData).value<Fooyin::Track>());
        }
        else {
            for(int row{rowCount - 1}; row >= 0; --row) {
                const QModelIndex childIndex = model->index(row, 0, currentIndex);
                indexStack.push(childIndex);
            }
        }
    }
    return tracks;
}
} // namespace

namespace Fooyin {
using namespace Settings::Gui::Internal;

PlaylistWidgetPrivate::PlaylistWidgetPrivate(PlaylistWidget* self_, ActionManager* actionManager_,
                                             PlaylistController* playlistController_, MusicLibrary* library_,
                                             SettingsManager* settings_)
    : self{self_}
    , actionManager{actionManager_}
    , selectionController{playlistController_->selectionController()}
    , library{library_}
    , settings{settings_}
    , settingsDialog{settings->settingsDialog()}
    , playlistController{playlistController_}
    , columnRegistry{settings}
    , presetRegistry{settings}
    , sortRegistry{settings}
    , layout{new QHBoxLayout(self)}
    , model{new PlaylistModel(library, settings, self)}
    , playlistView{new PlaylistView(self)}
    , header{new AutoHeaderView(Qt::Horizontal, self)}
    , currentPlaylist{playlistController->currentPlaylist()}
    , columnMode{false}
    , playlistContext{new WidgetContext(self, Context{Constants::Context::Playlist}, self)}
    , removeTrackAction{new QAction(tr("Remove"), self)}
{
    layout->setContentsMargins(0, 0, 0, 0);

    playlistView->setHeader(header);
    header->setStretchEnabled(true);
    header->setSectionsMovable(true);
    header->setFirstSectionMovable(true);
    header->setContextMenuPolicy(Qt::CustomContextMenu);

    playlistView->setModel(model);
    playlistView->setItemDelegate(new PlaylistDelegate(self));

    playlistView->setSelectionModel(new RecursiveSelectionModel(model, this));

    layout->addWidget(playlistView);

    setHeaderHidden(!settings->value<PlaylistHeader>());
    setScrollbarHidden(settings->value<PlaylistScrollBar>());

    changePreset(presetRegistry.itemById(settings->value<PlaylistCurrentPreset>()));

    setupConnections();
    setupActions();

    model->setCurrentPlaylistIsActive(playlistController->currentIsActive());

    model->currentTrackChanged(playlistController->currentTrack());
    model->playStateChanged(playlistController->playState());
}

void PlaylistWidgetPrivate::setupConnections()
{
    QObject::connect(header, &QHeaderView::sortIndicatorChanged, this, &PlaylistWidgetPrivate::sortColumn);
    QObject::connect(header, &QHeaderView::customContextMenuRequested, this,
                     &PlaylistWidgetPrivate::customHeaderMenuRequested);
    QObject::connect(playlistView->selectionModel(), &QItemSelectionModel::selectionChanged, this,
                     &PlaylistWidgetPrivate::selectionChanged);
    QObject::connect(playlistView, &QAbstractItemView::doubleClicked, this, &PlaylistWidgetPrivate::doubleClicked);

    QObject::connect(model, &QAbstractItemModel::modelAboutToBeReset, playlistView, &QAbstractItemView::clearSelection);
    QObject::connect(model, &PlaylistModel::playlistTracksChanged, this, &PlaylistWidgetPrivate::trackIndexesChanged);
    QObject::connect(model, &PlaylistModel::filesDropped, this, &PlaylistWidgetPrivate::scanDroppedTracks);
    QObject::connect(model, &PlaylistModel::tracksInserted, this, &PlaylistWidgetPrivate::tracksInserted);
    QObject::connect(model, &PlaylistModel::tracksMoved, this, &PlaylistWidgetPrivate::tracksMoved);
    QObject::connect(model, &QAbstractItemModel::modelReset, this, &PlaylistWidgetPrivate::resetTree);
    QObject::connect(model, &QAbstractItemModel::rowsInserted, this,
                     [this](const QModelIndex& parent, int first, int last) {
                         expandTree(playlistView, model, parent, first, last);
                     });

    QObject::connect(playlistController->playlistHandler(), &PlaylistManager::playlistTracksChanged, this,
                     &PlaylistWidgetPrivate::handleTracksChanged);
    QObject::connect(playlistController, &PlaylistController::currentPlaylistChanged, this,
                     [this](Playlist* playlist) { changePlaylist(playlist, playlist != currentPlaylist); });
    QObject::connect(playlistController, &PlaylistController::currentTrackChanged, model,
                     &PlaylistModel::currentTrackChanged);
    QObject::connect(playlistController, &PlaylistController::playStateChanged, model,
                     &PlaylistModel::playStateChanged);
    QObject::connect(playlistController->playlistHandler(), &PlaylistManager::activeTrackChanged, this,
                     &PlaylistWidgetPrivate::followCurrentTrack);
    QObject::connect(playlistController->playlistHandler(), &PlaylistManager::playlistTracksAdded, this,
                     &PlaylistWidgetPrivate::playlistTracksAdded);
    QObject::connect(&presetRegistry, &PresetRegistry::presetChanged, this, &PlaylistWidgetPrivate::onPresetChanged);

    settings->subscribe<PlaylistHeader>(this, [this](bool show) { setHeaderHidden(!show); });
    settings->subscribe<PlaylistScrollBar>(this, &PlaylistWidgetPrivate::setScrollbarHidden);
    settings->subscribe<PlaylistCurrentPreset>(
        this, [this](int presetId) { changePreset(presetRegistry.itemById(presetId)); });
}

void PlaylistWidgetPrivate::setupActions()
{
    actionManager->addContextObject(playlistContext);

    auto* editMenu = actionManager->actionContainer(Constants::Menus::Edit);

    auto* undo    = new QAction(tr("Undo"), this);
    auto* undoCmd = actionManager->registerAction(undo, Constants::Actions::Undo, playlistContext->context());
    undoCmd->setDefaultShortcut(QKeySequence::Undo);
    editMenu->addAction(undoCmd);
    QObject::connect(undo, &QAction::triggered, this, [this]() { playlistController->undoPlaylistChanges(); });
    QObject::connect(playlistController, &PlaylistController::playlistHistoryChanged, this,
                     [this, undo]() { undo->setEnabled(playlistController->canUndo()); });
    undo->setEnabled(playlistController->canUndo());

    auto* redo    = new QAction(tr("Redo"), this);
    auto* redoCmd = actionManager->registerAction(redo, Constants::Actions::Redo, playlistContext->context());
    redoCmd->setDefaultShortcut(QKeySequence::Redo);
    editMenu->addAction(redoCmd);
    QObject::connect(redo, &QAction::triggered, this, [this]() { playlistController->redoPlaylistChanges(); });
    QObject::connect(playlistController, &PlaylistController::playlistHistoryChanged, this,
                     [this, redo]() { redo->setEnabled(playlistController->canRedo()); });
    redo->setEnabled(playlistController->canRedo());

    editMenu->addSeparator();

    auto* clear = new QAction(PlaylistWidget::tr("&Clear"), self);
    editMenu->addAction(actionManager->registerAction(clear, Constants::Actions::Clear, playlistContext->context()));
    QObject::connect(clear, &QAction::triggered, this, [this]() {
        if(auto* playlist = playlistController->currentPlaylist()) {
            playlist->clear();
            model->reset(currentPreset, columnMode ? columns : PlaylistColumnList{}, playlist);
        }
    });

    auto* selectAll = new QAction(PlaylistWidget::tr("&Select All"), self);
    auto* selectAllCmd
        = actionManager->registerAction(selectAll, Constants::Actions::SelectAll, playlistContext->context());
    selectAllCmd->setDefaultShortcut(QKeySequence::SelectAll);
    editMenu->addAction(selectAllCmd);
    QObject::connect(selectAll, &QAction::triggered, this, [this]() {
        while(model->canFetchMore({})) {
            model->fetchMore({});
        }
        playlistView->selectAll();
    });

    removeTrackAction = new QAction(tr("Remove"), self);
    removeTrackAction->setEnabled(false);
    auto* removeCmd
        = actionManager->registerAction(removeTrackAction, Constants::Actions::Remove, playlistContext->context());
    removeCmd->setDefaultShortcut(QKeySequence::Delete);
    QObject::connect(removeTrackAction, &QAction::triggered, this, [this]() { tracksRemoved(); });
}

void PlaylistWidgetPrivate::onPresetChanged(const PlaylistPreset& preset)
{
    if(currentPreset.id == preset.id) {
        changePreset(preset);
    }
}

void PlaylistWidgetPrivate::changePreset(const PlaylistPreset& preset)
{
    currentPreset = preset;
    resetModel();
}

void PlaylistWidgetPrivate::changePlaylist(Playlist* playlist, bool savePlaylistState)
{
    if(settings->value<Settings::Gui::RememberPlaylistState>() && savePlaylistState) {
        saveState();
    }

    if(!playlist) {
        return;
    }

    if(std::exchange(currentPlaylist, playlist)) {
        playlistView->setUpdatesEnabled(false);
    }

    model->reset(currentPreset, columnMode ? columns : PlaylistColumnList{}, currentPlaylist);
}

void PlaylistWidgetPrivate::resetTree() const
{
    playlistView->expandAll();

    restoreState();
}

PlaylistViewState PlaylistWidgetPrivate::getState() const
{
    PlaylistViewState state;

    if(currentPlaylist->trackCount() > 0) {
        QModelIndex topTrackIndex = playlistView->indexAt({0, 0});

        if(topTrackIndex.isValid()) {
            while(model->hasChildren(topTrackIndex)) {
                topTrackIndex = playlistView->indexBelow(topTrackIndex);
            }

            state.topIndex  = topTrackIndex.data(PlaylistItem::Index).toInt();
            state.scrollPos = playlistView->verticalScrollBar()->value();
        }
    }

    return state;
}

void PlaylistWidgetPrivate::saveState() const
{
    if(currentPlaylist) {
        const auto state = getState();
        // Don't save state if still populating model
        if(currentPlaylist->trackCount() == 0 || (currentPlaylist->trackCount() > 0 && state.topIndex >= 0)) {
            playlistController->savePlaylistState(currentPlaylist->id(), state);
        }
    }
}

void PlaylistWidgetPrivate::restoreState() const
{
    auto restoreDefaultState = [this]() {
        playlistView->scrollToTop();
        playlistView->setUpdatesEnabled(true);
    };

    if(!settings->value<Settings::Gui::RememberPlaylistState>() || !currentPlaylist
       || currentPlaylist->trackCount() == 0) {
        restoreDefaultState();
        return;
    }

    const auto state = playlistController->playlistState(currentPlaylist->id());

    if(!state) {
        restoreDefaultState();
        return;
    }

    const QModelIndex modelIndex = model->indexAtTrackIndex(state->topIndex);
    if(modelIndex.isValid()) {
        playlistView->scrollTo(modelIndex);
        playlistView->verticalScrollBar()->setValue(state->scrollPos);
        playlistView->setUpdatesEnabled(true);
        return;
    }

    QMetaObject::invokeMethod(
        self,
        [this]() {
            if(model->canFetchMore({})) {
                model->fetchMore({});
            }
            restoreState();
        },
        Qt::QueuedConnection);
}

void PlaylistWidgetPrivate::resetModel() const
{
    model->reset(currentPreset, columnMode ? columns : PlaylistColumnList{}, playlistController->currentPlaylist());
}

std::vector<int> PlaylistWidgetPrivate::selectedPlaylistIndexes() const
{
    std::vector<int> selectedPlaylistIndexes;

    const auto selected = playlistView->selectionModel()->selectedRows();
    for(const QModelIndex& index : selected) {
        selectedPlaylistIndexes.emplace_back(index.data(PlaylistItem::Index).toInt());
    }

    return selectedPlaylistIndexes;
}

void PlaylistWidgetPrivate::restoreSelectedPlaylistIndexes(const std::vector<int>& indexes)
{
    if(indexes.empty()) {
        return;
    }

    const int columnCount = static_cast<int>(columns.size());

    QItemSelection indexesToSelect;
    indexesToSelect.reserve(indexes.size());

    for(const int selectedIndex : indexes) {
        const QModelIndex index = model->indexAtTrackIndex(selectedIndex);
        if(index.isValid()) {
            const QModelIndex last = index.siblingAtColumn(columnCount - 1);
            indexesToSelect.append({index, last.isValid() ? last : index});
        }
    }
    playlistView->selectionModel()->select(indexesToSelect, QItemSelectionModel::ClearAndSelect);
}

bool PlaylistWidgetPrivate::isHeaderHidden() const
{
    return playlistView->isHeaderHidden();
}

bool PlaylistWidgetPrivate::isScrollbarHidden() const
{
    return playlistView->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff;
}

void PlaylistWidgetPrivate::setHeaderHidden(bool hide) const
{
    header->setFixedHeight(hide ? 0 : QWIDGETSIZE_MAX);
    header->adjustSize();
}

void PlaylistWidgetPrivate::setScrollbarHidden(bool showScrollBar) const
{
    playlistView->setVerticalScrollBarPolicy(!showScrollBar ? Qt::ScrollBarAlwaysOff : Qt::ScrollBarAsNeeded);
}

void PlaylistWidgetPrivate::selectionChanged() const
{
    TrackList tracks;
    int firstIndex{-1};

    const QModelIndexList indexes = playlistView->selectionModel()->selectedIndexes();

    for(const QModelIndex& index : indexes) {
        const auto type = index.data(PlaylistItem::Type).toInt();
        if(type == PlaylistItem::Track) {
            tracks.push_back(index.data(PlaylistItem::Role::ItemData).value<Track>());
            const int trackIndex = index.data(PlaylistItem::Role::Index).toInt();
            firstIndex           = firstIndex >= 0 ? std::min(firstIndex, trackIndex) : trackIndex;
        }
    }

    selectionController->changeSelectedTracks(playlistContext, firstIndex, tracks);

    if(tracks.empty()) {
        removeTrackAction->setEnabled(false);
        return;
    }

    if(settings->value<Settings::Gui::PlaybackFollowsCursor>()) {
        if(auto* playlist = playlistController->currentPlaylist()) {
            if(playlist->currentTrackIndex() != firstIndex) {
                if(playlistController->playState() != PlayState::Playing) {
                    playlist->changeCurrentTrack(firstIndex);
                }
                else {
                    playlist->scheduleNextIndex(firstIndex);
                }
                playlistController->playlistHandler()->schedulePlaylist(playlist);
            }
        }
    }
    removeTrackAction->setEnabled(true);
}

void PlaylistWidgetPrivate::trackIndexesChanged(int playingIndex) const
{
    const TrackList tracks = getAllTracks(model, {QModelIndex{}});

    if(!tracks.empty()) {
        const QModelIndexList selected = playlistView->selectionModel()->selectedIndexes();
        if(!selected.empty()) {
            const int firstIndex           = selected.front().data(PlaylistItem::Role::Index).toInt();
            const TrackList selectedTracks = getAllTracks(model, selected);
            selectionController->changeSelectedTracks(playlistContext, firstIndex, selectedTracks);
        }
    }

    playlistController->playlistHandler()->clearSchedulePlaylist();

    if(auto* playlist = playlistController->currentPlaylist()) {
        playlist->replaceTracks(tracks);
        if(playingIndex >= 0) {
            playlist->changeCurrentTrack(playingIndex);
        }
        model->updateHeader(playlist);
    }
}

void PlaylistWidgetPrivate::scanDroppedTracks(const QList<QUrl>& urls, int index)
{
    playlistView->setFocus(Qt::ActiveWindowFocusReason);

    playlistController->filesToTracks(urls, [this, index](const TrackList& tracks) {
        auto* insertCmd = new InsertTracks(model, {{index, tracks}});
        playlistController->addToHistory(insertCmd);
    });
}

void PlaylistWidgetPrivate::tracksInserted(const TrackGroups& tracks) const
{
    auto* insertCmd = new InsertTracks(model, tracks);
    playlistController->addToHistory(insertCmd);

    playlistView->setFocus(Qt::ActiveWindowFocusReason);
}

void PlaylistWidgetPrivate::tracksRemoved() const
{
    const auto selected = playlistView->selectionModel()->selectedIndexes();

    QModelIndexList trackSelection;
    IndexSet indexes;

    for(const QModelIndex& index : selected) {
        if(index.isValid() && index.data(PlaylistItem::Type).toInt() == PlaylistItem::Track) {
            trackSelection.push_back(index);
            indexes.emplace(index.data(PlaylistItem::Index).toInt());
        }
    }

    playlistController->playlistHandler()->clearSchedulePlaylist();

    if(auto* playlist = playlistController->currentPlaylist()) {
        playlist->removeTracks(indexes);

        auto* delCmd = new RemoveTracks(model, model->saveTrackGroups(trackSelection));
        playlistController->addToHistory(delCmd);

        model->updateHeader(playlist);
    }
}

void PlaylistWidgetPrivate::tracksMoved(const MoveOperation& operation) const
{
    auto* moveCmd = new MoveTracks(model, operation);
    playlistController->addToHistory(moveCmd);

    playlistView->setFocus(Qt::ActiveWindowFocusReason);
}

void PlaylistWidgetPrivate::playlistTracksAdded(Playlist* playlist, const TrackList& tracks, int index) const
{
    if(playlistController->currentPlaylist() == playlist) {
        auto* insertCmd = new InsertTracks(model, {{index, tracks}});
        playlistController->addToHistory(insertCmd);
    }
}

void PlaylistWidgetPrivate::handleTracksChanged(Playlist* playlist, const std::vector<int>& indexes)
{
    if(playlist != currentPlaylist) {
        return;
    }

    std::vector<int> selected;

    saveState();
    playlistView->setUpdatesEnabled(false);

    const auto changedTrackCount = static_cast<int>(indexes.size());
    // It's faster to just reset if we're going to be updating more than half the playlist
    // or we're updating a large number of tracks
    if(changedTrackCount > 500 || changedTrackCount > (playlist->trackCount() / 2)) {
        model->reset(currentPreset, columnMode ? columns : PlaylistColumnList{}, playlist);
    }
    else {
        selected = selectedPlaylistIndexes();
        model->updateTracks(indexes);
    }

    QObject::connect(
        model, &PlaylistModel::playlistTracksChanged, this,
        [this, selected]() {
            restoreState();
            restoreSelectedPlaylistIndexes(selected);

            playlistView->setUpdatesEnabled(true);
        },
        Qt::SingleShotConnection);
}

void PlaylistWidgetPrivate::toggleColumnMode()
{
    columnMode = !columnMode;
    self->finalise();

    if(columnMode && columns.empty()) {
        columns.push_back(columnRegistry.itemByName(QStringLiteral("Playing")));
        columns.push_back(columnRegistry.itemByName(QStringLiteral("Track")));
        columns.push_back(columnRegistry.itemByName(QStringLiteral("Title")));
        columns.push_back(columnRegistry.itemByName(QStringLiteral("Artist")));
        columns.push_back(columnRegistry.itemByName(QStringLiteral("Album")));
        columns.push_back(columnRegistry.itemByName(QStringLiteral("Duration")));

        QObject::connect(
            header, &QHeaderView::sectionCountChanged, self,
            [this]() {
                header->setHeaderSectionWidths({{0, 0.03}, {1, 0.05}, {2, 0.35}, {3, 0.25}, {4, 0.25}, {5, 0.07}});
            },
            Qt::SingleShotConnection);
    }

    resetModel();
}

void PlaylistWidgetPrivate::customHeaderMenuRequested(const QPoint& pos)
{
    auto* menu = new QMenu(self);
    menu->setAttribute(Qt::WA_DeleteOnClose);

    if(columnMode) {
        auto* filterList = new QActionGroup{menu};
        filterList->setExclusionPolicy(QActionGroup::ExclusionPolicy::None);

        auto hasColumn = [this](int id) {
            return std::ranges::any_of(columns, [id](const PlaylistColumn& column) { return column.id == id; });
        };

        for(const auto& column : columnRegistry.items()) {
            auto* columnAction = new QAction(column.name, menu);
            columnAction->setData(column.id);
            columnAction->setCheckable(true);
            columnAction->setChecked(hasColumn(column.id));
            menu->addAction(columnAction);
            filterList->addAction(columnAction);
        }

        menu->setDefaultAction(filterList->checkedAction());
        QObject::connect(filterList, &QActionGroup::triggered, self, [this](QAction* action) {
            const int columnId = action->data().toInt();
            if(action->isChecked()) {
                const PlaylistColumn column = columnRegistry.itemById(action->data().toInt());
                if(column.isValid()) {
                    columns.push_back(column);
                    changePreset(currentPreset);
                }
            }
            else {
                std::erase_if(columns,
                              [columnId](const PlaylistColumn& filterCol) { return filterCol.id == columnId; });
                changePreset(currentPreset);
            }
        });

        menu->addSeparator();
        header->addHeaderContextMenu(menu, self->mapToGlobal(pos));
        menu->addSeparator();
        header->addHeaderAlignmentMenu(menu, self->mapToGlobal(pos));
        menu->addSeparator();
    }

    auto* columnModeAction = new QAction(tr("Multi-column Mode"), menu);
    columnModeAction->setCheckable(true);
    columnModeAction->setChecked(columnMode);
    QObject::connect(columnModeAction, &QAction::triggered, self, [this]() { toggleColumnMode(); });
    menu->addAction(columnModeAction);

    menu->addSeparator();

    addPlaylistMenu(menu);
    addPresetMenu(menu);

    menu->popup(self->mapToGlobal(pos));
}

void PlaylistWidgetPrivate::doubleClicked(const QModelIndex& /*index*/) const
{
    selectionController->executeAction(TrackAction::Play);
    model->currentTrackChanged(playlistController->currentTrack());
    playlistView->clearSelection();
}

void PlaylistWidgetPrivate::followCurrentTrack(const Track& track, int index) const
{
    if(!settings->value<Settings::Gui::CursorFollowsPlayback>()) {
        return;
    }

    if(playlistController->playState() != PlayState::Playing) {
        return;
    }

    if(!track.isValid()) {
        return;
    }

    const QModelIndex modelIndex = model->indexAtTrackIndex(index);
    if(!modelIndex.isValid()) {
        return;
    }

    const QRect indexRect    = playlistView->visualRect(modelIndex);
    const QRect viewportRect = playlistView->viewport()->rect();

    if((indexRect.top() < 0) || (viewportRect.bottom() - indexRect.bottom() < 0)) {
        playlistView->scrollTo(modelIndex, QAbstractItemView::PositionAtCenter);
    }

    playlistView->setCurrentIndex(modelIndex);
}

QCoro::Task<void> PlaylistWidgetPrivate::sortTracks(QString script)
{
    if(auto* playlist = playlistController->currentPlaylist()) {
        if(playlistView->selectionModel()->hasSelection()) {
            const auto selected = playlistView->selectionModel()->selectedRows();

            std::vector<int> indexesToSort;

            for(const QModelIndex& index : selected) {
                if(index.data(PlaylistItem::Type).toInt() == PlaylistItem::Track) {
                    indexesToSort.push_back(index.data(PlaylistItem::Index).toInt());
                }
            }

            std::ranges::sort(indexesToSort);

            const auto sortedTracks = co_await Utils::asyncExec([script, playlist, indexesToSort]() {
                return Sorting::calcSortTracks(script, playlist->tracks(), indexesToSort);
            });
            playlist->replaceTracks(sortedTracks);
            changePlaylist(playlist);
        }
        else {
            const auto sortedTracks = co_await Utils::asyncExec(
                [script, playlist]() { return Sorting::calcSortTracks(script, playlist->tracks()); });
            playlist->replaceTracks(sortedTracks);
            changePlaylist(playlist);
        }
    }
}

QCoro::Task<void> PlaylistWidgetPrivate::sortColumn(int column, Qt::SortOrder order)
{
    if(column < 0) {
        co_return;
    }

    if(auto* playlist = playlistController->currentPlaylist()) {
        const auto sortedTracks = co_await Utils::asyncExec([this, playlist, column, order]() {
            return Sorting::calcSortTracks(columns.at(column).field, playlist->tracks(), order);
        });
        playlist->replaceTracks(sortedTracks);
        changePlaylist(playlist);
    }
}

void PlaylistWidgetPrivate::addSortMenu(QMenu* parent)
{
    auto* sortMenu = new QMenu(PlaylistWidget::tr("Sort"), parent);

    const auto& groups = sortRegistry.items();
    for(const auto& script : groups) {
        auto* switchSort = new QAction(script.name, sortMenu);
        QObject::connect(switchSort, &QAction::triggered, self, [this, script]() { sortTracks(script.script); });
        sortMenu->addAction(switchSort);
    }
    parent->addMenu(sortMenu);
}

void PlaylistWidgetPrivate::addPresetMenu(QMenu* parent)
{
    auto* presetsMenu = new QMenu(PlaylistWidget::tr("Presets"), parent);

    const auto& presets = presetRegistry.items();

    for(const auto& preset : presets) {
        auto* switchPreset = new QAction(preset.name, presetsMenu);
        if(preset == currentPreset) {
            presetsMenu->setDefaultAction(switchPreset);
        }

        const int presetId = preset.id;
        QObject::connect(switchPreset, &QAction::triggered, self,
                         [this, presetId]() { settings->set<PlaylistCurrentPreset>(presetId); });

        presetsMenu->addAction(switchPreset);
    }

    parent->addMenu(presetsMenu);
}

void PlaylistWidgetPrivate::addPlaylistMenu(QMenu* parent)
{
    auto* playlistMenu = new QMenu(Fooyin::PlaylistWidget::tr("Playlists"), parent);

    const auto* currPlaylist = playlistController->currentPlaylist();
    const auto playlists     = playlistController->playlists();

    for(const auto& playlist : playlists) {
        if(playlist != currPlaylist) {
            auto* switchPl = new QAction(playlist->name(), playlistMenu);
            const int id   = playlist->id();
            QObject::connect(switchPl, &QAction::triggered, playlistMenu,
                             [this, id]() { playlistController->changeCurrentPlaylist(id); });
            playlistMenu->addAction(switchPl);
        }
    }

    parent->addMenu(playlistMenu);
}

PlaylistWidget::PlaylistWidget(ActionManager* actionManager, PlaylistController* playlistController,
                               MusicLibrary* library, SettingsManager* settings, QWidget* parent)
    : FyWidget{parent}
    , p{std::make_unique<PlaylistWidgetPrivate>(this, actionManager, playlistController, library, settings)}
{
    setObjectName(PlaylistWidget::name());
}

PlaylistWidget::~PlaylistWidget()
{
    if(p->settings->value<Settings::Gui::RememberPlaylistState>() && p->currentPlaylist) {
        p->playlistController->savePlaylistState(p->currentPlaylist->id(), p->getState());
    }
}

QString PlaylistWidget::name() const
{
    return QStringLiteral("Playlist");
}

void PlaylistWidget::saveLayoutData(QJsonObject& layout)
{
    if(p->columnMode) {
        QStringList columns;

        for(int i{0}; const auto& column : p->columns) {
            const auto alignment = p->model->columnAlignment(i++);
            QString colStr       = QString::number(column.id);

            if(alignment != Qt::AlignLeft) {
                colStr += QStringLiteral(":") + QString::number(alignment.toInt());
            }

            columns.push_back(colStr);
        }

        layout[QStringLiteral("Columns")] = columns.join(QStringLiteral("|"));
    }

    QByteArray state = p->header->saveHeaderState();
    state            = qCompress(state, 9);

    layout[QStringLiteral("State")] = QString::fromUtf8(state.toBase64());
}

void PlaylistWidget::loadLayoutData(const QJsonObject& layout)
{
    if(layout.contains(QStringLiteral("Columns"))) {
        p->columns.clear();

        const QString columnData    = layout.value(QStringLiteral("Columns")).toString();
        const QStringList columnIds = columnData.split(QStringLiteral("|"));

        for(int i{0}; const auto& columnId : columnIds) {
            const auto column     = columnId.split(QStringLiteral(":"));
            const auto columnItem = p->columnRegistry.itemById(column.at(0).toInt());

            if(columnItem.isValid()) {
                p->columns.push_back(columnItem);

                if(column.size() > 1) {
                    p->model->changeColumnAlignment(i, static_cast<Qt::Alignment>(column.at(1).toInt()));
                }
            }
            ++i;
        }

        p->columnMode = !p->columns.empty();
    }

    p->resetModel();

    if(layout.contains(QStringLiteral("State"))) {
        auto state = QByteArray::fromBase64(layout.value(QStringLiteral("State")).toString().toUtf8());

        if(state.isEmpty()) {
            return;
        }

        state = qUncompress(state);

        // Workaround to ensure QHeaderView section count is updated before restoring state
        QMetaObject::invokeMethod(p->model, "headerDataChanged", Q_ARG(Qt::Orientation, Qt::Horizontal), Q_ARG(int, 0),
                                  Q_ARG(int, 0));

        p->header->restoreHeaderState(state);
    }
}

void PlaylistWidget::finalise()
{
    p->header->setSectionsClickable(p->columnMode);
    p->header->setSortIndicatorShown(p->columnMode);
}

void PlaylistWidget::contextMenuEvent(QContextMenuEvent* event)
{
    auto* menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);

    if(auto* removeCmd = p->actionManager->command(Constants::Actions::Remove)) {
        menu->addAction(removeCmd->action());
    }

    menu->addSeparator();

    p->addSortMenu(menu);
    p->selectionController->addTrackContextMenu(menu);

    menu->popup(event->globalPos());
}

void PlaylistWidget::keyPressEvent(QKeyEvent* event)
{
    const auto key = event->key();

    if(key == Qt::Key_Enter || key == Qt::Key_Return) {
        if(p->selectionController->hasTracks()) {
            p->selectionController->executeAction(TrackAction::Play);
        }
        p->playlistView->clearSelection();
    }
    QWidget::keyPressEvent(event);
}
} // namespace Fooyin

#include "moc_playlistwidget.cpp"
