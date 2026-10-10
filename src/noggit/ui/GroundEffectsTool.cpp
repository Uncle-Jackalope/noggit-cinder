#include <noggit/ActionManager.hpp>
#include <noggit/application/Configuration/NoggitApplicationConfiguration.hpp>
#include <noggit/application/NoggitApplication.hpp>
#include <noggit/DetailDoodads.hpp>
#include <noggit/project/CurrentProject.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <QDialog>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMenu>
#include <QMessageBox>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QGridLayout>
#include <QHBoxLayout>
#include <noggit/DBC.h>
#include <noggit/MapChunk.h>
#include <noggit/MapTile.h>
#include <noggit/MapView.h>
#include <noggit/texture_set.hpp>
#include <noggit/ui/CurrentTexture.h>
#include <noggit/ui/FontAwesome.hpp>
#include <noggit/ui/GroundEffectsTool.hpp>
#include <noggit/ui/texturing_tool.hpp>
#include <noggit/ui/tools/AssetBrowser/Ui/AssetBrowser.hpp>
#include <noggit/ui/tools/PreviewRenderer/PreviewRenderer.hpp>
#include <noggit/ui/tools/UiCommon/ExtendedSlider.hpp>
#include <noggit/World.h>
#include <noggit/Log.h>

#include <exception>
#include <string>

#include <QDockWidget>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QSpinBox>
#include <QVBoxLayout>

namespace
{
    std::string normalizedTextureName(std::string name)
    {
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c)
            { return static_cast<char>(std::tolower(c)); });
        std::replace(name.begin(), name.end(), '\\', '/');
        return name;
    }
    constexpr char const* STRING_EMPTY_TEXTURE = "tileset\\generic\\black.blp";

    constexpr char const* STRING_DISK_SWEEP_WARNING = "Full-map disk assignment is not available in this port.";
    QString normalizedDoodadFilename(QString name)
    {
        name = name.toLower();
        name.replace(".mdx", ".m2");
        name.replace(".mdl", ".m2");
        return name;
    }

    // Returns the id of the GroundEffectDoodad record for this filename,
    // creating the record if no existing one matches.
    unsigned int findOrCreateGroundEffectDoodad(std::string const& filename)
    {
        QString const wanted = normalizedDoodadFilename(QString(filename.c_str()));

        for (auto it = gGroundEffectDoodadDB.begin(); it != gGroundEffectDoodadDB.end(); ++it)
        {
            if (normalizedDoodadFilename(QString(it->getString(GroundEffectDoodadDB::Filename))) == wanted)
            {
                return it->getUInt(GroundEffectDoodadDB::ID);
            }
        }

        int const new_id = gGroundEffectDoodadDB.getEmptyRecordID();
        auto record = gGroundEffectDoodadDB.addRecord(new_id);
        record.writeString(GroundEffectDoodadDB::Filename, filename);
        return static_cast<unsigned int>(new_id);
    }
    // AreaTable's parent field is shared by vanilla and Azure; keep this adapter local.
    unsigned int groundEffectZoneId(unsigned int area)
    {
        std::unordered_set<unsigned int> visited;
        while (area && visited.insert(area).second && gAreaDB.CheckIfIdExists(area))
        {
            unsigned int const parent = gAreaDB.getByID(area).getUInt(AreaDB::Region);
            if (!parent) break;
            area = parent;
        }
        return area;
    }

}

namespace Noggit
{
    namespace Ui
    {
        GroundEffectsTool::GroundEffectsTool(texturing_tool* texturing_tool, MapView* map_view, QWidget* parent)
            : QWidget(parent, Qt::Window), _texturing_tool(texturing_tool), _map_view(map_view)
        {
            setWindowTitle("Ground Effects Tool");
            setMinimumSize(750, 600);
            setWindowFlags(Qt::Tool | Qt::WindowStaysOnTopHint);
            setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
            QHBoxLayout* main_layout = new QHBoxLayout(this);
            QVBoxLayout* left_side_layout = new QVBoxLayout(this);
            QVBoxLayout* right_side_layout = new QVBoxLayout(this);
            main_layout->addLayout(left_side_layout);
            main_layout->addLayout(right_side_layout);

            // Render modes.
            {
                _render_group_box = new QGroupBox("Render Mode", this);
                _render_group_box->setCheckable(true);
                _render_group_box->setChecked(true);
                left_side_layout->addWidget(_render_group_box);

                auto render_layout(new QGridLayout(_render_group_box));
                _render_group_box->setLayout(render_layout);

                _render_type_group = new QButtonGroup(_render_group_box);

                _render_active_sets = new QRadioButton("Effect Id/Set", this);
                _render_active_sets->setToolTip("Render all the loaded effect sets for this texture in matching colors");
                _render_type_group->addButton(_render_active_sets);
                render_layout->addWidget(_render_active_sets, 0, 0);

                // _render_exclusion_map = new QRadioButton("Doodads Disabled", this);
                // _render_exclusion_map->setToolTip("Render chunk units where effect doodads are disabled as white, rest as black");
                // _render_type_group->addButton(_render_exclusion_map);
                // render_layout->addWidget(_render_exclusion_map, 0, 1);

                // If chunk contains Texture/Effect : Render as green or red if the effect layer is active or not.
                _render_placement_map = new QRadioButton("Selected Texture state", this);
                _render_placement_map->setToolTip("Render chunk unit as red if texture is present in the chunk and NOT the current \
                active layer, render as green if it's active. \nThis defines which of the 4 textures' set is currently active,\
                this is determined by which has the highest opacity.");
                _render_type_group->addButton(_render_placement_map);
                render_layout->addWidget(_render_placement_map, 0, 1);

                _render_active_sets->setChecked(true);
            }

            _chkbox_merge_duplicates = new QCheckBox("Ignore duplicates", this);
            _chkbox_merge_duplicates->setChecked(true);
            left_side_layout->addWidget(_chkbox_merge_duplicates);

            auto button_scan_adt = new QPushButton("Scan for sets in curr tile", this);
            left_side_layout->addWidget(button_scan_adt);

            auto button_scan_adt_loaded = new QPushButton("Scan for sets in loaded Tiles", this);
            left_side_layout->addWidget(button_scan_adt_loaded);

            // Selection.
            auto selection_group = new QGroupBox("Effect Set Selection", this);
            selection_group->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
            left_side_layout->addWidget(selection_group);
            auto selection_layout(new QVBoxLayout(selection_group));
            selection_group->setLayout(selection_layout);

            selection_layout->addWidget(new QLabel("Blizzard Ground Effects", this));
            _blizzard_assignments_combo = new QComboBox(this);
            _blizzard_assignments_combo->addItem("Choose a Blizzard ground effect...");
            _blizzard_assignments_combo->setEnabled(false);
            _blizzard_assignments_combo->setToolTip(
                "Choose an effect Blizzard used with the current terrain texture. "
                "This only changes the editor selection; it does not apply anything to terrain.");
            selection_layout->addWidget(_blizzard_assignments_combo);

            auto set_buttons_widget = new QWidget(this);
            auto set_buttons_layout = new QHBoxLayout(set_buttons_widget);
            set_buttons_layout->setContentsMargins(0, 0, 0, 0);
            selection_layout->addWidget(set_buttons_widget);

            auto button_create_new = new QPushButton("Create New", this);
            set_buttons_layout->addWidget(button_create_new);

            auto button_duplicate = new QPushButton("Duplicate", this);
            button_duplicate->setToolTip("Copies the selected set into a new unsaved set to edit from.");
            set_buttons_layout->addWidget(button_duplicate);

            auto button_delete = new QPushButton("Delete", this);
            button_delete->setToolTip("Removes the selected set's GroundEffectTexture record from the dbc.\
            \nChunks still referencing the id simply spawn nothing, like the client with a missing record.");
            set_buttons_layout->addWidget(button_delete);

            connect(button_create_new, &QPushButton::clicked, [this]() { createNewSet(); });
            connect(button_duplicate, &QPushButton::clicked, [this]() { duplicateSelectedSet(); });
            connect(button_delete, &QPushButton::clicked, [this]() { deleteSelectedSet(); });
            connect(_blizzard_assignments_combo, qOverload<int>(&QComboBox::currentIndexChanged),
                this, &GroundEffectsTool::selectBlizzardGroundEffect);

            _effect_sets_list = new QListWidget(this);
            selection_layout->addWidget(_effect_sets_list);
            _effect_sets_list->setViewMode(QListView::ListMode);
            _effect_sets_list->setSelectionMode(QAbstractItemView::SelectionMode::SingleSelection);
            _effect_sets_list->setSelectionBehavior(QAbstractItemView::SelectItems);
            _effect_sets_list->setUniformItemSizes(true);
            _effect_sets_list->setFixedHeight(160);
            _effect_sets_list->setIconSize(QSize(20, 20));

            // Effect settings.
            {
                auto settings_group = new QGroupBox("Selected Set Settings", this);
                settings_group->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
                right_side_layout->addWidget(settings_group);
                auto settings_layout(new QFormLayout(settings_group));
                settings_group->setLayout(settings_layout);

                auto set_help_label = new QLabel("A ground Effect Set contains up to 4 different doodads.\nTerrain Type is used for footprints and sounds.");
                settings_layout->addRow(set_help_label);

                _object_list = new QListWidget(this);
                _object_list->setItemAlignment(Qt::AlignCenter);
                _object_list->setViewMode(QListView::IconMode);
                _object_list->setWrapping(false);
                _object_list->setIconSize(QSize(100, 100));
                _object_list->setFlow(QListWidget::LeftToRight);
                _object_list->setSelectionMode(QAbstractItemView::SingleSelection);
                _object_list->setAcceptDrops(false);
                _object_list->setMovement(QListView::Movement::Static);
                _object_list->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
                _object_list->setFixedWidth(_object_list->iconSize().width() * 4 + 40); //  padding-right: 10px * 4
                _object_list->setFixedHeight(_object_list->iconSize().height() + 20);

                settings_layout->addRow(_object_list);
                for (int i = 0; i < 4; i++)
                {
                    QListWidgetItem* list_item = new QListWidgetItem(_object_list);
                    list_item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
                    list_item->setIcon(Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::plus));
                    list_item->setText(STRING_EMPTY_DISPLAY);
                    list_item->setToolTip("");
                    _object_list->addItem(list_item);
                }

                // One weight per doodad slot, aligned under the icons.
                auto weights_widget = new QWidget(this);
                auto weights_layout = new QHBoxLayout(weights_widget);
                weights_layout->setContentsMargins(0, 0, 0, 0);
                for (int i = 0; i < 4; i++)
                {
                    _weight_spinboxes[i] = new QSpinBox(this);
                    _weight_spinboxes[i]->setRange(0, 100);
                    _weight_spinboxes[i]->setValue(1);
                    _weight_spinboxes[i]->setPrefix("Weight : ");
                    _weight_spinboxes[i]->setToolTip("Relative chance of this doodad being picked for a placement.");
                    _weight_spinboxes[i]->setMinimumWidth(90);
                    weights_layout->addWidget(_weight_spinboxes[i]);

                    connect(_weight_spinboxes[i], qOverload<int>(&QSpinBox::valueChanged),
                        [this](int) { updateWeightShares(); updateLivePreview(); });
                }
                weights_layout->addStretch();
                settings_layout->addRow(weights_widget);

                // The share of the client's 16-slot spawn table each slot really
                // gets - weights are not straight percentages.
                auto shares_widget = new QWidget(this);
                auto shares_layout = new QHBoxLayout(shares_widget);
                shares_layout->setContentsMargins(0, 0, 0, 0);
                for (int i = 0; i < 4; i++)
                {
                    _weight_share_labels[i] = new QLabel("-", this);
                    _weight_share_labels[i]->setAlignment(Qt::AlignHCenter);
                    _weight_share_labels[i]->setToolTip("The slot's actual share of the client's 16-entry spawn table\
                    \nbuilt from the weights. \"empty\" table entries spawn nothing.");
                    _weight_share_labels[i]->setFixedWidth(_object_list->iconSize().width());
                    shares_layout->addWidget(_weight_share_labels[i]);
                }
                shares_layout->addStretch();
                settings_layout->addRow(shares_widget);

                _preview_renderer = new Tools::PreviewRenderer(_object_list->iconSize().width(),
                    _object_list->iconSize().height(),
                    Noggit::NoggitRenderContext::GROUND_EFFECT_PREVIEW, this);
                _preview_renderer->setVisible(false);
                // Initialize renderer.
                auto const preview_model = std::string("world/wmo/azeroth/buildings/human_farm/farm.wmo");

                try
                {
                  _preview_renderer->setModelOffscreen(preview_model);
                  _preview_renderer->renderToPixmap();
                }
                catch (std::exception const& e)
                {
                  LogError << "GroundEffectsTool preview render failed for " << preview_model
                    << ": " << e.what() << std::endl;
                }
                catch (...)
                {
                  LogError << "GroundEffectsTool preview render failed for " << preview_model
                    << " with unknown exception" << std::endl;
                }

                // Disable this if no active doodad. 
                // Density: 0 → 8. > 24 → 24. This value is for the amount of doodads and on higher values for coverage.
                // Till an amount of around 24 it just increases the amount.After this the doodads begin to group.
                // In WOTLK, only 4 entries out of 25k use more than 20.In retail only 5 use more than 25. 16 or less seems standard
                // TODO : If we end up limiting, a slider could be more apropriate.
                _spinbox_doodads_amount = new QSpinBox(this);
                _spinbox_doodads_amount->setRange(0, 24);
                _spinbox_doodads_amount->setValue(8);
                settings_layout->addRow("Doodads amount : ", _spinbox_doodads_amount);
                _cbbox_terrain_type = new QComboBox(this);
                settings_layout->addRow("Terrain Type", _cbbox_terrain_type);

                for (auto it = gTerrainTypeDB.begin(); it != gTerrainTypeDB.end(); ++it)
                {
                    auto terrain_type_record = *it;

                    _cbbox_terrain_type->addItem(QString(terrain_type_record.getString(TerrainTypeDB::TerrainDesc)),
                        QVariant(terrain_type_record.getUInt(TerrainTypeDB::TerrainId)));
                }

                auto button_save_settings = new QPushButton("Save Set", this);
                settings_layout->addRow(button_save_settings);
                button_save_settings->setBaseSize(button_save_settings->size() / 2.0);
                connect(button_save_settings, &QPushButton::clicked, this, &GroundEffectsTool::saveSelectedSet);
            }


