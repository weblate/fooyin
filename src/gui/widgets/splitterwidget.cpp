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

#include "splitterwidget.h"

#include "dummy.h"
#include "internalguisettings.h"

#include <gui/widgetprovider.h>
#include <utils/actions/actioncontainer.h>
#include <utils/actions/actionmanager.h>
#include <utils/enum.h>
#include <utils/settings/settingsmanager.h>

#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QMenu>
#include <QSplitter>

namespace {
Fooyin::ActionContainer* createNewMenu(Fooyin::ActionManager* actionManager, Fooyin::FyWidget* parent,
                                       const QString& title)
{
    auto id       = parent->id().append(title);
    auto* newMenu = actionManager->createMenu(id);
    newMenu->menu()->setTitle(title);
    return newMenu;
}
} // namespace

namespace Fooyin {
class SplitterHandle : public QSplitterHandle
{
    Q_OBJECT

public:
    explicit SplitterHandle(Qt::Orientation type, QSplitter* parent = nullptr)
        : QSplitterHandle{type, parent}
    { }

    void showHandle(bool show)
    {
        m_showHandle = show;
        update();
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        if(m_showHandle) {
            QSplitterHandle::paintEvent(event);
        }
    };

private:
    bool m_showHandle{true};
};

class Splitter : public QSplitter
{
    Q_OBJECT

public:
    explicit Splitter(Qt::Orientation type, SettingsManager* settings, QWidget* parent = nullptr)
        : QSplitter{type, parent}
        , m_settings{settings}
    {
        setObjectName(QStringLiteral("Splitter"));
        setChildrenCollapsible(false);
    }

protected:
    QSplitterHandle* createHandle() override
    {
        auto* handle = new SplitterHandle(orientation(), this);
        handle->showHandle(m_settings->value<Settings::Gui::Internal::SplitterHandles>());
        m_settings->subscribe<Settings::Gui::Internal::SplitterHandles>(handle, &SplitterHandle::showHandle);
        return handle;
    };

private:
    SettingsManager* m_settings;
};

struct SplitterWidget::Private
{
    SplitterWidget* self;

    SettingsManager* settings;
    ActionManager* actionManager;
    WidgetProvider* widgetProvider;

    Splitter* splitter;
    WidgetList children;
    Dummy* dummy{nullptr};

    int limit{0};
    bool showDummy{false};
    int widgetCount{0};
    int baseWidgetCount{0};

    Private(SplitterWidget* self_, ActionManager* actionManager_, WidgetProvider* widgetProvider_,
            SettingsManager* settings_)
        : self{self_}
        , settings{settings_}
        , actionManager{actionManager_}
        , widgetProvider{widgetProvider_}
        , splitter{new Splitter(Qt::Vertical, settings, self)}
    { }

    void checkShowDummy()
    {
        if(!dummy && showDummy && widgetCount == 0) {
            dummy = new Dummy(self);
            splitter->addWidget(dummy);
        }
        else if(dummy && !showDummy && widgetCount > 0) {
            dummy->deleteLater();
            dummy = nullptr;
        }
    }

    int findIndex(FyWidget* widgetToFind) const
    {
        const auto it = std::ranges::find(std::as_const(children), widgetToFind);
        if(it != children.cend()) {
            return static_cast<int>(std::distance(children.cbegin(), it));
        }
        return -1;
    }

