// Copyright (c) 2024 UltiMaker
// CuraEngine is released under the terms of the AGPLv3 or higher

#include <algorithm>
#include <atomic>
#include <fstream> // ifstream.good()
#include <map> // multimap (ordered map allowing duplicate keys)
#include <numeric>

#include <spdlog/spdlog.h>

// Code smell: Order of the includes is important here, probably due to some forward declarations which might be masking some undefined behaviours
// clang-format off
#include "Application.h"
#include "ConicalOverhang.h"
#include "ExtruderTrain.h"
#include "FffPolygonGenerator.h"
#include "infill.h"
#include "settings/HeightParameterGraph.h"
#include "InterlockingGenerator.h"
#include "layerPart.h"
#include "MeshGroup.h"
#include "Mold.h"
#include "multiVolumes.h"
#include "PrintFeature.h"
#include "raft.h"
#include "skin.h"
#include "SkirtBrim.h"
#include "Slice.h"
#include "sliceDataStorage.h"
#include "slicer.h"
#include "support.h"
#include "TopSurface.h"
#include "TreeSupport.h"
#include "WallsComputation.h"
#include "infill/DensityProvider.h"
#include "infill/ImageBasedDensityProvider.h"
#include "infill/LightningGenerator.h"
#include "infill/SierpinskiFillProvider.h"
#include "infill/SubDivCube.h"
#include "infill/UniformDensityProvider.h"
#include "progress/Progress.h"
#include "progress/ProgressEstimator.h"
#include "progress/ProgressEstimatorLinear.h"
#include "progress/ProgressStageEstimator.h"
#include "settings/AdaptiveLayerHeights.h"
#include "settings/HeightParameterGraph.h"
#include "settings/types/Angle.h"
#include "settings/types/LayerIndex.h"
#include "utils/algorithm.h"
#include "utils/ThreadPool.h"
#include "utils/gettime.h"
#include "utils/math.h"
#include "PrimeTower/PrimeTower.h"
#include "geometry/OpenPolyline.h"
#include "utils/Simplify.h"
// clang-format on