            // Apply group.
            auto apply_group = new QGroupBox("Apply Ground Effect", this);
            right_side_layout->addWidget(apply_group);

            auto apply_layout(new QVBoxLayout(apply_group));
            apply_group->setLayout(apply_layout);

            // Generate modes.
            {
                auto buttons_layout(new QGridLayout(this));
                apply_layout->addLayout(buttons_layout);

                _generate_type_group = new QButtonGroup(apply_group);

                auto generate_effect_zone = new QRadioButton("Current Zone", this);
                _generate_type_group->addButton(generate_effect_zone, 0);
                buttons_layout->addWidget(generate_effect_zone, 0, 0);

                auto generate_effect_area = new QRadioButton("Current Area (Subzone)", this);
                _generate_type_group->addButton(generate_effect_area, 1);
                buttons_layout->addWidget(generate_effect_area, 0, 1);

                auto generate_effect_adt = new QRadioButton("Current ADT (Tile)", this);
                _generate_type_group->addButton(generate_effect_adt, 2);
                buttons_layout->addWidget(generate_effect_adt, 1, 0);

                auto generate_effect_global = new QRadioButton("Global (Entire Map)", this);
                _generate_type_group->addButton(generate_effect_global, 3);
                buttons_layout->addWidget(generate_effect_global, 1, 1);

                generate_effect_zone->setToolTip("Affects loaded tiles in the zone; undoable.");
                generate_effect_area->setToolTip("Affects loaded tiles in the area; undoable.");
                // TODO(azure-port): full-map disk sweep needs World::applyGroundEffectGlobal.
                generate_effect_global->setEnabled(false);
                generate_effect_global->setToolTip("Full-map disk assignment is not available yet.");
                auto generate_effect_chunk = new QRadioButton("Current Chunk", this);
                _generate_type_group->addButton(generate_effect_chunk, 4);
                buttons_layout->addWidget(generate_effect_chunk, 2, 0);
                generate_effect_chunk->setToolTip("The chunk under the camera; undoable.");
                generate_effect_adt->setChecked(true);
            }

            _apply_override_cb = new QCheckBox("Override", this);
            _apply_override_cb->setToolTip("If the texture already had a ground effect, replace it.");
            _apply_override_cb->setChecked(true);
            apply_layout->addWidget(_apply_override_cb);

            auto button_generate = new QPushButton("Apply to Texture", this);
            apply_layout->addWidget(button_generate);
            connect(button_generate, &QPushButton::clicked, this, &GroundEffectsTool::applySelectedSet);

            _scope_disk_sweep_cb = new QCheckBox("Include unloaded tiles (disk sweep)", this);
            // TODO(azure-port): area/zone disk sweep needs World::applyGroundEffectGlobal.
            _scope_disk_sweep_cb->setEnabled(false);
            _scope_disk_sweep_cb->setToolTip("Unloaded-tile assignment is not available yet.");
            apply_layout->addWidget(_scope_disk_sweep_cb);

            auto preview_group = new QGroupBox("Detail Doodad Preview", this);
            right_side_layout->addWidget(preview_group);
            auto preview_layout = new QVBoxLayout(preview_group);
            auto render_doodads = new QCheckBox("Render detail doodads", this);
            render_doodads->setEnabled(false);
            render_doodads->setToolTip("Renderer controls are not available in this version yet.");
            preview_layout->addWidget(render_doodads);
            // TODO(azure-port): WorldRender's enable/density/distance fields are private;
            // add public setters preserving Cinder's opt-in and exception-disable behavior.
            // The public preview API below works when the renderer is already enabled.
            _live_preview_cb = new QCheckBox("Preview working set on selected texture", this);
            _live_preview_cb->setChecked(true);
            _live_preview_cb->setToolTip("Previews these settings when detail-doodad rendering is enabled; the ADT and DBC are unchanged.");
            preview_layout->addWidget(_live_preview_cb);
            connect(_live_preview_cb, &QCheckBox::toggled, this, [this](bool) { updateLivePreview(); });
            connect(_spinbox_doodads_amount, qOverload<int>(&QSpinBox::valueChanged), this,
                [this](int) { updateLivePreview(); });

