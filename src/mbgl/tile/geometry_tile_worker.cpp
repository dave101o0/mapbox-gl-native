#include <mbgl/tile/geometry_tile_worker.hpp>
#include <mbgl/tile/geometry_tile_data.hpp>
#include <mbgl/tile/geometry_tile.hpp>
#include <mbgl/text/collision_tile.hpp>
#include <mbgl/layout/symbol_layout.hpp>
#include <mbgl/sprite/sprite_atlas.hpp>
#include <mbgl/style/bucket_parameters.hpp>
#include <mbgl/style/group_by_layout.hpp>
#include <mbgl/style/filter.hpp>
#include <mbgl/style/filter_evaluator.hpp>
#include <mbgl/style/layers/symbol_layer.hpp>
#include <mbgl/style/layers/symbol_layer_impl.hpp>
#include <mbgl/renderer/symbol_bucket.hpp>
#include <mbgl/util/logging.hpp>
#include <mbgl/util/constants.hpp>
#include <mbgl/util/string.hpp>
#include <mbgl/util/exception.hpp>

#include <unordered_set>

namespace mbgl {

using namespace style;

GeometryTileWorker::GeometryTileWorker(ActorRef<GeometryTileWorker> self_,
                                       ActorRef<GeometryTile> parent_,
                                       OverscaledTileID id_,
                                       const std::atomic<bool>& obsolete_,
                                       const MapMode mode_)
    : self(std::move(self_)),
      parent(std::move(parent_)),
      id(std::move(id_)),
      obsolete(obsolete_),
      mode(mode_),
      waitingForGlyphs(false),
      waitingForIcons(false) {
}

GeometryTileWorker::~GeometryTileWorker() {
}

/*
   GeometryTileWorker is a state machine. This is its transition diagram.
   States are indicated by [state], lines are transitions triggered by
   messages, (parentheses) are actions taken on transition.

                              [idle] <----------------------------.
                                 |                                |
                       set{Data,Layers,Placement}                 |
                                 |                                |
           (do layout/placement; self-send "coalesced")           |
                                 v                                |
                           [coalescing] --- coalesced ------------.
                               |   |
             .-----------------.   .---------------.
             |                                     |
   .--- set{Data,Layers}                      setPlacement -----.
   |         |                                     |            |
   |         v                                     v            |
   .-- [need layout] <-- set{Data,Layers} -- [need placement] --.
             |                                     |
         coalesced                             coalesced
             |                                     |
             v                                     v
    (do layout or placement; self-send "coalesced"; goto [coalescing])

   The idea is that in the [idle] state, layout or placement happens immediately
   in response to a "set" message. During this processing, multiple "set" messages
   might get queued in the mailbox. At the end of processing, we self-send "coalesced",
   read all the queued messages until we get to "coalesced", and then redo either
   layout or placement if there were one or more "set"s (with layout taking priority,
   since it will trigger placement when complete), or return to the [idle] state if not.
   
   TODO: The transition diagram is complicated by handling symbol dependencies
   asynchronously (insert link to updated diagram or ASCII-fy it)
*/

void GeometryTileWorker::setData(std::unique_ptr<const GeometryTileData> data_, uint64_t correlationID_) {
    try {
        data = std::move(data_);
        correlationID = correlationID_;

        switch (state) {
        case Idle:
            redoLayout();
            break;

        case Coalescing:
        case NeedPlacement:
            state = NeedLayout;
            break;
        case NeedLayout:
            break;
        }
    } catch (...) {
        parent.invoke(&GeometryTile::onError, std::current_exception());
    }
}

void GeometryTileWorker::setLayers(std::vector<std::unique_ptr<Layer>> layers_, uint64_t correlationID_) {
    try {
        layers = std::move(layers_);
        correlationID = correlationID_;

        switch (state) {
        case Idle:
            redoLayout();
            break;

        case Coalescing:
        case NeedPlacement:
            state = NeedLayout;
            break;

        case NeedLayout:
            break;
        }
    } catch (...) {
        parent.invoke(&GeometryTile::onError, std::current_exception());
    }
}

void GeometryTileWorker::setPlacementConfig(PlacementConfig placementConfig_, uint64_t correlationID_) {
    try {
        bool firstPlacement = !placementConfig;
        placementConfig = std::move(placementConfig_);
        correlationID = correlationID_;

        switch (state) {
        case Idle:
            attemptPlacement();
            break;

        case Coalescing:
            state = NeedPlacement;
            break;

        case NeedPlacement:
            if (firstPlacement) {
                attemptPlacement();
            }
            break;
        case NeedLayout:
            if (firstPlacement && !hasPendingSymbolDependencies()) {
                redoLayout();
            }
            break;
        }
    } catch (...) {
        parent.invoke(&GeometryTile::onError, std::current_exception());
    }
}

void GeometryTileWorker::onGlyphsAvailable(GlyphPositionMap glyphs) {
    assert(waitingForGlyphs);
    glyphPositions = std::move(glyphs);
    waitingForGlyphs = false;
    symbolDependenciesChanged();
}

void GeometryTileWorker::onIconsAvailable(IconAtlasMap icons_) {
    assert(waitingForIcons);
    icons = std::move(icons_);
    waitingForIcons = false;
    symbolDependenciesChanged();
}

bool GeometryTileWorker::hasPendingSymbolDependencies() const {
    return waitingForGlyphs || waitingForIcons;
}

void GeometryTileWorker::symbolDependenciesChanged() {
    try {
        switch (state) {
        case NeedPlacement:
            attemptPlacement();
            break;

        case NeedLayout:
            if (!hasPendingSymbolDependencies()) {
                redoLayout();
            }
            break;
    
        case Idle:
        case Coalescing:
            assert(false);
            break;
        }
    } catch (...) {
        parent.invoke(&GeometryTile::onError, std::current_exception());
    }
}

void GeometryTileWorker::coalesced() {
    try {
        switch (state) {
        case Idle:
            assert(false);
            break;

        case Coalescing:
            state = Idle;
            break;

        case NeedLayout:
            if (!hasPendingSymbolDependencies()) {
                // Don't re-trigger layout if we have outstanding symbol dependencies
                // Redoing layout could invalidate the outstanding dependencies, but
                // when they arrived we would treat them as valid
                redoLayout();
            }
            break;

        case NeedPlacement:
            attemptPlacement();
            break;
        }
    } catch (...) {
        parent.invoke(&GeometryTile::onError, std::current_exception());
    }
}

bool GeometryTileWorker::hasGlyphDependencies(const GlyphDependencies& glyphDependencies) const {
    for (auto fontDependencies : glyphDependencies) {
        auto fontGlyphs = glyphPositions.find(fontDependencies.first);
        if (fontGlyphs == glyphPositions.end()) {
            return false;
        }
        for (auto glyphID : fontDependencies.second) {
            if (fontGlyphs->second.find(glyphID) == fontGlyphs->second.end()) {
                return false;
            }
        }
    }
    return true;
}

bool GeometryTileWorker::hasIconDependencies(const IconDependencyMap &iconDependencies) const {
    for (auto atlasDependency : iconDependencies) {
        if (icons.find((uintptr_t)atlasDependency.first) == icons.end()) {
            return false;
        }
    }
    return true;
}

void GeometryTileWorker::redoLayout() {
    if (!data || !layers) {
        return;
    }

    std::vector<std::string> symbolOrder;
    for (auto it = layers->rbegin(); it != layers->rend(); it++) {
        if ((*it)->is<SymbolLayer>()) {
            symbolOrder.push_back((*it)->getID());
        }
    }

    std::unordered_map<std::string, std::unique_ptr<SymbolLayout>> symbolLayoutMap;
    std::unordered_map<std::string, std::shared_ptr<Bucket>> buckets;
    auto featureIndex = std::make_unique<FeatureIndex>();
    BucketParameters parameters { id, mode };
    
    GlyphDependencies glyphDependencies;
    IconDependencyMap iconDependencyMap;

    std::vector<std::vector<const Layer*>> groups = groupByLayout(*layers);
    for (auto& group : groups) {
        if (obsolete) {
            return;
        }

        if (!*data) {
            continue; // Tile has no data.
        }

        const Layer& leader = *group.at(0);

        auto geometryLayer = (*data)->getLayer(leader.baseImpl->sourceLayer);
        if (!geometryLayer) {
            continue;
        }

        std::vector<std::string> layerIDs;
        for (const auto& layer : group) {
            layerIDs.push_back(layer->getID());
        }

        featureIndex->setBucketLayerIDs(leader.getID(), layerIDs);

        if (leader.is<SymbolLayer>()) {
            symbolLayoutMap.emplace(leader.getID(),
                leader.as<SymbolLayer>()->impl->createLayout(parameters, group, *geometryLayer, glyphDependencies, iconDependencyMap));
        } else {
            const Filter& filter = leader.baseImpl->filter;
            const std::string& sourceLayerID = leader.baseImpl->sourceLayer;
            std::shared_ptr<Bucket> bucket = leader.baseImpl->createBucket(parameters, group);

            for (std::size_t i = 0; !obsolete && i < geometryLayer->featureCount(); i++) {
                std::unique_ptr<GeometryTileFeature> feature = geometryLayer->getFeature(i);

                if (!filter(feature->getType(), feature->getID(), [&] (const auto& key) { return feature->getValue(key); }))
                    continue;

                GeometryCollection geometries = feature->getGeometries();
                bucket->addFeature(*feature, geometries);
                featureIndex->insert(geometries, i, sourceLayerID, leader.getID());
            }

            if (!bucket->hasData()) {
                continue;
            }

            for (const auto& layer : group) {
                buckets.emplace(layer->getID(), bucket);
            }
        }
    }

    symbolLayouts.clear();
    for (const auto& symbolLayerID : symbolOrder) {
        auto it = symbolLayoutMap.find(symbolLayerID);
        if (it != symbolLayoutMap.end()) {
            symbolLayouts.push_back(std::move(it->second));
        }
    }
    
    if (!hasGlyphDependencies(glyphDependencies)) {
        glyphPositions.clear();
        waitingForGlyphs = true;
        parent.invoke(&GeometryTile::getGlyphs, std::move(glyphDependencies));
    }
    
    if (!hasIconDependencies(iconDependencyMap)) {
        icons.clear();
        waitingForIcons = true;
        parent.invoke(&GeometryTile::getIcons, std::move(iconDependencyMap));
    }

    parent.invoke(&GeometryTile::onLayout, GeometryTile::LayoutResult {
        std::move(buckets),
        std::move(featureIndex),
        *data ? (*data)->clone() : nullptr,
        correlationID
    });

    if (!placementConfig || hasPendingSymbolDependencies()) {
        state = NeedPlacement;
    } else {
        attemptPlacement();
    }
}

void GeometryTileWorker::attemptPlacement() {
    if (!data || !layers || !placementConfig || hasPendingSymbolDependencies()) {
        return;
    }
    
    auto collisionTile = std::make_unique<CollisionTile>(*placementConfig);
    std::unordered_map<std::string, std::shared_ptr<Bucket>> buckets;

    for (auto& symbolLayout : symbolLayouts) {
        if (obsolete) {
            return;
        }
        
        if (symbolLayout->state == SymbolLayout::Pending) {
            symbolLayout->prepare(glyphPositions,icons);
        }

        symbolLayout->state = SymbolLayout::Placed;
        if (!symbolLayout->hasSymbolInstances()) {
            continue;
        }

        std::shared_ptr<Bucket> bucket = symbolLayout->place(*collisionTile);
        for (const auto& pair : symbolLayout->layerPaintProperties) {
            buckets.emplace(pair.first, bucket);
        }
    }

    parent.invoke(&GeometryTile::onPlacement, GeometryTile::PlacementResult {
        std::move(buckets),
        std::move(collisionTile),
        correlationID
    });
    
    state = Coalescing;
    self.invoke(&GeometryTileWorker::coalesced);
}

} // namespace mbgl