namespace cura
{


bool FffPolygonGenerator::generateAreas(SliceDataStorage& storage, MeshGroup* meshgroup, TimeKeeper& timeKeeper)
{
    if (! sliceModel(meshgroup, timeKeeper, storage))
    {
        return false;
    }

    slices2polygons(storage, timeKeeper);

    return true;
}

size_t FffPolygonGenerator::getDraftShieldLayerCount(const size_t total_layers) const
{
    const Settings& mesh_group_settings = Application::getInstance().current_slice_->scene.current_mesh_group->settings;
    if (! mesh_group_settings.get<bool>("draft_shield_enabled"))
    {
        return 0;
    }
    switch (mesh_group_settings.get<DraftShieldHeightLimitation>("draft_shield_height_limitation"))
    {
    case DraftShieldHeightLimitation::FULL:
        return total_layers;
    case DraftShieldHeightLimitation::LIMITED:
        return std::max(
            (coord_t)0,
            (mesh_group_settings.get<coord_t>("draft_shield_height") - mesh_group_settings.get<coord_t>("layer_height_0")) / mesh_group_settings.get<coord_t>("layer_height") + 1);
    default:
        spdlog::warn("A draft shield height limitation option was added without implementing the new option in getDraftShieldLayerCount.");
        return total_layers;
    }
}

bool FffPolygonGenerator::sliceModel(MeshGroup* meshgroup, TimeKeeper& timeKeeper, SliceDataStorage& storage) /// slices the model
{
    Progress::messageProgressStage(Progress::Stage::SLICING, &timeKeeper);

    storage.model_min = meshgroup->min();
    storage.model_max = meshgroup->max();
    storage.model_size = storage.model_max - storage.model_min;

    spdlog::info("Slicing model...");

    const Settings& mesh_group_settings = Application::getInstance().current_slice_->scene.current_mesh_group->settings;

    // regular layers
    int slice_layer_count = 0; // Use signed int because we need to subtract the initial layer in a calculation temporarily.

    // Initial layer height of 0 is not allowed. Negative layer height is nonsense.
    coord_t initial_layer_thickness = mesh_group_settings.get<coord_t>("layer_height_0");
    if (initial_layer_thickness <= 0)
    {
        spdlog::error("Initial layer height {} is disallowed.", initial_layer_thickness);
        return false;
    }

    // Layer height of 0 is not allowed. Negative layer height is nonsense.
    const coord_t layer_thickness = mesh_group_settings.get<coord_t>("layer_height");
    if (layer_thickness <= 0)
    {
        spdlog::error("Layer height {} is disallowed.\n", layer_thickness);
        return false;
    }

    // variable layers
    AdaptiveLayerHeights* adaptive_layer_heights = nullptr;
    const bool use_variable_layer_heights = mesh_group_settings.get<bool>("adaptive_layer_height_enabled");

    if (use_variable_layer_heights)
    {
        // Calculate adaptive layer heights
        const auto variable_layer_height_max_variation = mesh_group_settings.get<coord_t>("adaptive_layer_height_variation");
        const auto variable_layer_height_variation_step = mesh_group_settings.get<coord_t>("adaptive_layer_height_variation_step");
        const auto adaptive_threshold = mesh_group_settings.get<coord_t>("adaptive_layer_height_threshold");

        // 用户定义层厚控制
        const bool user_thickness_definition_enable = mesh_group_settings.get<bool>("user_thickness_definition_enable");
        const HeightParameterGraph user_thickness_definition = mesh_group_settings.get<HeightParameterGraph>("user_thickness_definition");

        adaptive_layer_heights = new AdaptiveLayerHeights(
            layer_thickness,
            variable_layer_height_max_variation,
            variable_layer_height_variation_step,
            adaptive_threshold,
            meshgroup,
            user_thickness_definition_enable,
            user_thickness_definition
        );

        // Get the amount of layers
        slice_layer_count = adaptive_layer_heights->getLayerCount();
    }
    else
    {
        // Find highest layer count according to each mesh's settings.
        for (const Mesh& mesh : meshgroup->meshes)
        {
            if (! mesh.isPrinted())
            {
                continue;
            }
            const coord_t mesh_height = mesh.max().z_;
            switch (mesh.settings_.get<SlicingTolerance>("slicing_tolerance"))
            {
            case SlicingTolerance::MIDDLE:
                if (storage.model_max.z_ < initial_layer_thickness)
                {
                    slice_layer_count = std::max(slice_layer_count, (mesh_height > initial_layer_thickness / 2) ? 1 : 0); // One layer if higher than half initial layer height.
                }
                else
                {
                    slice_layer_count = std::max(slice_layer_count, static_cast<int>(round_divide_signed(mesh_height - initial_layer_thickness, layer_thickness) + 1));
                }
                break;
            case SlicingTolerance::EXCLUSIVE:
            {
                int new_slice_layer_count = 0;
                if (mesh_height >= initial_layer_thickness) // If less than the initial layer thickness, leave it at 0.
                {
                    new_slice_layer_count = static_cast<int>(floor_divide_signed(mesh_height - 1 - initial_layer_thickness, layer_thickness) + 1);
                }
                if (new_slice_layer_count > 0) // If there is at least one layer already, then...
                {
                    new_slice_layer_count += 1; // ... need one extra, since we clear the top layer after the repeated intersections with the layer above.
                }
                slice_layer_count = std::max(slice_layer_count, new_slice_layer_count);
                break;
            }
            case SlicingTolerance::INCLUSIVE:
                if (mesh_height < initial_layer_thickness)
                {
                    slice_layer_count
                        = std::max(slice_layer_count, (mesh_height > 0) ? 1 : 0); // If less than the initial layer height, it always has 1 layer unless the height is truly zero.
                }
                else
                {
                    slice_layer_count = std::max(slice_layer_count, static_cast<int>(ceil_divide_signed(mesh_height - initial_layer_thickness, layer_thickness) + 1));
                }
                break;
            default:
                spdlog::error("Unknown slicing tolerance. Did you forget to add a case here?");
                return false;
            }
        }
    }

    // Model is shallower than layer_height_0, so not even the first layer is sliced. Return an empty model then.
    if (slice_layer_count <= 0)
    {
        return true; // This is NOT an error state!
    }

    std::vector<Slicer*> slicerList;
    for (unsigned int mesh_idx = 0; mesh_idx < meshgroup->meshes.size(); mesh_idx++)
    {
        // Check if adaptive layers is populated to prevent accessing a method on NULL
        std::vector<AdaptiveLayer>* adaptive_layer_height_values = {};
        if (adaptive_layer_heights != nullptr)
        {
            adaptive_layer_height_values = adaptive_layer_heights->getLayers();
        }

        Mesh& mesh = meshgroup->meshes[mesh_idx];
        Slicer* slicer = new Slicer(&mesh, layer_thickness, slice_layer_count, use_variable_layer_heights, adaptive_layer_height_values);

        slicerList.push_back(slicer);

        Progress::messageProgress(Progress::Stage::SLICING, mesh_idx + 1, meshgroup->meshes.size());
    }

    // Clear the mesh face and vertex data, it is no longer needed after this point, and it saves a lot of memory.
    meshgroup->clear();

    Mold::process(slicerList);

    Scene& scene = Application::getInstance().current_slice_->scene;
    for (unsigned int mesh_idx = 0; mesh_idx < slicerList.size(); mesh_idx++)
    {
        Mesh& mesh = scene.current_mesh_group->meshes[mesh_idx];
        if (mesh.settings_.get<bool>("conical_overhang_enabled") && ! mesh.settings_.get<bool>("anti_overhang_mesh"))
        {
            ConicalOverhang::apply(slicerList[mesh_idx], mesh);
        }
    }

    MultiVolumes::carveCuttingMeshes(slicerList, scene.current_mesh_group->meshes);

    Progress::messageProgressStage(Progress::Stage::PARTS, &timeKeeper);

    if (scene.current_mesh_group->settings.get<bool>("carve_multiple_volumes"))
    {
        carveMultipleVolumes(slicerList);
    }

    generateMultipleVolumesOverlap(slicerList);


    if (Application::getInstance().current_slice_->scene.current_mesh_group->settings.get<bool>("interlocking_enable"))
    {
        InterlockingGenerator::generateInterlockingStructure(slicerList);
    }

    storage.print_layer_count = 0;
    for (unsigned int meshIdx = 0; meshIdx < slicerList.size(); meshIdx++)
    {
        Mesh& mesh = scene.current_mesh_group->meshes[meshIdx];
        Slicer* slicer = slicerList[meshIdx];
        if (! mesh.settings_.get<bool>("anti_overhang_mesh") && ! mesh.settings_.get<bool>("infill_mesh") && ! mesh.settings_.get<bool>("cutting_mesh"))
        {
            storage.print_layer_count = std::max(storage.print_layer_count, slicer->layers.size());
        }
    }
    storage.support.supportLayers.resize(storage.print_layer_count);

    storage.meshes.reserve(
        slicerList.size()); // causes there to be no resize in meshes so that the pointers in sliceMeshStorage._config to retraction_config don't get invalidated.
    for (unsigned int meshIdx = 0; meshIdx < slicerList.size(); meshIdx++)
    {
        Slicer* slicer = slicerList[meshIdx];
        Mesh& mesh = scene.current_mesh_group->meshes[meshIdx];

        // always make a new SliceMeshStorage, so that they have the same ordering / indexing as meshgroup.meshes
        storage.meshes.push_back(std::make_shared<SliceMeshStorage>(&meshgroup->meshes[meshIdx], slicer->layers.size())); // new mesh in storage had settings from the Mesh
        SliceMeshStorage& meshStorage = *storage.meshes.back();

        // only create layer parts for normal meshes
        const bool is_support_modifier = AreaSupport::handleSupportModifierMesh(storage, mesh.settings_, slicer);
        if (! is_support_modifier)
        {
            createLayerParts(meshStorage, slicer);
        }

        // Do not add and process support _modifier_ meshes further, and ONLY skip support _modifiers_. They have been
        // processed in AreaSupport::handleSupportModifierMesh(), but other helper meshes such as infill meshes are
        // processed in a later stage, except for support mesh itself, so an exception is made for that.
        if (is_support_modifier && ! mesh.settings_.get<bool>("support_mesh"))
        {
            storage.meshes.pop_back();
            continue;
        }

        // check one if raft offset is needed
        const bool has_raft = mesh_group_settings.get<EPlatformAdhesion>("adhesion_type") == EPlatformAdhesion::RAFT;

        // calculate the height at which each layer is actually printed (printZ)
        for (LayerIndex layer_nr = 0; layer_nr < meshStorage.layers.size(); layer_nr++)
        {
            SliceLayer& layer = meshStorage.layers[layer_nr];

            if (use_variable_layer_heights)
            {
                meshStorage.layers[layer_nr].printZ = adaptive_layer_heights->getLayers()->at(layer_nr).z_position_;
                meshStorage.layers[layer_nr].thickness = adaptive_layer_heights->getLayers()->at(layer_nr).layer_height_;
            }
            else
            {
                meshStorage.layers[layer_nr].printZ = initial_layer_thickness + (layer_nr * layer_thickness);

                if (layer_nr == 0)
                {
                    meshStorage.layers[layer_nr].thickness = initial_layer_thickness;
                }
                else
                {
                    meshStorage.layers[layer_nr].thickness = layer_thickness;
                }
            }

            // add the raft offset to each layer
            if (has_raft)
            {
                const ExtruderTrain& train = mesh_group_settings.get<ExtruderTrain&>("raft_surface_extruder_nr");
                layer.printZ += Raft::getTotalThickness() + train.settings_.get<coord_t>("raft_airgap")
                              - train.settings_.get<coord_t>("layer_0_z_overlap"); // shift all layers (except 0) down

                if (layer_nr == 0)
                {
                    layer.printZ += train.settings_.get<coord_t>("layer_0_z_overlap"); // undo shifting down of first layer
                }
            }
        }

        delete slicerList[meshIdx];

        Progress::messageProgress(Progress::Stage::PARTS, meshIdx + 1, slicerList.size());
    }
    return true;
}

void FffPolygonGenerator::slices2polygons(SliceDataStorage& storage, TimeKeeper& time_keeper)
{
    // compute layer count and remove first empty layers
    // there is no separate progress stage for removeEmptyFisrtLayer (TODO)
    unsigned int slice_layer_count = 0;
    for (std::shared_ptr<SliceMeshStorage>& mesh_ptr : storage.meshes)
    {
        auto& mesh = *mesh_ptr;
        if (! mesh.settings.get<bool>("infill_mesh") && ! mesh.settings.get<bool>("anti_overhang_mesh"))
        {
            slice_layer_count = std::max<unsigned int>(slice_layer_count, mesh.layers.size());
        }
    }

    // handle meshes
    std::vector<double> mesh_timings;
    for (unsigned int mesh_idx = 0; mesh_idx < storage.meshes.size(); mesh_idx++)
    {
        mesh_timings.push_back(1.0); // TODO: have a more accurate estimate of the relative time it takes per mesh, based on the height and number of polygons
    }
    ProgressStageEstimator inset_skin_progress_estimate(mesh_timings);

    Progress::messageProgressStage(Progress::Stage::INSET_SKIN, &time_keeper);
    std::vector<size_t> mesh_order;
    { // compute mesh order
        std::multimap<int, size_t> order_to_mesh_indices;
        for (size_t mesh_idx = 0; mesh_idx < storage.meshes.size(); mesh_idx++)
        {
            order_to_mesh_indices.emplace(storage.meshes[mesh_idx]->settings.get<int>("infill_mesh_order"), mesh_idx);
        }
        for (std::pair<const int, size_t>& order_and_mesh_idx : order_to_mesh_indices)
        {
            mesh_order.push_back(order_and_mesh_idx.second);
        }
    }
    for (size_t mesh_order_idx = 0; mesh_order_idx < mesh_order.size(); ++mesh_order_idx)
    {
        processBasicWallsSkinInfill(storage, mesh_order_idx, mesh_order, inset_skin_progress_estimate);
        Progress::messageProgress(Progress::Stage::INSET_SKIN, mesh_order_idx + 1, storage.meshes.size());
    }

    const Settings& mesh_group_settings = Application::getInstance().current_slice_->scene.current_mesh_group->settings;

    // we need to remove empty layers after we have processed the insets
    // processInsets might throw away parts if they have no wall at all (cause it doesn't fit)
    // brim depends on the first layer not being empty
    // only remove empty layers if we haven't generate support, because then support was added underneath the model.
    //   for some materials it's better to print on support than on the build plate.
    const auto has_support = mesh_group_settings.get<bool>("support_enable") || mesh_group_settings.get<bool>("support_mesh");
    const auto remove_empty_first_layers = mesh_group_settings.get<bool>("remove_empty_first_layers") && ! has_support;
    if (remove_empty_first_layers)
    {
        removeEmptyFirstLayers(storage, storage.print_layer_count); // changes storage.print_layer_count!
    }
    if (storage.print_layer_count == 0)
    {
        spdlog::warn("Stopping process because there are no non-empty layers.");
        return;
    }

    Progress::messageProgressStage(Progress::Stage::SUPPORT, &time_keeper);

    AreaSupport::generateOverhangAreas(storage);
    AreaSupport::generateSupportAreas(storage);
    TreeSupport tree_support_generator(storage);
    tree_support_generator.generateSupportAreas(storage);

    computePrintHeightStatistics(storage);

    // handle helpers
    storage.initializePrimeTower();

    spdlog::debug("Processing ooze shield");
    processOozeShield(storage);

    spdlog::debug("Processing draft shield");
    processDraftShield(storage);

    // This catches a special case in which the models are in the air, and then
    // the adhesion mustn't be calculated.
    if (! isEmptyLayer(storage, 0) || storage.prime_tower_)
    {
        spdlog::debug("Processing platform adhesion");
        processPlatformAdhesion(storage);
    }

    spdlog::debug("Meshes post-processing");
    // meshes post processing
    for (std::shared_ptr<SliceMeshStorage>& mesh : storage.meshes)
    {
        processDerivedWallsSkinInfill(*mesh);
    }

    spdlog::debug("Processing gradual support");
    // generate gradual support
    AreaSupport::generateSupportInfillFeatures(storage);
}

void FffPolygonGenerator::processBasicWallsSkinInfill(
    SliceDataStorage& storage,
    const size_t mesh_order_idx,
    const std::vector<size_t>& mesh_order,
    ProgressStageEstimator& inset_skin_progress_estimate)
{
    size_t mesh_idx = mesh_order[mesh_order_idx];
    SliceMeshStorage& mesh = *storage.meshes[mesh_idx];
    size_t mesh_layer_count = mesh.layers.size();
    if (mesh.settings.get<bool>("infill_mesh"))
    {
        processInfillMesh(storage, mesh_order_idx, mesh_order);
    }

    // === 新增功能：基于周长和面积的截面筛选 ===
    // 在处理walls、skin、infill之前，先筛选掉不符合条件的截面
    filterSmallLayerParts(mesh);

    // TODO: make progress more accurate!!
    // note: estimated time for     insets : skins = 22.953 : 48.858
    std::vector<double> walls_vs_skin_timing({ 22.953, 48.858 });
    ProgressStageEstimator* mesh_inset_skin_progress_estimator = new ProgressStageEstimator(walls_vs_skin_timing);

    inset_skin_progress_estimate.nextStage(mesh_inset_skin_progress_estimator); // the stage of this function call

    ProgressEstimatorLinear* inset_estimator = new ProgressEstimatorLinear(mesh_layer_count);
    mesh_inset_skin_progress_estimator->nextStage(inset_estimator);

    struct
    {
        ProgressStageEstimator& progress_estimator;
        std::mutex mutex{};
        std::atomic<size_t> processed_layer_count = 0;

        void operator++(int)
        {
            std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
            if (lock)
            { // progress estimation is done only in one thread so that no two threads message progress at the same time
                size_t processed_layer_count_ = processed_layer_count.fetch_add(1, std::memory_order_relaxed);
                double progress = progress_estimator.progress(processed_layer_count_);
                Progress::messageProgress(Progress::Stage::INSET_SKIN, progress * 100, 100);
            }
            else
            {
                processed_layer_count.fetch_add(1, std::memory_order_release);
            }
        }
        void reset()
        {
            processed_layer_count.store(0, std::memory_order_relaxed);
        }
    } guarded_progress = { inset_skin_progress_estimate };

    // walls
    cura::parallel_for<size_t>(
        0,
        mesh_layer_count,
        [&](size_t layer_number)
        {
            spdlog::debug("Processing insets for layer {} of {}", layer_number, mesh.layers.size());
            processWalls(mesh, layer_number);
            guarded_progress++;
        });

    ProgressEstimatorLinear* skin_estimator = new ProgressEstimatorLinear(mesh_layer_count);
    mesh_inset_skin_progress_estimator->nextStage(skin_estimator);

    bool process_infill = mesh.settings.get<coord_t>("infill_line_distance") > 0;
    if (! process_infill)
    { // do process infill anyway if it's modified by modifier meshes
        const Scene& scene = Application::getInstance().current_slice_->scene;
        for (size_t other_mesh_order_idx = mesh_order_idx + 1; other_mesh_order_idx < mesh_order.size(); ++other_mesh_order_idx)
        {
            const size_t other_mesh_idx = mesh_order[other_mesh_order_idx];
            SliceMeshStorage& other_mesh = *storage.meshes[other_mesh_idx];
            if (other_mesh.settings.get<bool>("infill_mesh"))
            {
                AABB3D aabb = scene.current_mesh_group->meshes[mesh_idx].getAABB();
                AABB3D other_aabb = scene.current_mesh_group->meshes[other_mesh_idx].getAABB();
                if (aabb.hit(other_aabb))
                {
                    process_infill = true;
                }
            }
        }
    }

    // skin & infill
    const Settings& mesh_group_settings = Application::getInstance().current_slice_->scene.current_mesh_group->settings;
    bool magic_spiralize = mesh_group_settings.get<bool>("magic_spiralize");
    size_t mesh_max_initial_bottom_layer_count = 0;

    // 检查是否使用magic_spiralize_range
    bool use_spiralize_range = false;
    if (magic_spiralize)
    {
        mesh_max_initial_bottom_layer_count = std::max(mesh_max_initial_bottom_layer_count, mesh.settings.get<size_t>("initial_bottom_layers"));

        // 检查是否定义了magic_spiralize_range
        HeightRangeList magic_spiralize_range = mesh.settings.get<HeightRangeList>("magic_spiralize_range");
        use_spiralize_range = !magic_spiralize_range.isEmpty();

        if (use_spiralize_range)
        {
            spdlog::info("【Skin/Infill处理】检测到magic_spiralize_range，为所有层生成完整数据");
        }
    }

    guarded_progress.reset();
    cura::parallel_for<size_t>(
        0,
        mesh_layer_count,
        [&](size_t layer_number)
        {
            spdlog::debug("Processing skins and infill layer {} of {}", layer_number, mesh.layers.size());

            // 修改逻辑：当使用magic_spiralize_range时，为所有层生成skin和infill
            // 这样在后续处理时可以根据每层的实际需要选择使用螺旋或正常模式
            bool should_process_skin_infill = true;
            if (magic_spiralize && !use_spiralize_range)
            {
                // 传统magic_spiralize逻辑：只为前几层生成
                should_process_skin_infill = (layer_number < mesh_max_initial_bottom_layer_count);
            }
            // 当use_spiralize_range=true时，should_process_skin_infill保持true，为所有层生成数据

            if (should_process_skin_infill)
            {
                processSkinsAndInfill(mesh, layer_number, process_infill);
            }
            guarded_progress++;
        });
}

void FffPolygonGenerator::processInfillMesh(SliceDataStorage& storage, const size_t mesh_order_idx, const std::vector<size_t>& mesh_order)
{
    size_t mesh_idx = mesh_order[mesh_order_idx];
    SliceMeshStorage& mesh = *storage.meshes[mesh_idx];
    coord_t surface_line_width = mesh.settings.get<coord_t>("wall_line_width_0");

    mesh.layer_nr_max_filled_layer = -1;
    for (LayerIndex layer_idx = 0; layer_idx < LayerIndex(mesh.layers.size()); layer_idx++)
    {
        SliceLayer& layer = mesh.layers[layer_idx];

        if (mesh.settings.get<ESurfaceMode>("magic_mesh_surface_mode") == ESurfaceMode::SURFACE)
        {
            // break up polygons into polylines
            // they have to be polylines, because they might break up further when doing the cutting
            for (SliceLayerPart& part : layer.parts)
            {
                for (const Polygon& poly : part.outline)
                {
                    layer.open_polylines.push_back(poly.toPseudoOpenPolyline());
                }
            }
            layer.parts.clear();
        }

        std::vector<SingleShape> new_parts;
        OpenLinesSet new_polylines;

        for (const size_t other_mesh_idx : mesh_order)
        { // limit the infill mesh's outline to within the infill of all meshes with lower order
            if (other_mesh_idx == mesh_idx)
            {
                break; // all previous meshes have been processed
            }
            SliceMeshStorage& other_mesh = *storage.meshes[other_mesh_idx];
            if (layer_idx >= LayerIndex(other_mesh.layers.size()))
            { // there can be no interaction between the infill mesh and this other non-infill mesh
                continue;
            }

            SliceLayer& other_layer = other_mesh.layers[layer_idx];

            for (SliceLayerPart& other_part : other_layer.parts)
            {
                if (mesh.settings.get<ESurfaceMode>("magic_mesh_surface_mode") != ESurfaceMode::SURFACE)
                {
                    for (SliceLayerPart& part : layer.parts)
                    { // limit the outline of each part of this infill mesh to the infill of parts of the other mesh with lower infill mesh order
                        if (! part.boundaryBox.hit(other_part.boundaryBox))
                        { // early out
                            continue;
                        }
                        Shape new_outline = part.outline.intersection(other_part.getOwnInfillArea());
                        if (new_outline.size() == 1)
                        { // we don't have to call splitIntoParts, because a single polygon can only be a single part
                            SingleShape outline_part_here;
                            outline_part_here.push_back(new_outline[0]);
                            new_parts.push_back(outline_part_here);
                        }
                        else if (new_outline.size() > 1)
                        { // we don't know whether it's a multitude of parts because of newly introduced holes, or because the polygon has been split up
                            std::vector<SingleShape> new_parts_here = new_outline.splitIntoParts();
                            for (SingleShape& new_part_here : new_parts_here)
                            {
                                new_parts.push_back(new_part_here);
                            }
                        }
                        // change the infill area of the non-infill mesh which is to be filled with e.g. lines
                        other_part.infill_area_own = other_part.getOwnInfillArea().difference(part.outline);
                        // note: don't change the part.infill_area, because we change the structure of that area, while the basic area in which infill is printed remains the same
                        //       the infill area remains the same for combing
                    }
                }
                if (mesh.settings.get<ESurfaceMode>("magic_mesh_surface_mode") != ESurfaceMode::NORMAL)
                {
                    const Shape& own_infill_area = other_part.getOwnInfillArea();
                    OpenLinesSet cut_lines = own_infill_area.intersection(layer.open_polylines);
                    new_polylines.push_back(cut_lines);
                    // NOTE: closed polygons will be represented as polylines, which will be closed automatically in the PathOrderOptimizer
                    if (! own_infill_area.empty())
                    {
                        other_part.infill_area_own = own_infill_area.difference(layer.open_polylines.offset(surface_line_width / 2));
                    }
                }
            }
        }

        layer.parts.clear();
        for (SingleShape& part : new_parts)
        {
            if (part.empty())
            {
                continue;
            }
            layer.parts.emplace_back();
            layer.parts.back().outline = part;
            layer.parts.back().boundaryBox.calculate(part);
        }

        if (mesh.settings.get<ESurfaceMode>("magic_mesh_surface_mode") != ESurfaceMode::NORMAL)
        {
            layer.open_polylines = new_polylines;
        }

        if (layer.parts.size() > 0 || (mesh.settings.get<ESurfaceMode>("magic_mesh_surface_mode") != ESurfaceMode::NORMAL && layer.open_polylines.size() > 0))
        {
            mesh.layer_nr_max_filled_layer = layer_idx; // last set by the highest non-empty layer
        }
    }
}

void FffPolygonGenerator::processDerivedWallsSkinInfill(SliceMeshStorage& mesh)
{
    if (mesh.settings.get<bool>("infill_support_enabled"))
    { // create gradual infill areas
        SkinInfillAreaComputation::generateInfillSupport(mesh);
    }

    // create gradual infill areas
    SkinInfillAreaComputation::generateGradualInfill(mesh);

    // SubDivCube Pre-compute Octree
    if (mesh.settings.get<coord_t>("infill_line_distance") > 0 && mesh.settings.get<EFillMethod>("infill_pattern") == EFillMethod::CUBICSUBDIV)
    {
        const Point3LL mesh_middle = mesh.bounding_box.getMiddle();
        const Point2LL infill_origin(mesh_middle.x_ + mesh.settings.get<coord_t>("infill_offset_x"), mesh_middle.y_ + mesh.settings.get<coord_t>("infill_offset_y"));
        SubDivCube::precomputeOctree(mesh, infill_origin);
    }

    // Pre-compute Cross Fractal
    if (mesh.settings.get<coord_t>("infill_line_distance") > 0
        && (mesh.settings.get<EFillMethod>("infill_pattern") == EFillMethod::CROSS || mesh.settings.get<EFillMethod>("infill_pattern") == EFillMethod::CROSS_3D))
    {
        const std::string cross_subdivision_spec_image_file = mesh.settings.get<std::string>("cross_infill_density_image");
        std::ifstream cross_fs(cross_subdivision_spec_image_file.c_str());
        if (! cross_subdivision_spec_image_file.empty() && cross_fs.good())
        {
            mesh.cross_fill_provider = std::make_shared<SierpinskiFillProvider>(
                mesh.bounding_box,
                mesh.settings.get<coord_t>("infill_line_distance"),
                mesh.settings.get<coord_t>("infill_line_width"),
                cross_subdivision_spec_image_file);
        }
        else
        {
            if (! cross_subdivision_spec_image_file.empty() && cross_subdivision_spec_image_file != " ")
            {
                spdlog::error("Cannot find density image: {}.", cross_subdivision_spec_image_file);
            }
            mesh.cross_fill_provider
                = std::make_shared<SierpinskiFillProvider>(mesh.bounding_box, mesh.settings.get<coord_t>("infill_line_distance"), mesh.settings.get<coord_t>("infill_line_width"));
        }
    }

    // Pre-compute lightning fill (aka minfill, aka ribbed support vaults)
    if (mesh.settings.get<coord_t>("infill_line_distance") > 0 && mesh.settings.get<EFillMethod>("infill_pattern") == EFillMethod::LIGHTNING)
    {
        // TODO: Make all of these into new type pointers (but the cross fill things need to happen too then, otherwise it'd just look weird).
        mesh.lightning_generator = std::make_shared<LightningGenerator>(mesh);
    }

    // combine infill
    SkinInfillAreaComputation::combineInfillLayers(mesh);

    // Fuzzy skin. Disabled when using interlocking structures, the internal interlocking walls become fuzzy.
    if (mesh.settings.get<bool>("magic_fuzzy_skin_enabled") && ! mesh.settings.get<bool>("interlocking_enable"))
    {
        processFuzzyWalls(mesh);
    }
}

/*
 * This function is executed in a parallel region based on layer_nr.
 * When modifying make sure any changes does not introduce data races.
 *
 * processInsets only reads and writes data for the current layer
 */
void FffPolygonGenerator::processWalls(SliceMeshStorage& mesh, size_t layer_nr)
{
    SliceLayer* layer = &mesh.layers[layer_nr];
    WallsComputation walls_computation(mesh.settings, layer_nr);
    walls_computation.generateWalls(layer, SectionType::WALL);
}

bool FffPolygonGenerator::isEmptyLayer(SliceDataStorage& storage, const LayerIndex& layer_idx)
{
    if (storage.support.generated && layer_idx < storage.support.supportLayers.size())
    {
        SupportLayer& support_layer = storage.support.supportLayers[layer_idx];
        if (! support_layer.support_infill_parts.empty() || ! support_layer.support_bottom.empty() || ! support_layer.support_roof.empty())
        {
            return false;
        }
    }
    for (std::shared_ptr<SliceMeshStorage>& mesh_ptr : storage.meshes)
    {
        auto& mesh = *mesh_ptr;
        if (layer_idx >= mesh.layers.size())
        {
            continue;
        }
        SliceLayer& layer = mesh.layers[layer_idx];
        if (mesh.settings.get<ESurfaceMode>("magic_mesh_surface_mode") != ESurfaceMode::NORMAL && layer.open_polylines.size() > 0)
        {
            return false;
        }
        for (const SliceLayerPart& part : layer.parts)
        {
            if (part.print_outline.size() > 0)
            {
                return false;
            }
        }
    }
    return true;
}

void FffPolygonGenerator::removeEmptyFirstLayers(SliceDataStorage& storage, size_t& total_layers)
{
    size_t n_empty_first_layers = 0;
    coord_t hightest_empty_layer = 0;
    for (size_t layer_idx = 0; layer_idx < total_layers; layer_idx++)
    {
        if (isEmptyLayer(storage, layer_idx))
        {
            n_empty_first_layers++;

            coord_t layer_highest_z = 0;
            for (const std::shared_ptr<SliceMeshStorage>& mesh_ptr : storage.meshes)
            {
                const auto& mesh = *mesh_ptr;
                layer_highest_z = layer_idx >= mesh.layers.size() ? layer_highest_z : std::max(layer_highest_z, mesh.layers[layer_idx].printZ);
            }
            hightest_empty_layer = std::max(hightest_empty_layer, layer_highest_z);
        }
        else
        {
            break;
        }
    }

    if (n_empty_first_layers > 0)
    {
        spdlog::info("Removing {} layers because they are empty", n_empty_first_layers);
        const coord_t layer_height = Application::getInstance().current_slice_->scene.current_mesh_group->settings.get<coord_t>("layer_height");
        for (auto& mesh_ptr : storage.meshes)
        {
            auto& mesh = *mesh_ptr;
            std::vector<SliceLayer>& layers = mesh.layers;
            if (layers.size() > n_empty_first_layers)
            {
                // transfer initial layer thickness to new initial layer
                layers[n_empty_first_layers].thickness = layers[0].thickness;
            }
            layers.erase(layers.begin(), layers.begin() + n_empty_first_layers);
            for (SliceLayer& layer : layers)
            {
                layer.printZ -= hightest_empty_layer;
            }
            mesh.layer_nr_max_filled_layer -= n_empty_first_layers;
        }
        total_layers -= n_empty_first_layers;
        storage.support.layer_nr_max_filled_layer -= n_empty_first_layers;
        std::vector<SupportLayer>& support_layers = storage.support.supportLayers;
        support_layers.erase(support_layers.begin(), support_layers.begin() + n_empty_first_layers);
    }
}

/*
 * This function is executed in a parallel region based on layer_nr.
 * When modifying make sure any changes does not introduce data races.
 *
 * generateSkins read (depend on) data from mesh.layers[*].parts[*].insets and write mesh.layers[n].parts[*].skin_parts
 * generateInfill read mesh.layers[n].parts[*].{insets,skin_parts,boundingBox} and write mesh.layers[n].parts[*].infill_area
 *
 * processSkinsAndInfill read (depend on) mesh.layers[*].parts[*].{insets,boundingBox}.
 *                       write mesh.layers[n].parts[*].{skin_parts,infill_area}.
 */
void FffPolygonGenerator::processSkinsAndInfill(SliceMeshStorage& mesh, const LayerIndex layer_nr, bool process_infill)
{
    if (mesh.settings.get<ESurfaceMode>("magic_mesh_surface_mode") == ESurfaceMode::SURFACE)
    {
        return;
    }

    SkinInfillAreaComputation skin_infill_area_computation(layer_nr, mesh, process_infill);
    skin_infill_area_computation.generateSkinsAndInfill();

    if (((mesh.settings.get<bool>("ironing_enabled") && (! mesh.settings.get<bool>("ironing_only_highest_layer"))) || mesh.layer_nr_max_filled_layer == layer_nr)
        || ! mesh.settings.get<bool>("small_skin_on_surface"))
    {
        // Generate the top surface to iron over.
        mesh.layers[layer_nr].top_surface.setAreasFromMeshAndLayerNumber(mesh, layer_nr);
    }

    if (layer_nr >= 0 && ! mesh.settings.get<bool>("small_skin_on_surface"))
    {
        // Generate the bottom surface.
        mesh.layers[layer_nr].bottom_surface = mesh.layers[layer_nr].getOutlines();
        if (layer_nr > 0)
        {
            mesh.layers[layer_nr].bottom_surface = mesh.layers[layer_nr].bottom_surface.difference(mesh.layers[layer_nr - 1].getOutlines());
        }
    }
}

void FffPolygonGenerator::computePrintHeightStatistics(SliceDataStorage& storage)
{
    const size_t extruder_count = Application::getInstance().current_slice_->scene.extruders.size();

    std::vector<int>& max_print_height_per_extruder = storage.max_print_height_per_extruder;
    assert(max_print_height_per_extruder.size() == 0 && "storage.max_print_height_per_extruder shouldn't have been initialized yet!");
    const int raft_layers = Raft::getTotalExtraLayers();
    max_print_height_per_extruder.resize(extruder_count, -(raft_layers + 1)); // Initialize all as -1 (or lower in case of raft).
    { // compute max_object_height_per_extruder
        // Height of the meshes themselves.
        for (std::shared_ptr<SliceMeshStorage>& mesh_ptr : storage.meshes)
        {
            auto& mesh = *mesh_ptr;
            if (mesh.settings.get<bool>("anti_overhang_mesh") || mesh.settings.get<bool>("support_mesh"))
            {
                continue; // Special type of mesh that doesn't get printed.
            }
            for (size_t extruder_nr = 0; extruder_nr < extruder_count; extruder_nr++)
            {
                for (LayerIndex layer_nr = LayerIndex(mesh.layers.size()) - 1; layer_nr > max_print_height_per_extruder[extruder_nr]; layer_nr--)
                {
                    if (mesh.getExtruderIsUsed(extruder_nr, layer_nr))
                    {
                        assert(max_print_height_per_extruder[extruder_nr] <= layer_nr);
                        max_print_height_per_extruder[extruder_nr] = layer_nr;
                    }
                }
            }
        }

        // Height of where the support reaches.
        Scene& scene = Application::getInstance().current_slice_->scene;
        const Settings& mesh_group_settings = scene.current_mesh_group->settings;
        const size_t support_infill_extruder_nr
            = mesh_group_settings.get<ExtruderTrain&>("support_infill_extruder_nr").extruder_nr_; // TODO: Support extruder should be configurable per object.
        max_print_height_per_extruder[support_infill_extruder_nr] = std::max(max_print_height_per_extruder[support_infill_extruder_nr], storage.support.layer_nr_max_filled_layer);
        const size_t support_roof_extruder_nr
            = mesh_group_settings.get<ExtruderTrain&>("support_roof_extruder_nr").extruder_nr_; // TODO: Support roof extruder should be configurable per object.
        max_print_height_per_extruder[support_roof_extruder_nr] = std::max(max_print_height_per_extruder[support_roof_extruder_nr], storage.support.layer_nr_max_filled_layer);
        const size_t support_bottom_extruder_nr
            = mesh_group_settings.get<ExtruderTrain&>("support_bottom_extruder_nr").extruder_nr_; // TODO: Support bottom extruder should be configurable per object.
        max_print_height_per_extruder[support_bottom_extruder_nr] = std::max(max_print_height_per_extruder[support_bottom_extruder_nr], storage.support.layer_nr_max_filled_layer);

        // Height of where the platform adhesion reaches.
        const EPlatformAdhesion adhesion_type = mesh_group_settings.get<EPlatformAdhesion>("adhesion_type");
        switch (adhesion_type)
        {
        case EPlatformAdhesion::SKIRT:
        case EPlatformAdhesion::BRIM:
        {
            const std::vector<ExtruderTrain*> skirt_brim_extruder_trains = mesh_group_settings.get<std::vector<ExtruderTrain*>>("skirt_brim_extruder_nr");
            for (ExtruderTrain* train : skirt_brim_extruder_trains)
            {
                const size_t skirt_brim_extruder_nr = train->extruder_nr_;
                max_print_height_per_extruder[skirt_brim_extruder_nr] = std::max(0, max_print_height_per_extruder[skirt_brim_extruder_nr]); // Includes layer 0.
            }
            break;
        }
        case EPlatformAdhesion::RAFT:
        {
            const size_t base_extruder_nr = mesh_group_settings.get<ExtruderTrain&>("raft_base_extruder_nr").extruder_nr_;
            max_print_height_per_extruder[base_extruder_nr] = std::max(-raft_layers, max_print_height_per_extruder[base_extruder_nr]); // Includes the lowest raft layer.
            const size_t interface_extruder_nr = mesh_group_settings.get<ExtruderTrain&>("raft_interface_extruder_nr").extruder_nr_;
            max_print_height_per_extruder[interface_extruder_nr]
                = std::max(-raft_layers + 1, max_print_height_per_extruder[interface_extruder_nr]); // Includes the second-lowest raft layer.
            const size_t surface_extruder_nr = mesh_group_settings.get<ExtruderTrain&>("raft_surface_extruder_nr").extruder_nr_;
            max_print_height_per_extruder[surface_extruder_nr]
                = std::max(-1, max_print_height_per_extruder[surface_extruder_nr]); // Includes up to the first layer below the model (so -1).
            break;
        }
        default:
            break; // No adhesion, so no maximum necessary.
        }
    }

    storage.max_print_height_order = order(max_print_height_per_extruder);
    if (extruder_count >= 2)
    {
        int second_highest_extruder = storage.max_print_height_order[extruder_count - 2];
        storage.max_print_height_second_to_last_extruder = max_print_height_per_extruder[second_highest_extruder];
    }
    else
    {
        storage.max_print_height_second_to_last_extruder = -(raft_layers + 1);
    }
}


void FffPolygonGenerator::processOozeShield(SliceDataStorage& storage)
{
    const Settings& mesh_group_settings = Application::getInstance().current_slice_->scene.current_mesh_group->settings;
    if (! mesh_group_settings.get<bool>("ooze_shield_enabled"))
    {
        return;
    }

    const coord_t ooze_shield_dist = mesh_group_settings.get<coord_t>("ooze_shield_dist");

    for (int layer_nr = 0; layer_nr <= storage.max_print_height_second_to_last_extruder; layer_nr++)
    {
        constexpr bool around_support = true;
        constexpr bool around_prime_tower = false;
        storage.ooze_shield.push_back(storage.getLayerOutlines(layer_nr, around_support, around_prime_tower).offset(ooze_shield_dist, ClipperLib::jtRound).getOutsidePolygons());
    }

    const AngleDegrees angle = mesh_group_settings.get<AngleDegrees>("ooze_shield_angle");
    if (angle <= 89)
    {
        const coord_t allowed_angle_offset
            = tan(mesh_group_settings.get<AngleRadians>("ooze_shield_angle")) * mesh_group_settings.get<coord_t>("layer_height"); // Allow for a 60deg angle in the oozeShield.
        for (LayerIndex layer_nr = 1; layer_nr <= storage.max_print_height_second_to_last_extruder; layer_nr++)
        {
            storage.ooze_shield[layer_nr] = storage.ooze_shield[layer_nr].unionPolygons(storage.ooze_shield[layer_nr - 1].offset(-allowed_angle_offset));
        }
        for (LayerIndex layer_nr = storage.max_print_height_second_to_last_extruder; layer_nr > 0; layer_nr--)
        {
            storage.ooze_shield[layer_nr - 1] = storage.ooze_shield[layer_nr - 1].unionPolygons(storage.ooze_shield[layer_nr].offset(-allowed_angle_offset));
        }
    }

    const double largest_printed_area = 1.0; // TODO: make var a parameter, and perhaps even a setting?
    for (LayerIndex layer_nr = 0; layer_nr <= storage.max_print_height_second_to_last_extruder; layer_nr++)
    {
        storage.ooze_shield[layer_nr].removeSmallAreas(largest_printed_area);
    }
    if (storage.prime_tower_)
    {
        coord_t max_line_width = 0;
        { // compute max_line_width
            const std::vector<bool> extruder_is_used = storage.getExtrudersUsed();
            const auto& extruders = Application::getInstance().current_slice_->scene.extruders;
            for (int extruder_nr = 0; extruder_nr < int(extruders.size()); extruder_nr++)
            {
                if (! extruder_is_used[extruder_nr])
                    continue;
                max_line_width = std::max(max_line_width, extruders[extruder_nr].settings_.get<coord_t>("skirt_brim_line_width"));
            }
        }
        for (LayerIndex layer_nr = 0; layer_nr <= storage.max_print_height_second_to_last_extruder; layer_nr++)
        {
            storage.ooze_shield[layer_nr] = storage.ooze_shield[layer_nr].difference(storage.prime_tower_->getOccupiedOutline(layer_nr).offset(max_line_width / 2));
        }
    }
}

void FffPolygonGenerator::processDraftShield(SliceDataStorage& storage)
{
    const size_t draft_shield_layers = getDraftShieldLayerCount(storage.print_layer_count);
    if (draft_shield_layers <= 0)
    {
        return;
    }
    const Settings& mesh_group_settings = Application::getInstance().current_slice_->scene.current_mesh_group->settings;
    const coord_t layer_height = mesh_group_settings.get<coord_t>("layer_height");

    const LayerIndex layer_skip{ 500 / layer_height + 1 };

    Shape& draft_shield = storage.draft_protection_shield;
    for (LayerIndex layer_nr = 0; layer_nr < storage.print_layer_count && layer_nr < draft_shield_layers; layer_nr += layer_skip)
    {
        constexpr bool around_support = true;
        constexpr bool around_prime_tower = false;
        draft_shield = draft_shield.unionPolygons(storage.getLayerOutlines(layer_nr, around_support, around_prime_tower));
    }

    const coord_t draft_shield_dist = mesh_group_settings.get<coord_t>("draft_shield_dist");
    storage.draft_protection_shield = draft_shield.approxConvexHull(draft_shield_dist);

    // Extra offset has rounded joints, so simplify again.
    coord_t maximum_resolution = 0; // Draft shield is printed with every extruder, so resolve with the max() or min() of them to meet the requirements of all extruders.
    coord_t maximum_deviation = std::numeric_limits<coord_t>::max();
    for (const ExtruderTrain& extruder : Application::getInstance().current_slice_->scene.extruders)
    {
        maximum_resolution = std::max(maximum_resolution, extruder.settings_.get<coord_t>("meshfix_maximum_resolution"));
        maximum_deviation = std::min(maximum_deviation, extruder.settings_.get<coord_t>("meshfix_maximum_deviation"));
    }
    storage.draft_protection_shield = Simplify(maximum_resolution, maximum_deviation, 0).polygon(storage.draft_protection_shield);
    if (storage.prime_tower_)
    {
        coord_t max_line_width = 0;
        { // compute max_line_width
            const std::vector<bool> extruder_is_used = storage.getExtrudersUsed();
            const auto& extruders = Application::getInstance().current_slice_->scene.extruders;
            for (int extruder_nr = 0; extruder_nr < int(extruders.size()); extruder_nr++)
            {
                if (! extruder_is_used[extruder_nr])
                    continue;
                max_line_width = std::max(max_line_width, extruders[extruder_nr].settings_.get<coord_t>("skirt_brim_line_width"));
            }
        }
        storage.draft_protection_shield = storage.draft_protection_shield.difference(storage.prime_tower_->getOccupiedGroundOutline().offset(max_line_width / 2));
    }
}

void FffPolygonGenerator::processPlatformAdhesion(SliceDataStorage& storage)
{
    const Settings& mesh_group_settings = Application::getInstance().current_slice_->scene.current_mesh_group->settings;
    EPlatformAdhesion adhesion_type = mesh_group_settings.get<EPlatformAdhesion>("adhesion_type");

    if (adhesion_type == EPlatformAdhesion::RAFT)
    {
        Raft::generate(storage);
        return;
    }

    SkirtBrim skirt_brim(storage);
    if (adhesion_type != EPlatformAdhesion::NONE)
    {
        skirt_brim.generate();
    }

    if (mesh_group_settings.get<bool>("support_brim_enable"))
    {
        skirt_brim.generateSupportBrim();
    }
}


void FffPolygonGenerator::processFuzzyWalls(SliceMeshStorage& mesh)
{
    if (mesh.settings.get<size_t>("wall_line_count") == 0)
    {
        return;
    }

    const coord_t line_width = mesh.settings.get<coord_t>("line_width");
    const bool apply_outside_only = mesh.settings.get<bool>("magic_fuzzy_skin_outside_only");
    const coord_t fuzziness = mesh.settings.get<coord_t>("magic_fuzzy_skin_thickness");
    const coord_t avg_dist_between_points = mesh.settings.get<coord_t>("magic_fuzzy_skin_point_dist");
    const coord_t min_dist_between_points = avg_dist_between_points * 3 / 4; // hardcoded: the point distance may vary between 3/4 and 5/4 the supplied value
    const coord_t range_random_point_dist = avg_dist_between_points / 2;
    unsigned int start_layer_nr
        = (mesh.settings.get<EPlatformAdhesion>("adhesion_type") == EPlatformAdhesion::BRIM) ? 1 : 0; // don't make fuzzy skin on first layer if there's a brim

    auto hole_area = Shape();
    std::function<bool(const bool&, const ExtrusionJunction&)> accumulate_is_in_hole
        = []([[maybe_unused]] const bool& prev_result, [[maybe_unused]] const ExtrusionJunction& junction)
    {
        return false;
    };

    for (LayerIndex layer_nr = start_layer_nr; layer_nr < LayerIndex(mesh.layers.size()); layer_nr++)
    {
        SliceLayer& layer = mesh.layers[layer_nr];
        for (SliceLayerPart& part : layer.parts)
        {
            std::vector<VariableWidthLines> result_paths;
            for (auto& toolpath : part.wall_toolpaths)
            {
                if (toolpath.front().inset_idx_ != 0)
                {
                    result_paths.push_back(toolpath);
                    continue;
                }

                auto& result_lines = result_paths.emplace_back();

                if (apply_outside_only)
                {
                    hole_area = part.print_outline.getOutsidePolygons().offset(-line_width);
                    accumulate_is_in_hole = [&hole_area](const bool& prev_result, const ExtrusionJunction& junction)
                    {
                        return prev_result || hole_area.inside(junction.p_);
                    };
                }
                for (auto& line : toolpath)
                {
                    if (apply_outside_only && std::accumulate(line.begin(), line.end(), false, accumulate_is_in_hole))
                    {
                        result_lines.push_back(line);
                        continue;
                    }

                    auto& result = result_lines.emplace_back(line.inset_idx_, line.is_odd_, line.is_closed_);

                    // generate points in between p0 and p1
                    int64_t dist_left_over
                        = (min_dist_between_points / 4) + rand() % (min_dist_between_points / 4); // the distance to be traversed on the line before making the first new point
                    auto* p0 = &line.front();
                    for (auto& p1 : line)
                    {
                        if (p0->p_ == p1.p_) // avoid seams
                        {
                            result.emplace_back(p1.p_, p1.w_, p1.perimeter_index_);
                            continue;
                        }

                        // 'a' is the (next) new point between p0 and p1
                        const Point2LL p0p1 = p1.p_ - p0->p_;
                        const int64_t p0p1_size = vSize(p0p1);
                        int64_t p0pa_dist = dist_left_over;
                        if (p0pa_dist >= p0p1_size)
                        {
                            const Point2LL p = p1.p_ - (p0p1 / 2);
                            const double width = (p1.w_ * vSize(p1.p_ - p) + p0->w_ * vSize(p0->p_ - p)) / p0p1_size;
                            result.emplace_back(p, width, p1.perimeter_index_);
                        }
                        for (; p0pa_dist < p0p1_size; p0pa_dist += min_dist_between_points + rand() % range_random_point_dist)
                        {
                            const int r = rand() % (fuzziness * 2) - fuzziness;
                            const Point2LL perp_to_p0p1 = turn90CCW(p0p1);
                            const Point2LL fuzz = normal(perp_to_p0p1, r);
                            const Point2LL pa = p0->p_ + normal(p0p1, p0pa_dist);
                            const double width = (p1.w_ * vSize(p1.p_ - pa) + p0->w_ * vSize(p0->p_ - pa)) / p0p1_size;
                            result.emplace_back(pa + fuzz, width, p1.perimeter_index_);
                        }
                        // p0pa_dist > p0p1_size now because we broke out of the for-loop
                        dist_left_over = p0pa_dist - p0p1_size;

                        p0 = &p1;
                    }
                    while (result.size() < 3)
                    {
                        size_t point_idx = line.size() - 2;
                        result.emplace_back(line[point_idx].p_, line[point_idx].w_, line[point_idx].perimeter_index_);
                        if (point_idx == 0)
                        {
                            break;
                        }
                        point_idx--;
                    }
                    if (result.size() < 3)
                    {
                        result.clear();
                        for (auto& p : line)
                        {
                            result.emplace_back(p.p_, p.w_, p.perimeter_index_);
                        }
                    }
                    if (line.back().p_ == line.front().p_) // avoid seams
                    {
                        result.back().p_ = result.front().p_;
                    }
                }
            }
            part.wall_toolpaths = result_paths;
        }
    }
}

void FffPolygonGenerator::filterSmallLayerParts(SliceMeshStorage& mesh)
{
    // === 获取筛选参数 ===
    // minimum_polygon_circumference: 最小周长阈值（微米）
    // minimum_polygon_area: 最小面积阈值（平方微米）

    // 安全获取minimum_polygon_circumference参数
    coord_t min_circumference = 0;
    try {
        min_circumference = mesh.settings.get<coord_t>("minimum_polygon_circumference");
    } catch (...) {
        // 参数未设置，使用默认值0
        min_circumference = 0;
    }

    // 安全获取minimum_polygon_area参数，如果不存在则使用默认值0（不进行面积筛选）
    coord_t min_area_um2 = 0;  // 默认值：不进行面积筛选
    try {
        // 从设置中获取面积阈值（单位：平方毫米），转换为平方微米
        double min_area_mm2 = mesh.settings.get<double>("minimum_polygon_area");
        min_area_um2 = static_cast<coord_t>(min_area_mm2 * 1000000.0);  // mm² -> μm²
    } catch (...) {
        // 参数未设置，使用默认值0
        min_area_um2 = 0;
    }

    // 如果两个阈值都为0或负数，则不进行筛选
    if (min_circumference <= 0 && min_area_um2 <= 0)
    {
        spdlog::debug("跳过小图形筛选: 周长阈值={:.3f}mm, 面积阈值={:.3f}mm²",
                     INT2MM(min_circumference), min_area_um2 / 1000000.0);
        return;
    }

    spdlog::info("=== 小图形筛选功能开始 ===");
    spdlog::info("分析标准: 只分析最外层wall（inset0）的周长和面积");
    spdlog::info("删除策略: 删除整个截面的所有内容（inset、infill、skin等）");
    spdlog::info("最小周长阈值: {:.3f}mm", INT2MM(min_circumference));
    spdlog::info("最小面积阈值: {:.3f}mm²", min_area_um2 / 1000000.0);

    size_t total_removed_parts = 0;
    size_t total_original_parts = 0;

    // === 遍历所有层进行筛选 ===
    for (LayerIndex layer_idx = 0; layer_idx < LayerIndex(mesh.layers.size()); layer_idx++)
    {
        SliceLayer& layer = mesh.layers[layer_idx];
        size_t original_parts_count = layer.parts.size();
        total_original_parts += original_parts_count;

        if (original_parts_count == 0)
        {
            continue;  // 空层，跳过
        }

        // 如果该层有多个图形，输出详细分析信息
        if (original_parts_count > 1)
        {
            spdlog::info("=== 层{} 多图形分析开始 ===", layer_idx);
            spdlog::info("该层包含{}个图形，开始逐个分析最外层wall", original_parts_count);
        }

        // 记录每个part的分析结果，用于详细日志
        std::vector<bool> part_removal_decisions;
        std::vector<std::string> part_analysis_details;

        // 使用remove_if算法筛选掉不符合条件的parts
        auto removed_parts_begin = std::remove_if(
            layer.parts.begin(),
            layer.parts.end(),
            [&](const SliceLayerPart& part) -> bool
            {
                // 获取当前part在原始列表中的索引（用于日志）
                size_t part_index = &part - &layer.parts[0];

                // === 分析最外层wall（inset0）的周长和面积 ===
                // 注意：这里分析的是part.outline，它代表最外层轮廓
                coord_t total_circumference = 0;
                coord_t total_area = 0;

                spdlog::debug("  图形[{}]: 开始分析最外层wall", part_index);
                spdlog::debug("  图形[{}]: outline包含{}个多边形", part_index, part.outline.size());

                // 遍历part的所有outline多边形（最外层wall）
                for (size_t poly_idx = 0; poly_idx < part.outline.size(); ++poly_idx)
                {
                    const Polygon& polygon = part.outline[poly_idx];

                    // 计算周长
                    coord_t polygon_circumference = polygon.length();
                    total_circumference += polygon_circumference;

                    // 计算面积（使用绝对值处理正负面积）
                    coord_t polygon_area = std::abs(static_cast<coord_t>(polygon.area()));
                    total_area += polygon_area;

                    spdlog::debug("    多边形[{}]: 周长={:.3f}mm, 面积={:.3f}mm², 顶点数={}",
                                poly_idx, INT2MM(polygon_circumference), INT2MM2(polygon_area), polygon.size());
                }

                spdlog::info("  图形[{}]: 总周长={:.3f}mm, 总面积={:.3f}mm²",
                            part_index, INT2MM(total_circumference), INT2MM2(total_area));

                // === 判断是否需要删除 ===
                bool should_remove = false;
                std::string removal_reason = "";

                // 检查周长条件
                if (min_circumference > 0 && total_circumference < min_circumference)
                {
                    should_remove = true;
                    removal_reason += "周长不足(" + std::to_string(INT2MM(total_circumference)) + "mm < " +
                                     std::to_string(INT2MM(min_circumference)) + "mm)";
                }

                // 检查面积条件
                if (min_area_um2 > 0 && total_area < min_area_um2)
                {
                    if (should_remove) {
                        removal_reason += " 且 ";
                    }
                    should_remove = true;
                    removal_reason += "面积不足(" + std::to_string(INT2MM2(total_area)) + "mm² < " +
                                     std::to_string(min_area_um2 / 1000000.0) + "mm²)";
                }

                // 输出决策结果
                if (should_remove)
                {
                    spdlog::info("  图形[{}]: ❌ 删除决策 - {}", part_index, removal_reason);
                    spdlog::info("  图形[{}]: 将删除整个截面（包括inset、infill、skin等所有内容）", part_index);
                }
                else
                {
                    spdlog::info("  图形[{}]: ✅ 保留决策 - 满足所有阈值要求", part_index);
                }

                return should_remove;
            }
        );

        // 实际删除不符合条件的parts
        size_t removed_count = std::distance(removed_parts_begin, layer.parts.end());
        layer.parts.erase(removed_parts_begin, layer.parts.end());

        total_removed_parts += removed_count;

        // 输出层级筛选结果
        if (original_parts_count > 1 || removed_count > 0)
        {
            spdlog::info("=== 层{} 筛选结果 ===", layer_idx);
            spdlog::info("原始图形数: {}, 删除图形数: {}, 保留图形数: {}",
                        original_parts_count, removed_count, layer.parts.size());

            if (removed_count > 0)
            {
                double removal_rate = (double)removed_count / original_parts_count * 100.0;
                spdlog::info("该层删除率: {:.1f}%", removal_rate);
            }
        }
    }

    // === 输出总体筛选结果 ===
    spdlog::info("=== 小图形筛选功能完成 ===");
    spdlog::info("处理层数: {}", mesh.layers.size());
    spdlog::info("原始图形总数: {}", total_original_parts);
    spdlog::info("删除图形总数: {}", total_removed_parts);
    spdlog::info("保留图形总数: {}", total_original_parts - total_removed_parts);

    if (total_removed_parts > 0)
    {
        double removal_percentage = (double)total_removed_parts / total_original_parts * 100.0;
        spdlog::info("总体删除率: {:.1f}%", removal_percentage);
        spdlog::info("删除策略: 基于最外层wall分析，删除整个截面内容");
    }
    else
    {
        spdlog::info("结果: 没有图形被删除，所有图形都满足阈值要求");
    }
}


} // namespace cura