            _clear_target_combo = new QComboBox(this);
            _clear_target_combo->addItem("Selected effect ID (all textures)", 0);
            _clear_target_combo->addItem("Selected terrain texture (all IDs)", 1);
            _clear_target_combo->addItem("All effects", 2);
            apply_layout->addWidget(_clear_target_combo);
            auto button_clear = new QPushButton("Clear Effects", this);
            apply_layout->addWidget(button_clear);
            connect(button_clear, &QPushButton::clicked, this, &GroundEffectsTool::clearEffectsAtScope);

            auto button_sample = new QPushButton("Sample effect at cursor", this);
            selection_layout->addWidget(button_sample);
            connect(button_sample, &QPushButton::clicked, this,
                [this]() { sampleEffectAt(_map_view->cursorPosition()); });

            // Brush modes.
            {
                _brush_grup_box = new QGroupBox("Brush Mode", this);
                _brush_grup_box->setCheckable(true);
                _brush_grup_box->setChecked(false);
                left_side_layout->addWidget(_brush_grup_box);

                QVBoxLayout* brush_layout = new QVBoxLayout(_brush_grup_box);
                _brush_grup_box->setLayout(brush_layout);

                QHBoxLayout* brush_buttons_layout = new QHBoxLayout(_brush_grup_box);
                brush_layout->addLayout(brush_buttons_layout);
                _brush_type_group = new QButtonGroup(_brush_grup_box);

                _paint_effect = new QRadioButton("Paint Effect", this);
                _brush_type_group->addButton(_paint_effect);
                brush_buttons_layout->addWidget(_paint_effect);
                _paint_exclusion = new QRadioButton("Paint Exclusion", this);
                _brush_type_group->addButton(_paint_exclusion);
                brush_buttons_layout->addWidget(_paint_exclusion);

                _paint_effect->setChecked(true);
                _paint_effect->setAutoExclusive(true);

                brush_layout->addWidget(new QLabel("Radius:", _brush_grup_box));
                _effect_radius_slider = new Noggit::Ui::Tools::UiCommon::ExtendedSlider(_brush_grup_box);
                _effect_radius_slider->setPrefix("");
                _effect_radius_slider->setRange(0, 1000);
                _effect_radius_slider->setDecimals(2);
                _effect_radius_slider->setValue(_texturing_tool->texture_brush().getRadius());
                brush_layout->addWidget(_effect_radius_slider);
            }
            left_side_layout->addSpacerItem(new QSpacerItem(20, 40, QSizePolicy::Minimum, QSizePolicy::Expanding));

            connect(_render_group_box, &QGroupBox::clicked,
                [this](bool checked)
                {
                    // Checks if it is checked.
                    updateTerrainUniformParams(); 
                });

            connect(_render_active_sets, &QRadioButton::clicked,
                [this](bool checked)
                {
                    updateTerrainUniformParams();
                });

            connect(_render_placement_map, &QRadioButton::clicked,
                [this](bool checked)
                {
                    updateTerrainUniformParams();
                });

            // connect(_render_exclusion_map, &QRadioButton::clicked,
            //     [this](bool checked)
            //     {
            //         updateTerrainUniformParams();
            //     });

            connect(_brush_grup_box, &QGroupBox::clicked,
                [this](bool checked)
                {
                    updateTerrainUniformParams();
                });

            connect(_paint_effect, &QRadioButton::clicked,
                [this](bool checked)
                {
                    updateTerrainUniformParams();
                });

            connect(_paint_exclusion, &QRadioButton::clicked,
                 [this](bool checked)
                 {
                     updateTerrainUniformParams();
                 });

            // Get list of ground effect id this texture uses in this ADT.
            connect(button_scan_adt, &QPushButton::clicked
                , [=]()
                {
                    _loaded_effects.clear();
                    scanTileForEffects(TileIndex(_map_view->getCamera()->position));
                    updateSetsList();
                }
            );

            connect(button_scan_adt_loaded, &QPushButton::clicked
                , [=]()
                {
                    _loaded_effects.clear();

                    for (MapTile* tile : _map_view->getWorld()->mapIndex.loaded_tiles())
                    {
                        scanTileForEffects(TileIndex(tile->index));
                    }
                    updateSetsList();
                }
            );

            QObject::connect(_effect_sets_list, &QListWidget::itemSelectionChanged, [this]()
              {
                    auto effect = getSelectedGroundEffect();
                    if (!effect.has_value())
                    {
                        updateLivePreview();
                        return;
                    }
                    setActiveGroundEffect(effect.value());
                    _last_selected_set_id = effect->ID;
                    saveProjectSetRegistry();
                    updateLivePreview();
                    emit activeEffectChanged(effect->ID, QString::fromStdString(effect->Name));
                });

            // TODO fix this shit
            // for (int i = 0; i < 4; i++)
            // {
            //     connect(_button_effect_doodad[i], &QPushButton::clicked
            //         , [=]()
            //         {
            //             active_doodad_widget = i;
            //             _map_view->getAssetBrowserWidget()->set_browse_mode(Tools::AssetBrowser::asset_browse_mode::detail_doodads);
            //             _map_view->getAssetBrowser()->setVisible(true);
            //         }
            //     );
            // }

            connect(_object_list, &QListWidget::itemClicked, this, [=](QListWidgetItem* item)
                {
                    _map_view->getAssetBrowserWidget()->set_browse_mode(Tools::AssetBrowser::asset_browse_mode::detail_doodads);
                    _map_view->getAssetBrowser()->setVisible(true);
                }
            );

            _object_list->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(_object_list, &QWidget::customContextMenuRequested, this, [=](QPoint const& pos)
                {
                    QListWidgetItem* item = _object_list->itemAt(pos);
                    if (!item)
                        return;

                    QMenu menu(_object_list);
                    QAction* clear_action = menu.addAction("Clear slot");
                    if (menu.exec(_object_list->mapToGlobal(pos)) == clear_action)
                    {
                        item->setText(STRING_EMPTY_DISPLAY);
                        item->setToolTip("");
                        updateDoodadPreviewRender(_object_list->row(item));
                        updateWeightShares();
                        updateLivePreview();
                    }
                }
            );

            using AssetBrowser = Noggit::Ui::Tools::AssetBrowser::Ui::AssetBrowserWidget;
            connect(map_view->getAssetBrowserWidget(), &AssetBrowser::selectionChanged, this, [=](std::string const& path) {
                if (isVisible()) setDoodadSlotFromBrowser(path.c_str());
                });

            loadBlizzardGroundEffectIds();
            loadProjectSetRegistry();
            updateSetsList();
            updateBlizzardAssignmentsCombo();
        }

        void GroundEffectsTool::updateTerrainUniformParams()
        {
            if (_map_view->getWorld()->renderer()->getTerrainParamsUniformBlock()->draw_groundeffectid_overlay != render_active_sets_overlay())
            {
                _map_view->getWorld()->renderer()->getTerrainParamsUniformBlock()->draw_groundeffectid_overlay = render_active_sets_overlay();
                _map_view->getWorld()->renderer()->markTerrainParamsUniformBlockDirty();
            }
            if (_map_view->getWorld()->renderer()->getTerrainParamsUniformBlock()->draw_groundeffect_layerid_overlay != render_placement_map_overlay())
            {
                _map_view->getWorld()->renderer()->getTerrainParamsUniformBlock()->draw_groundeffect_layerid_overlay = render_placement_map_overlay();
                _map_view->getWorld()->renderer()->markTerrainParamsUniformBlockDirty();
            }
            if (_map_view->getWorld()->renderer()->getTerrainParamsUniformBlock()->draw_noeffectdoodad_overlay != render_exclusion_map_overlay())
            {
                _map_view->getWorld()->renderer()->getTerrainParamsUniformBlock()->draw_noeffectdoodad_overlay = render_exclusion_map_overlay();
                _map_view->getWorld()->renderer()->markTerrainParamsUniformBlockDirty();
            }
        }