    void replaceWidget(int index, FyWidget* widget)
    {
        if(!widget || children.empty()) {
            return;
        }

        if(index < 0 || index >= self->childCount()) {
            return;
        }

        FyWidget* oldWidget = children.at(index);
        children.erase(children.begin() + index);
        oldWidget->deleteLater();

        children.insert(children.begin() + index, widget);
        splitter->insertWidget(index, widget);
    }
};

SplitterWidget::SplitterWidget(ActionManager* actionManager, WidgetProvider* widgetProvider, SettingsManager* settings,
                               QWidget* parent)
    : WidgetContainer{widgetProvider, parent}
    , p{std::make_unique<Private>(this, actionManager, widgetProvider, settings)}
{
    setObjectName(SplitterWidget::name());

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(p->splitter);

    p->checkShowDummy();
}

void SplitterWidget::setWidgetLimit(int count)
{
    p->limit = count;
}

void SplitterWidget::showPlaceholder(bool show)
{
    p->showDummy = show;

    p->checkShowDummy();
}

SplitterWidget::~SplitterWidget() = default;

Qt::Orientation SplitterWidget::orientation() const
{
    return p->splitter->orientation();
}

void SplitterWidget::setOrientation(Qt::Orientation orientation)
{
    p->splitter->setOrientation(orientation);
    setObjectName(SplitterWidget::name());
}

QByteArray SplitterWidget::saveState() const
{
    return p->splitter->saveState();
}

bool SplitterWidget::restoreState(const QByteArray& state)
{
    return p->splitter->restoreState(state);
}

int SplitterWidget::childCount()
{
    return static_cast<int>(p->children.size());
}

void SplitterWidget::addWidget(FyWidget* widget)
{
    if(p->limit > 0 && p->widgetCount >= p->limit) {
        return;
    }

    if(!widget) {
        return;
    }

    p->widgetCount += 1;

    if(p->dummy && p->widgetCount > 0) {
        p->dummy->deleteLater();
        p->dummy = nullptr;
    }

    const int index = childCount() + p->baseWidgetCount;
    p->children.push_back(widget);
    p->splitter->insertWidget(index, widget);
}

void SplitterWidget::replaceWidget(FyWidget* oldWidget, FyWidget* newWidget)
{
    if(!oldWidget || !newWidget || p->children.empty()) {
        return;
    }

    const int index = p->findIndex(oldWidget);
    p->replaceWidget(index, newWidget);
}

void SplitterWidget::removeWidget(FyWidget* widget)
{
    const int index = p->findIndex(widget);
    if(index < 0) {
        return;
    }

    p->widgetCount -= 1;

    widget->deleteLater();
    p->children.erase(p->children.begin() + index);

    p->checkShowDummy();
}

WidgetList SplitterWidget::widgets() const
{
    return p->children;
}

QString SplitterWidget::name() const
{
    return Utils::Enum::toString(p->splitter->orientation()) + QStringLiteral(" Splitter");
}

QString SplitterWidget::layoutName() const
{
    return QStringLiteral("Splitter") + Utils::Enum::toString(p->splitter->orientation());
}

void SplitterWidget::layoutEditingMenu(ActionContainer* menu)
{
    auto* changeSplitter = new QAction(tr("Change Splitter"), this);
    QObject::connect(changeSplitter, &QAction::triggered, this, [this] {
        setOrientation(p->splitter->orientation() == Qt::Horizontal ? Qt::Vertical : Qt::Horizontal);
        setObjectName(Utils::Enum::toString(p->splitter->orientation()) + QStringLiteral(" Splitter"));
    });
    menu->addAction(changeSplitter);

    if(p->limit > 0 && p->widgetCount >= p->limit) {
        return;
    }

    auto* addMenu = createNewMenu(p->actionManager, this, tr("&Add"));

    p->widgetProvider->setupWidgetMenu(addMenu, [this](FyWidget* newWidget) {
        addWidget(newWidget);
        newWidget->finalise();
    });

    menu->addMenu(addMenu);
}

void SplitterWidget::saveLayoutData(QJsonObject& layout)
{
    layout[QStringLiteral("State")] = QString::fromUtf8(saveState().toBase64());

    if(!p->children.empty()) {
        QJsonArray children;
        for(const auto& widget : p->children) {
            widget->saveLayout(children);
        }
        layout[QStringLiteral("Widgets")] = children;
    }
}

void SplitterWidget::loadLayoutData(const QJsonObject& layout)
{
    const auto state    = QByteArray::fromBase64(layout[QStringLiteral("State")].toString().toUtf8());
    const auto children = layout[QStringLiteral("Widgets")].toArray();

    WidgetContainer::loadWidgets(children);

    restoreState(state);

    p->checkShowDummy();
}
} // namespace Fooyin

#include "moc_splitterwidget.cpp"
#include "splitterwidget.moc"