        void GroundEffectsTool::scanTileForEffects(TileIndex tile_index)
        {
            std::string active_texture = _texturing_tool->_current_texture
                ? _texturing_tool->_current_texture->filename() : std::string();

            if (active_texture.empty() || active_texture == "tileset\\generic\\black.blp")
            {
                return;
            }    

            // could use a map to store number of users.
            // std::unordered_set<unsigned int> texture_effect_ids;
            // std::unordered_map<unsigned int, int> texture_effect_ids;

            MapTile* tile(_map_view->getWorld()->mapIndex.getTile(tile_index));
            if (!tile || !tile->finishedLoading()) return;
            for (int x = 0; x < 16; x++)
            {
                for (int y = 0; y < 16; y++)
                {
                    auto chunk = tile->getChunk(x, y);
                    for (int layer_id = 0; layer_id < chunk->getTextureSet()->num(); layer_id++)
                    {
                        auto texture_name = chunk->getTextureSet()->filename(layer_id);
                        if (texture_name == active_texture)
                        {
                            unsigned int const effect_id = chunk->getTextureSet()->getEffectForLayer(layer_id);

                            if (effect_id && !(effect_id == 0xFFFFFFFF))
                            {
                                ground_effect_set ground_effect;

                                if (_ground_effect_cache.contains(effect_id)) {
                                    ground_effect = _ground_effect_cache.at(effect_id);
                                }
                                else {
                                    ground_effect.load_from_id(effect_id);
                                    _ground_effect_cache[effect_id] = ground_effect;
                                }

                                if (ground_effect.empty())
                                    continue;

                                bool is_duplicate = false;

                                for (int i = 0; i < _loaded_effects.size(); i++)
                                    // for (auto& effect_set : _loaded_effects)
                                {
                                    auto effect_set = &_loaded_effects[i];
                                    // always filter identical ids
                                    if (effect_id == effect_set->ID
                                        || (_chkbox_merge_duplicates->isChecked() && ground_effect == effect_set))
                                    {
                                        is_duplicate = true;
                                        // _duplicate_effects[i].push_back(ground_effect); // mapped by loaded index, could use effect id ?
                                        break;
                                    }
                                }
                                if (!is_duplicate)
                                {
                                    _loaded_effects.push_back(ground_effect);
                                    // give it a name
                                    // Area is probably useless if we merge since duplictes are per area.
                                    _loaded_effects.back().Name += " - " + gAreaDB.getAreaFullName(chunk->getAreaID());
                                }

                                // _texture_effect_ids[effect_id]++;
                            }
                        }
                    }
                }
            }
        }

        void GroundEffectsTool::updateSetsList()
        {
            mergeProjectSetsIntoLoaded();
            // Rebuilding the list must not erase the persisted selection through transient signals.
            int selected_row = 0;
            if (_last_selected_set_id)
            {
                for (int row = 0; row < static_cast<int>(_loaded_effects.size()); ++row)
                    if (_loaded_effects[row].ID == _last_selected_set_id) { selected_row = row; break; }
            }
            {
                QSignalBlocker const blocker(_effect_sets_list);
                _effect_sets_list->clear();
                genEffectColors();
                for (int row = 0; row < static_cast<int>(_loaded_effects.size()); ++row)
                {
                    auto const& set = _loaded_effects[row];
                    auto const& rgb = _effects_colors[row];
                    QColor const color = QColor::fromRgbF(rgb.r, rgb.g, rgb.b);
                    auto item = new QListWidgetItem(QString::fromStdString(set.Name), _effect_sets_list);
                    item->setBackground(color);
                    QPixmap pixmap(_effect_sets_list->iconSize());
                    pixmap.fill(color);
                    item->setIcon(QIcon(pixmap));
                }
                if (!_loaded_effects.empty()) _effect_sets_list->setCurrentRow(selected_row);
            }
            auto const effect = getSelectedGroundEffect();
            setActiveGroundEffect(effect.value_or(ground_effect_set{}));
            emit activeEffectChanged(effect ? effect->ID : 0,
                effect ? QString::fromStdString(effect->Name) : QString());
        }

        void GroundEffectsTool::genEffectColors()
        {
            _effects_colors.clear();

            int color_count = 1;
            for (auto& effect : _loaded_effects)
            {
                // Same formula as in the shader.
                float partr, partg, partb;
                // TODO : Can use id instead of count?
                float r = modf(sin(glm::dot(glm::vec2(color_count), glm::vec2(12.9898, 78.233))) * 43758.5453, &partr);
                float g = modf(sin(glm::dot(glm::vec2(color_count), glm::vec2(11.5591, 70.233))) * 43569.5451, &partg);
                float b = modf(sin(glm::dot(glm::vec2(color_count), glm::vec2(13.1234, 76.234))) * 43765.5452, &partg);
                color_count++;
                _effects_colors.push_back(glm::vec3(r, g, b));
            }

            std::string active_texture = _texturing_tool->_current_texture
                ? _texturing_tool->_current_texture->filename() : std::string();
            // Check in loop instead to clear data everytime.
            for (MapTile* tile : _map_view->getWorld()->mapIndex.loaded_tiles())
            {
                if (!tile || !tile->finishedLoading()) continue;
                tile->renderer()->setActiveRenderGEffectTexture(active_texture);
                for (int x = 0; x < 16; x++)
                {
                    for (int y = 0; y < 16; y++)
                    {
                        auto chunk = tile->getChunk(x, y);

                        int chunk_index = chunk->px * 16 + chunk->py;

                        // reset to black by default
                        tile->renderer()->setChunkGroundEffectColor(chunk_index, glm::vec3(0.0, 0.0, 0.0));

                        // ! Set the chunk active layer data.
                        // new system : just update the active texture and mark dirty to the renderer
                        // tile->renderer()->setChunkGroundEffectActiveData(chunk);

                        if (active_texture.empty() || active_texture == "tileset\\generic\\black.blp" || _loaded_effects.empty())
                            continue;

                        for (int layer_id = 0; layer_id < chunk->getTextureSet()->num(); layer_id++)
                        {
                            auto texture_name = chunk->getTextureSet()->filename(layer_id);

                            if (texture_name == active_texture)
                            {
                                unsigned int const effect_id = chunk->getTextureSet()->getEffectForLayer(layer_id);

                                if (effect_id && !(effect_id == 0xFFFFFFFF))
                                {
                                    ground_effect_set ground_effect;

                                    if (_ground_effect_cache.contains(effect_id)) {
                                        ground_effect = _ground_effect_cache.at(effect_id);
                                    }
                                    else {
                                        ground_effect.load_from_id(effect_id);
                                        _ground_effect_cache[effect_id] = ground_effect;
                                    }

                                    int count = -1;
                                    bool found_debug = false;
                                    for (auto& effect_set : _loaded_effects)
                                    {
                                        count++;
                                        if (effect_id == effect_set.ID)
                                        {
                                            tile->renderer()->setChunkGroundEffectColor(chunk_index, _effects_colors[count]);
                                            found_debug = true;
                                            break;
                                        }
                                        if (_chkbox_merge_duplicates->isChecked() && (ground_effect == &effect_set)) // do deep comparison, find those that have the same effect as loaded effects, but diff id.
                                        {
                                            if (ground_effect.empty())
                                                continue;
                                            // same color
                                            tile->renderer()->setChunkGroundEffectColor(chunk_index, _effects_colors[count]);
                                            found_debug = true;
                                            break;
                                        }
                                    }
                                    // in case some chunks couldn't be resolved, paint them in pure red
                                    if (!found_debug)
                                        tile->renderer()->setChunkGroundEffectColor(chunk_index, glm::vec3(1.0, 0.0, 0.0));
                                }
                                break;
                            }
                        }
                    }
                }

            }
        }

        void GroundEffectsTool::TextureChanged()
        {
            _loaded_effects.clear();
            _ground_effect_cache.clear();

            // Establish empty-editor defaults before selecting a scanned set.
            // updateSetsList() then loads the selected record's real values.
            _spinbox_doodads_amount->setValue(8);
            _cbbox_terrain_type->setCurrentIndex(0);
            for (int i = 0; i < 4; i++)
            {
                _weight_spinboxes[i]->setValue(1);
                _object_list->item(i)->setText(STRING_EMPTY_DISPLAY);
                updateDoodadPreviewRender(i);
            }

            // Keep the set list in sync with what the user can currently see.
            // Project sets are merged by updateSetsList().
            for (MapTile* tile : _map_view->getWorld()->mapIndex.loaded_tiles())
            {
                if (tile && tile->finishedLoading())
                {
                    scanTileForEffects(tile->index);
                }
            }
            updateSetsList();
            updateBlizzardAssignmentsCombo();

            updateWeightShares();
            updateLivePreview();
        }

        bool GroundEffectsTool::render_active_sets_overlay() const
        {
          return isVisible() && !render_exclusion_map_overlay() && _render_active_sets->isChecked() && render_mode();
        }

        bool GroundEffectsTool::render_placement_map_overlay() const
        {
          return isVisible() && !render_exclusion_map_overlay() && _render_placement_map->isChecked() && render_mode();
        }

        bool GroundEffectsTool::render_exclusion_map_overlay() const
        {
          // return isVisible() && _render_exclusion_map->isChecked() && render_mode();
          return isVisible() && brush_mode() == ground_effect_brush_mode::exclusion;
        }

        void GroundEffectsTool::change_radius(float change)
        {
          _effect_radius_slider->setValue(static_cast<float>(_effect_radius_slider->value()) + change);
        }


        //Close event triggers, hide event.
        void GroundEffectsTool::hideEvent(QHideEvent* event)
        {
          if (_map_view->_world)
          {
            _map_view->getWorld()->renderer()->getTerrainParamsUniformBlock()->draw_groundeffectid_overlay = false;
            _map_view->getWorld()->renderer()->getTerrainParamsUniformBlock()->draw_groundeffect_layerid_overlay = false;
            _map_view->getWorld()->renderer()->getTerrainParamsUniformBlock()->draw_noeffectdoodad_overlay = false;
            _map_view->getWorld()->renderer()->markTerrainParamsUniformBlockDirty();
          }

          if (_map_view->_world) _map_view->getWorld()->renderer()->clearDetailDoodadPreview();
          QWidget::hideEvent(event);
        }

        void GroundEffectsTool::setDoodadSlotFromBrowser(QString doodad_path)
        {
            // the dbc stores a bare filename that the client resolves under
            // world/nodxt/detail/ (hardcoded prefix) - a model anywhere else
            // could never render, in noggit or in game
            QString normalized = doodad_path;
            normalized.replace('\\', '/');
            if (!normalized.startsWith("world/nodxt/detail/", Qt::CaseInsensitive))
            {
                QMessageBox::warning(this, "Ground Effect Doodad"
                    , "Detail doodads must be models under world\\nodxt\\detail\\.\nThe client resolves the dbc filename relative to that folder, so this model would never render.");
                return;
            }

            const QFileInfo info(normalized);
            const QString filename(info.fileName());

            if (_object_list->currentItem())
                _object_list->currentItem()->setText(filename);

            updateDoodadPreviewRender(_object_list->currentRow());
            updateWeightShares();
            updateLivePreview();
        }

        void GroundEffectsTool::updateDoodadPreviewRender(int slot_index)
        {
            QListWidgetItem* list_item = _object_list->item(slot_index);
            if (!list_item || !_preview_renderer) // item(-1) when nothing is selected
            {
                return;
            }

            QString filename = list_item->text();
            filename.replace(".mdx", ".m2", Qt::CaseInsensitive);
            filename.replace(".mdl", ".m2", Qt::CaseInsensitive);
            list_item->setText(filename);

            if (filename.isEmpty() || filename == STRING_EMPTY_DISPLAY)
            {
                list_item->setIcon(Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::plus));
                list_item->setToolTip("");
            }
            else
            {
                // Load preview render. A few stock records reference models that
                // no longer ship (dead Tirisfal/Silverpine/Arathi entries) - a
                // failed render must not abort filling the other slots
                QString filepath(("world/nodxt/detail/" + filename.toStdString()).c_str());
                try
                {
                    _preview_renderer->setModelOffscreen(filepath.toStdString());
                    list_item->setIcon(*_preview_renderer->renderToPixmap());
                    list_item->setToolTip(filepath);
                }
                catch (...)
                {
                    list_item->setIcon(Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::exclamationtriangle));
                    list_item->setToolTip(filepath + " (preview render failed)");
                    LogError << "Ground effect doodad preview render failed for "
                        << filepath.toStdString() << std::endl;
                }
            }
        }

        GroundEffectsTool::~GroundEffectsTool()
        {
            unload();
        }

        void GroundEffectsTool::unload()
        {
          if (!_preview_renderer)
          {
            return;
          }

          _preview_renderer->unloadOpenglData();
          delete _preview_renderer;
          _preview_renderer = nullptr;
        }

        float GroundEffectsTool::radius() const
        {
          return _effect_radius_slider->value();
        }

        ground_effect_brush_mode GroundEffectsTool::brush_mode() const
        {
            if (!_brush_grup_box->isChecked())
            {
                return ground_effect_brush_mode::none;
            }
            else if (_paint_effect->isChecked())
            {
                return ground_effect_brush_mode::effect;
            }
            else if (_paint_exclusion->isChecked())
            {
                return ground_effect_brush_mode::exclusion;
            }
            return ground_effect_brush_mode::none;
        }

        bool GroundEffectsTool::render_mode() const
        {
          return _render_group_box->isChecked();
        }

        void GroundEffectsTool::delete_renderer()
        {
          delete _preview_renderer;
          _preview_renderer = nullptr;
        }

        void GroundEffectsTool::showEvent(QShowEvent* event)
        {
          QWidget::showEvent(event);
          updateTerrainUniformParams();
          updateLivePreview();
        }

        std::optional<ground_effect_set> GroundEffectsTool::getSelectedGroundEffect()
        {
            int index = _effect_sets_list->currentIndex().row();
            if (index < 0 || index >= static_cast<int>(_loaded_effects.size()) || !_effect_sets_list->count())
            {
                return std::nullopt;
            }
            auto effect = _loaded_effects[index];
            return effect;
        }

        std::optional<glm::vec3> GroundEffectsTool::getSelectedEffectColor()
        {
            int index = _effect_sets_list->currentIndex().row();
            if (index < 0 || index >= static_cast<int>(_loaded_effects.size()) || !_effect_sets_list->count())
            {
                return std::nullopt;
            }
            glm::vec3 effect_color = _effects_colors[index];
            return effect_color;
        }

        void GroundEffectsTool::loadBlizzardGroundEffectIds()
        {
            _blizzard_ground_effect_ids.clear();

            QString const definitions_path = QString::fromStdString(
                Noggit::Application::NoggitApplication::instance()->getConfiguration()->ApplicationNoggitDefinitionsPath)
                + "\\GroundEffectIDs.json";
            QFile file(definitions_path);
            if (!file.open(QIODevice::ReadOnly))
            {
                LogError << "Couldn't load Blizzard ground-effect assignments from "
                    << definitions_path.toStdString() << std::endl;
                return;
            }

            QJsonParseError parse_error;
            QJsonDocument const document = QJsonDocument::fromJson(file.readAll(), &parse_error);
            if (parse_error.error != QJsonParseError::NoError || !document.isObject())
            {
                LogError << "Couldn't parse Blizzard ground-effect assignments from "
                    << definitions_path.toStdString() << ": "
                    << parse_error.errorString().toStdString() << std::endl;
                return;
            }

            QJsonObject const root = document.object();
            for (auto it = root.constBegin(); it != root.constEnd(); ++it)
            {
                if (!it.value().isArray())
                    continue;

                std::vector<unsigned int> effect_ids;
                for (QJsonValue const& value : it.value().toArray())
                {
                    int const effect_id = value.toInt();
                    if (effect_id > 0)
                        effect_ids.push_back(static_cast<unsigned int>(effect_id));
                }
                _blizzard_ground_effect_ids[normalizedTextureName(it.key().toStdString())]
                    = std::move(effect_ids);
            }
        }

        void GroundEffectsTool::updateBlizzardAssignmentsCombo()
        {
            if (!_blizzard_assignments_combo)
                return;

            QSignalBlocker const blocker(_blizzard_assignments_combo);
            _blizzard_assignments_combo->clear();
            _blizzard_assignments_combo->setEnabled(false);

            if (!_texturing_tool->_current_texture)
            {
                _blizzard_assignments_combo->addItem("Choose a Blizzard ground effect...");
                return;
            }

            std::string const texture = normalizedTextureName(
                _texturing_tool->_current_texture->filename());
            auto const assignments = _blizzard_ground_effect_ids.find(texture);
            if (assignments == _blizzard_ground_effect_ids.end() || assignments->second.empty())
            {
                _blizzard_assignments_combo->addItem("No Blizzard assignments found");
                _blizzard_assignments_combo->setToolTip(
                    "No Blizzard ground-effect assignments are listed for this texture.");
                return;
            }

            _blizzard_assignments_combo->addItem("Choose a Blizzard ground effect...");
            int compatible_assignments = 0;
            for (unsigned int const effect_id : assignments->second)
            {
                if (!gGroundEffectTextureDB.CheckIfIdExists(effect_id))
                    continue;

                ground_effect_set effect;
                effect.load_from_id(effect_id);
                if (effect.empty())
                    continue;

                _blizzard_assignments_combo->addItem(
                    QString::fromStdString(effect.Name), QVariant::fromValue(effect_id));
                ++compatible_assignments;
            }

            if (!compatible_assignments)
            {
                _blizzard_assignments_combo->clear();
                _blizzard_assignments_combo->addItem("No compatible assignments for this client");
                _blizzard_assignments_combo->setToolTip(
                    "Assignments exist for this texture, but none exist in the current client's "
                    "GroundEffectTexture.dbc.");
                return;
            }

            _blizzard_assignments_combo->setEnabled(true);
            _blizzard_assignments_combo->setToolTip(
                "Choose an effect Blizzard used with the current terrain texture. "
                "This only changes the editor selection; it does not apply anything to terrain.");
        }

        void GroundEffectsTool::selectBlizzardGroundEffect(int combo_index)
        {
            if (!_blizzard_assignments_combo || combo_index <= 0)
                return;

            bool valid_effect = false;
            unsigned int const effect_id = _blizzard_assignments_combo->itemData(combo_index)
                .toUInt(&valid_effect);
            if (!valid_effect || !effect_id || !gGroundEffectTextureDB.CheckIfIdExists(effect_id))
                return;

            auto loaded = std::find_if(_loaded_effects.begin(), _loaded_effects.end(),
                [effect_id](ground_effect_set const& effect) { return effect.ID == effect_id; });
            if (loaded == _loaded_effects.end())
            {
                ground_effect_set effect;
                effect.load_from_id(effect_id);
                if (effect.empty())
                {
                    QMessageBox::warning(this, "Blizzard ground effect unavailable",
                        QString("Blizzard ground-effect ID %1 is missing "
                                "from the current GroundEffectTexture.dbc.")
                            .arg(effect_id));
                    return;
                }
                _loaded_effects.push_back(effect);
                updateSetsList();
            }

            int selected_row = -1;
            for (int row = 0; row < static_cast<int>(_loaded_effects.size()); ++row)
            {
                if (_loaded_effects[row].ID == effect_id)
                {
                    selected_row = row;
                    break;
                }
            }
            if (selected_row < 0)
                return;

            QSignalBlocker const blocker(_effect_sets_list);
            _effect_sets_list->setCurrentRow(selected_row);
            setActiveGroundEffect(_loaded_effects[selected_row]);
            _last_selected_set_id = effect_id;
            saveProjectSetRegistry();
            updateLivePreview();
            emit activeEffectChanged(effect_id,
                QString::fromStdString(_loaded_effects[selected_row].Name));
        }

        void GroundEffectsTool::loadProjectSetRegistry()
        {
            _project_set_ids.clear();
            _last_selected_set_id = 0;
            QFile file(QString::fromStdString(Noggit::Project::CurrentProject::get()->ProjectPath + "/ground_effect_sets.json"));
            if (!file.open(QIODevice::ReadOnly)) return;
            QJsonParseError error;
            auto const doc = QJsonDocument::fromJson(file.readAll(), &error);
            if (error.error != QJsonParseError::NoError || !doc.isObject())
            {
                LogError << "Couldn't parse the project ground effect set registry: "
                    << error.errorString().toStdString() << std::endl;
                return;
            }
            auto const registry = doc.object();
            int const selected_id = registry["selected_set"].toInt();
            if (selected_id > 0) _last_selected_set_id = static_cast<unsigned int>(selected_id);
            for (auto const& value : registry["sets"].toArray())
            {
                int const id = value.toInt();
                if (id > 0 && std::find(_project_set_ids.begin(), _project_set_ids.end(), id) == _project_set_ids.end())
                    _project_set_ids.push_back(static_cast<unsigned int>(id));
            }
        }

        void GroundEffectsTool::saveProjectSetRegistry()
        {
            QJsonArray sets;
            for (unsigned int id : _project_set_ids) sets.append(static_cast<int>(id));
            QJsonObject registry;
            registry["sets"] = sets;
            registry["selected_set"] = static_cast<int>(_last_selected_set_id);
            QSaveFile file(QString::fromStdString(Noggit::Project::CurrentProject::get()->ProjectPath + "/ground_effect_sets.json"));
            QByteArray const data = QJsonDocument(registry).toJson();
            if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
                LogError << "Couldn't write the ground effect set registry to the project folder." << std::endl;
        }

        void GroundEffectsTool::mergeProjectSetsIntoLoaded()
        {
            std::unordered_set<unsigned int> known_ids;
            known_ids.reserve(_loaded_effects.size());
            for (auto const& effect : _loaded_effects)
            {
                known_ids.insert(effect.ID);
            }

            for (unsigned int id : _project_set_ids)
            {
                if (known_ids.contains(id))
                {
                    continue;
                }

                ground_effect_set set;
                set.load_from_id(id);
                if (set.empty()) // record no longer in the dbc
                {
                    continue;
                }
                set.Name += " [saved]";
                _loaded_effects.push_back(set);
                known_ids.insert(id);
            }
        }

        std::optional<unsigned int> GroundEffectsTool::sampleEffectAt(glm::vec3 const& pos)
        {
            MapChunk* chunk = _map_view->getWorld()->getChunkAt(pos);
            if (!chunk || !chunk->getTextureSet() || !chunk->getTextureSet()->num())
            {
                return std::nullopt;
            }

            TextureSet* texture_set = chunk->getTextureSet();
            if (chunk->doodadMappingNeedsUpdate())
            {
                texture_set->updateDoodadMapping();
                chunk->clearDoodadMappingNeedsUpdate();
            }

            unsigned int const column = static_cast<unsigned int>(std::clamp(
                static_cast<int>(std::floor((pos.x - chunk->xbase) / UNITSIZE)), 0, 7));
            unsigned int const row = static_cast<unsigned int>(std::clamp(
                static_cast<int>(std::floor((pos.z - chunk->zbase) / UNITSIZE)), 0, 7));
            unsigned int const layer = texture_set->getDoodadActiveLayerIdAt(column, row);
            if (layer >= texture_set->num())
            {
                return std::nullopt;
            }

            unsigned int const effect_id = texture_set->getEffectForLayer(layer);
            if (!effect_id || effect_id == 0xFFFFFFFF || !gGroundEffectTextureDB.CheckIfIdExists(effect_id))
            {
                return std::nullopt;
            }

            auto found = std::find_if(_loaded_effects.begin(), _loaded_effects.end(),
                [effect_id](ground_effect_set const& effect) { return effect.ID == effect_id; });
            if (found == _loaded_effects.end())
            {
                ground_effect_set effect;
                effect.load_from_id(effect_id);
                if (effect.empty())
                {
                    return std::nullopt;
                }
                _loaded_effects.push_back(effect);
                updateSetsList();
            }

            for (int i = 0; i < static_cast<int>(_loaded_effects.size()); ++i)
            {
                if (_loaded_effects[i].ID == effect_id)
                {
                    _effect_sets_list->setCurrentRow(i);
                    setActiveGroundEffect(_loaded_effects[i]);
                    _last_selected_set_id = effect_id;
                    saveProjectSetRegistry();
                    updateLivePreview();
                    emit activeEffectChanged(effect_id, QString::fromStdString(_loaded_effects[i].Name));
                    return effect_id;
                }
            }
            return std::nullopt;
        }

        void GroundEffectsTool::createNewSet()
        {
            ground_effect_set new_set;
            new_set.Name = "New Set (unsaved)";
            new_set.Amount = 8;
            _loaded_effects.push_back(new_set);
            updateSetsList();
            _effect_sets_list->setCurrentRow(_effect_sets_list->count() - 1);
        }

        void GroundEffectsTool::duplicateSelectedSet()
        {
            int const index = _effect_sets_list->currentIndex().row();
            if (_loaded_effects.empty() || index < 0 || index >= static_cast<int>(_loaded_effects.size()))
            {
                QMessageBox::information(this, "Duplicate Ground Effect Set", "Select a set to duplicate first.");
                return;
            }

            ground_effect_set copy = _loaded_effects[index];
            copy.Name = (copy.ID ? std::to_string(copy.ID) : copy.Name) + " copy (unsaved)";
            copy.ID = 0;
            _loaded_effects.push_back(copy);
            updateSetsList();
            _effect_sets_list->setCurrentRow(_effect_sets_list->count() - 1);
        }

        void GroundEffectsTool::deleteSelectedSet()
        {
            int const index = _effect_sets_list->currentIndex().row();
            if (_loaded_effects.empty() || index < 0 || index >= static_cast<int>(_loaded_effects.size()))
            {
                QMessageBox::information(this, "Delete Ground Effect Set", "Select a set to delete first.");
                return;
            }

            ground_effect_set const set = _loaded_effects[index];

            if (set.ID)
            {
                if (QMessageBox::question(this
                    , "Delete Ground Effect Set"
                    , QString("Delete GroundEffectTexture record %1 from the dbc?\nEvery map using this id loses its effect (chunks referencing it spawn nothing).\nThis cannot be undone.").arg(set.ID)
                    , QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
                {
                    return;
                }

                gGroundEffectTextureDB.removeRecord(set.ID);
                gGroundEffectTextureDB.save();

                // cached per-chunk placements rebuild without the record
                DetailDoodads::bumpDbcStamp();

                // the overlay must stop resolving the deleted id
                _ground_effect_cache.erase(set.ID);

                _project_set_ids.erase(std::remove(_project_set_ids.begin(), _project_set_ids.end(), set.ID), _project_set_ids.end());
                if (_last_selected_set_id == set.ID) _last_selected_set_id = 0;
                saveProjectSetRegistry();

                // the shared GroundEffectDoodad records stay: other sets may use them
            }

            _loaded_effects.erase(_loaded_effects.begin() + index);
            if (set.ID)
                _loaded_effects.erase(std::remove_if(_loaded_effects.begin(), _loaded_effects.end(),
                    [set](ground_effect_set const& effect) { return effect.ID == set.ID; }), _loaded_effects.end());
            updateSetsList();
            updateBlizzardAssignmentsCombo();
            updateLivePreview();
        }

        void GroundEffectsTool::updateWeightShares()
        {
            // spinbox valueChanged fires during construction, before the labels exist
            if (!_weight_share_labels[3])
            {
                return;
            }

            // replicates the client's stride-13, 16-slot spawn table build (see
            // DetailDoodads.cpp): later writes win, the padding cycles all id
            // slots including empty ones
            int spawn_table[16];
            std::fill(spawn_table, spawn_table + 16, -1);

            int write_pos = 0;
            int total = 0;
            for (int k = 0; k < 4; ++k)
            {
                int weight = static_cast<int>(_weight_spinboxes[k]->value());
                if (weight <= 0)
                {
                    continue;
                }
                total += weight;
                while (weight--)
                {
                    spawn_table[write_pos & 15] = k;
                    write_pos += 13;
                }
            }
            while (total < 16)
            {
                spawn_table[write_pos & 15] = total & 3;
                ++total;
                write_pos += 13;
            }

            int counts[4] = { 0, 0, 0, 0 };
            for (int s = 0; s < 16; ++s)
            {
                if (spawn_table[s] >= 0)
                {
                    counts[spawn_table[s]]++;
                }
            }

            for (int k = 0; k < 4; ++k)
            {
                QListWidgetItem* item = _object_list->item(k);
                bool const has_doodad = item && !item->text().isEmpty() && item->text() != STRING_EMPTY_DISPLAY;
                int const percent = counts[k] * 100 / 16;

                if (!counts[k])
                {
                    _weight_share_labels[k]->setText("0%");
                }
                else if (has_doodad)
                {
                    _weight_share_labels[k]->setText(QString::number(percent) + "%");
                }
                else
                {
                    // table entries pointing at an empty slot spawn nothing
                    _weight_share_labels[k]->setText(QString::number(percent) + "% empty");
                }
            }
        }

        void GroundEffectsTool::updateLivePreview()
        {
            if (!_map_view || !_map_view->_world || !_live_preview_cb
                || !_live_preview_cb->isChecked() || !isVisible() || !getSelectedGroundEffect())
            {
                if (_map_view && _map_view->_world)
                {
                    _map_view->getWorld()->renderer()->clearDetailDoodadPreview();
                }
                return;
            }

            std::string const texture = _texturing_tool->_current_texture
                ? _texturing_tool->_current_texture->filename() : std::string();
            if (texture.empty() || texture == STRING_EMPTY_TEXTURE)
            {
                _map_view->getWorld()->renderer()->clearDetailDoodadPreview();
                return;
            }

            DetailDoodadPreview preview;
            preview.enabled = true;
            preview.texture = texture;
            preview.amount = static_cast<std::uint32_t>(_spinbox_doodads_amount->value());
            for (int i = 0; i < 4; ++i)
            {
                QString const filename = _object_list->item(i)->text();
                if (!filename.isEmpty() && filename != STRING_EMPTY_DISPLAY)
                {
                    preview.filenames[i] = filename.toStdString();
                }
                preview.weights[i] = _weight_spinboxes[i]->value();
            }

            _map_view->getWorld()->renderer()->setDetailDoodadPreview(std::move(preview));
        }

        void GroundEffectsTool::saveSelectedSet()
        {
            int const index = _effect_sets_list->currentIndex().row();
            if (_loaded_effects.empty() || index < 0 || index >= static_cast<int>(_loaded_effects.size()))
            {
                QMessageBox::information(this, "Save Ground Effect Set", "Select or create a set first.");
                return;
            }

            std::string filenames[4];
            bool any_doodad = false;
            for (int i = 0; i < 4; ++i)
            {
                QString const text = _object_list->item(i)->text();
                if (text.isEmpty() || text == STRING_EMPTY_DISPLAY)
                {
                    continue;
                }
                filenames[i] = text.toStdString();
                any_doodad = true;
            }

            if (!any_doodad && QMessageBox::question(this, "Save Ground Effect Set",
                "This set has no doodads. Save it as a terrain-type-only effect?",
                QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
            {
                return;
            }

            int total_weight = 0;
            for (auto* spinbox : _weight_spinboxes)
            {
                total_weight += spinbox->value();
            }
            if (total_weight > 16 && QMessageBox::question(this, "Save Ground Effect Set",
                QString("The weights total %1, but the client has only 16 selection slots. Later writes overwrite earlier ones, so the displayed effective shares—not the raw weights—are what will spawn. Save anyway?").arg(total_weight),
                QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
            {
                return;
            }

            ground_effect_set& set = _loaded_effects[index];

            QDialog save_dialog(this);
            save_dialog.setWindowFlags(Qt::Dialog | Qt::WindowCloseButtonHint);
            save_dialog.setWindowTitle("Save Ground Effect Set");
            auto dialog_layout = new QVBoxLayout(&save_dialog);
            dialog_layout->addWidget(new QLabel("GroundEffectTexture Id : ", &save_dialog));
            auto id_spinbox = new QSpinBox(&save_dialog);
            id_spinbox->setRange(1, std::numeric_limits<int>::max());
            id_spinbox->setValue(set.ID ? static_cast<int>(set.ID) : gGroundEffectTextureDB.getEmptyRecordID());
            dialog_layout->addWidget(id_spinbox);
            dialog_layout->addWidget(new QLabel("Saving to an id that already exists overwrites that record\nfor every map using it.", &save_dialog));
            auto save_button = new QPushButton("Save", &save_dialog);
            dialog_layout->addWidget(save_button);
            connect(save_button, &QPushButton::clicked, &save_dialog, &QDialog::accept);

            if (save_dialog.exec() != QDialog::Accepted)
            {
                return;
            }

            unsigned int const record_id = static_cast<unsigned int>(id_spinbox->value());

            unsigned int doodad_ids[4] = { 0, 0, 0, 0 };
            for (int i = 0; i < 4; ++i)
            {
                if (!filenames[i].empty())
                {
                    doodad_ids[i] = findOrCreateGroundEffectDoodad(filenames[i]);
                }
            }

            DBCFile::Record record = gGroundEffectTextureDB.CheckIfIdExists(record_id)
                ? gGroundEffectTextureDB.getByID(record_id)
                : gGroundEffectTextureDB.addRecord(record_id);

            for (int i = 0; i < 4; ++i)
            {
                record.write(GroundEffectTextureDB::Doodads + i, doodad_ids[i]);
                record.write(GroundEffectTextureDB::Weights + i, static_cast<unsigned int>(_weight_spinboxes[i]->value()));
            }
            record.write(GroundEffectTextureDB::Amount, static_cast<unsigned int>(_spinbox_doodads_amount->value()));
            record.write(GroundEffectTextureDB::TerrainType, _cbbox_terrain_type->currentData().toUInt());

            gGroundEffectDoodadDB.save();
            gGroundEffectTextureDB.save();

            // cached per-chunk placements rebuild against the new records
            DetailDoodads::bumpDbcStamp();

            _last_selected_set_id = record_id;
            if (std::find(_project_set_ids.begin(), _project_set_ids.end(), record_id) == _project_set_ids.end())
            {
                _project_set_ids.push_back(record_id);
            }
            saveProjectSetRegistry();

            set.ID = record_id;
            set.Amount = _spinbox_doodads_amount->value();
            set.TerrainType = _cbbox_terrain_type->currentData().toUInt();
            for (int i = 0; i < 4; ++i)
            {
                set.Doodads[i].ID = doodad_ids[i];
                set.Doodads[i].filename = filenames[i];
                set.Weights[i] = _weight_spinboxes[i]->value();
            }
            set.rebuild_name();

            ground_effect_set const saved_set = set;
            _ground_effect_cache[record_id] = saved_set;
            _loaded_effects.erase(std::remove_if(_loaded_effects.begin(), _loaded_effects.end(),
                [record_id](ground_effect_set const& effect) { return effect.ID == record_id; }), _loaded_effects.end());
            _loaded_effects.push_back(saved_set);
            updateSetsList();
            updateBlizzardAssignmentsCombo();
        }

        void GroundEffectsTool::applyEffectIdAtScope(std::string const& texture, unsigned int effect_id,
                                                     bool override_existing, QString const& global_confirm,
                                                     std::optional<unsigned int> only_effect_id)
        {
            World* world = _map_view->getWorld();
            if (!world) return;
            int const scope = _generate_type_group->checkedId();
            if (scope == 3 || _scope_disk_sweep_cb->isChecked())
            {
                // TODO(azure-port): global/zone/area disk sweep needs World::applyGroundEffectGlobal.
                Q_UNUSED(global_confirm);
                QMessageBox::information(this, "Ground Effect Scope",
                    "Assignment to unloaded tiles is not available yet. Choose a loaded zone, area, ADT or chunk.");
                return;
            }
            if (scope < 0 || scope > 4) return;

            glm::vec3 const camera_pos = _map_view->getCamera()->position;
            MapChunk* camera_chunk = world->getChunkAt(camera_pos);
            MapTile* camera_tile = world->mapIndex.getTile(TileIndex(camera_pos));
            if (!camera_chunk || !camera_tile || !camera_tile->finishedLoading())
            {
                QMessageBox::information(this, "Ground Effect Scope", "The camera is not over a loaded tile.");
                return;
            }
            unsigned int const target_area = scope == 0
                ? groundEffectZoneId(camera_chunk->getAreaID()) : camera_chunk->getAreaID();
            std::string const matching_texture = normalizedTextureName(texture);

            // Cinder has layer-info undo and setEffect(), but not Azure's World scope helpers.
            // Work on loaded tiles locally, marking only tiles with changed assignments.
            bool const owns_action = NOGGIT_CUR_ACTION == nullptr;
            if (owns_action)
                NOGGIT_ACTION_MGR->beginAction(_map_view, Noggit::ActionFlags::eCHUNKS_LAYERINFO);

            for (MapTile* tile : world->mapIndex.loaded_tiles())
            {
                if (!tile || !tile->finishedLoading() || ((scope == 2 || scope == 4) && tile != camera_tile))
                    continue;
                bool changed = false;
                for (int x = 0; x < 16; ++x)
                {
                    for (int y = 0; y < 16; ++y)
                    {
                        MapChunk* chunk = tile->getChunk(x, y);
                        if (!chunk || (scope == 4 && chunk != camera_chunk)) continue;
                        if (scope == 0 && groundEffectZoneId(chunk->getAreaID()) != target_area) continue;
                        if (scope == 1 && chunk->getAreaID() != target_area) continue;
                        TextureSet* textures = chunk->getTextureSet();
                        if (!textures) continue;
                        bool registered = false;
                        for (std::size_t layer = 0; layer < textures->num(); ++layer)
                        {
                            if (!matching_texture.empty() && normalizedTextureName(textures->filename(layer)) != matching_texture)
                                continue;
                            unsigned int const old_id = textures->getEffectForLayer(layer);
                            if (only_effect_id && old_id != *only_effect_id) continue;
                            if (!override_existing && old_id && old_id != 0xFFFFFFFF) continue;
                            if (old_id == effect_id || (!effect_id && old_id == 0xFFFFFFFF)) continue;
                            if (!registered)
                            {
                                NOGGIT_CUR_ACTION->registerChunkLayerInfoChange(chunk);
                                registered = true;
                            }
                            textures->setEffect(layer, static_cast<int>(effect_id));
                            changed = true;
                        }
                    }
                }
                if (changed) world->mapIndex.setChanged(tile);
            }
            if (owns_action) NOGGIT_ACTION_MGR->endAction();
            genEffectColors();
        }

        void GroundEffectsTool::applySelectedSet()
        {
            auto effect = getSelectedGroundEffect();
            if (!effect.has_value() || !effect->ID)
            {
                QMessageBox::information(this, "Apply Ground Effect", "Select a saved set first (unsaved sets have no id to apply).");
                return;
            }

            std::string const texture = _texturing_tool->_current_texture
                ? _texturing_tool->_current_texture->filename() : std::string();
            if (texture.empty() || texture == STRING_EMPTY_TEXTURE)
            {
                QMessageBox::information(this, "Apply Ground Effect", "Select a texture first.");
                return;
            }

            applyEffectIdAtScope(texture, effect->ID, _apply_override_cb->isChecked()
                , QString("Apply this effect to the texture on every ADT of the map?\n") + STRING_DISK_SWEEP_WARNING);
        }

        void GroundEffectsTool::clearEffectsAtScope()
        {
            std::string texture;
            std::optional<unsigned int> only_effect_id;
            QString confirmation;

            switch (_clear_target_combo->currentData().toInt())
            {
            case 0: // selected effect id, regardless of texture
            {
                auto const effect = getSelectedGroundEffect();
                if (!effect.has_value() || !effect->ID)
                {
                    QMessageBox::information(this, "Clear Ground Effects", "Select a saved effect set first.");
                    return;
                }
                only_effect_id = effect->ID;
                confirmation = QString("Remove only ground effect ID %1 from every ADT of the map?\n").arg(effect->ID)
                             + STRING_DISK_SWEEP_WARNING;
                break;
            }
            case 1: // selected terrain texture, regardless of effect id
                texture = _texturing_tool->_current_texture
                    ? _texturing_tool->_current_texture->filename() : std::string();
                if (texture.empty() || texture == STRING_EMPTY_TEXTURE)
                {
                    QMessageBox::information(this, "Clear Ground Effects", "Select a terrain texture first.");
                    return;
                }
                confirmation = QString("Remove every ground effect assigned to texture\n%1\nfrom every ADT of the map?\n")
                             .arg(QString::fromStdString(texture)) + STRING_DISK_SWEEP_WARNING;
                break;
            case 2: // every effect on every texture
                confirmation = QString("Remove ALL ground effects from every ADT of the map?\n") + STRING_DISK_SWEEP_WARNING;
                break;
            default:
                return;
            }

            applyEffectIdAtScope(texture, 0, true, confirmation, only_effect_id);
        }

        void GroundEffectsTool::setActiveGroundEffect(ground_effect_set const& effect)
        {
            // Sets a ground effect to be actively selected in the UI.
            QSignalBlocker const amount_blocker(_spinbox_doodads_amount);
            _spinbox_doodads_amount->setValue(effect.Amount);
            int const terrain_type_index = _cbbox_terrain_type->findData(QVariant(effect.TerrainType));
            _cbbox_terrain_type->setCurrentIndex(terrain_type_index >= 0 ? terrain_type_index : 0);

            for (int i = 0; i < 4; ++i)
            {
                QSignalBlocker const weight_blocker(_weight_spinboxes[i]);
                _weight_spinboxes[i]->setValue(effect.Weights[i]);
                QString filename(effect.Doodads[i].filename.c_str());
                // Replace old extensions in the DBC.
                filename = filename.replace(".mdx", ".m2", Qt::CaseInsensitive);
                filename = filename.replace(".mdl", ".m2", Qt::CaseInsensitive);

                // TODO turn this into an array of elements.
                if (filename.isEmpty())
                {
                    _object_list->item(i)->setText(STRING_EMPTY_DISPLAY);
                }

                else
                {
                    _object_list->item(i)->setText(filename);
                }
                updateDoodadPreviewRender(i);
            }

            updateWeightShares();
            updateLivePreview();
        }

        void ground_effect_set::load_from_id(unsigned int effect_id)
        {
            if (!effect_id || (effect_id == 0xFFFFFFFF))
            {
                return;
            }
                
            if (!gGroundEffectTextureDB.CheckIfIdExists(effect_id))
            {
                return;
            }

            try
            {
                DBCFile::Record GErecord = gGroundEffectTextureDB.getByID(effect_id);
                Name = std::to_string(effect_id);
                ID = GErecord.getUInt(GroundEffectTextureDB::ID);
                Amount = GErecord.getUInt(GroundEffectTextureDB::Amount);
                TerrainType = GErecord.getUInt(GroundEffectTextureDB::TerrainType);

                for (int i = 0; i < 4; ++i)
                {
                    Weights[i] = GErecord.getUInt(GroundEffectTextureDB::Weights + i);
                    unsigned const curDoodadId
                    { 
                        GErecord.getUInt(GroundEffectTextureDB::Doodads + i) 
                    };

                    if (!curDoodadId)
                    {
                        continue;
                    }
                    
                    if (!gGroundEffectDoodadDB.CheckIfIdExists(curDoodadId))
                    {
                        continue;
                    }
                        
                    Doodads[i].ID = curDoodadId;
                    QString filename = gGroundEffectDoodadDB.getByID(curDoodadId).getString(GroundEffectDoodadDB::Filename);

                    filename.replace(".mdx", ".m2", Qt::CaseInsensitive);
                    filename.replace(".mdl", ".m2", Qt::CaseInsensitive);

                    Doodads[i].filename = filename.toStdString();
                }
                rebuild_name();
            }
            catch (GroundEffectTextureDB::NotFound)
            {
                ID = 0;
                assert(false);
                LogError << "Couldn't find ground effect Id : " << effect_id << "in GroundEffectTexture.dbc" << std::endl;
            }
        }

        void ground_effect_set::rebuild_name()
        {
            std::string doodad_names;
            int doodad_count = 0;
            for (int i = 0; i < 4; ++i)
            {
                if (Doodads[i].filename.empty())
                {
                    continue;
                }
                doodad_count++;
                if (doodad_count <= 2)
                {
                    std::string stem = Doodads[i].filename;
                    size_t const dot = stem.find_last_of('.');
                    if (dot != std::string::npos)
                    {
                        stem.resize(dot);
                    }
                    if (!doodad_names.empty())
                    {
                        doodad_names += ", ";
                    }
                    doodad_names += stem;
                }
            }

            Name = std::to_string(ID);
            if (doodad_count)
            {
                Name += " - " + doodad_names;
                if (doodad_count > 2)
                {
                    Name += " +" + std::to_string(doodad_count - 2);
                }
            }
            else
            {
                Name += " - no doodads";
            }
        }

        bool ground_effect_set::empty() const
        {
          return !ID;
        }

        bool ground_effect_set::operator== (ground_effect_set* effect2)
        {
          return (TerrainType == effect2->TerrainType && Amount == effect2->Amount
            && Doodads[0] == &effect2->Doodads[0] && Doodads[1] == &effect2->Doodads[1]
            && Doodads[2] == &effect2->Doodads[2] && Doodads[3] == &effect2->Doodads[3]
            && Weights[0] == effect2->Weights[0] && Weights[1] == effect2->Weights[1]
            && Weights[2] == effect2->Weights[2] && Weights[3] == effect2->Weights[3]
            );
        }

        bool ground_effect_doodad::empty() const
        {
          return filename.empty();
        }

        bool ground_effect_doodad::operator== (ground_effect_doodad* doodad2)
        {
          return filename == doodad2->filename;
        }
}
}
