/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <optional>

#include "MEM_guardedalloc.h"

#include "DNA_defaults.h"
#include "DNA_material_types.h"
#include "DNA_object_types.h"
#include "DNA_pointcloud_types.h"

#include "BLI_bounds.hh"
#include "BLI_index_range.hh"
#include "BLI_rand.h"
#include "BLI_resource_scope.hh"
#include "BLI_span.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "BKE_anim_data.hh"
#include "BKE_attribute_legacy_convert.hh"
#include "BKE_attribute_storage.hh"
#include "BKE_attribute_storage_blend_write.hh"
#include "BKE_bake_data_block_id.hh"
#include "BKE_customdata.hh"
#include "BKE_geometry_set.hh"
#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"
#include "BKE_lib_query.hh"
#include "BKE_modifier.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_pointcloud.hh"

#include "BLT_translation.hh"

#include "DEG_depsgraph_query.hh"

#include "BLO_read_write.hh"

using blender::CPPType;
using blender::float3;
using blender::IndexRange;
using blender::MutableSpan;
using blender::Span;
using blender::StringRef;
using blender::VArray;
using blender::Vector;

/* PointCloud datablock */

static void pointcloud_random(PointCloud *pointcloud);

constexpr StringRef ATTR_POSITION = "position";
constexpr StringRef ATTR_RADIUS = "radius";

static void pointcloud_init_data(ID *id)
{
  PointCloud *pointcloud = (PointCloud *)id;
  BLI_assert(MEMCMP_STRUCT_AFTER_IS_ZERO(pointcloud, id));

  MEMCPY_STRUCT_AFTER(pointcloud, DNA_struct_default_get(PointCloud), id);

  new (&pointcloud->attribute_storage.wrap()) blender::bke::AttributeStorage();
  pointcloud->runtime = new blender::bke::PointCloudRuntime();

  CustomData_reset(&pointcloud->pdata);
  pointcloud->attributes_for_write().add<float3>(
      "position", blender::bke::AttrDomain::Point, blender::bke::AttributeInitConstruct());
}

static void pointcloud_copy_data(Main * /*bmain*/,
                                 std::optional<Library *> /*owner_library*/,
                                 ID *id_dst,
                                 const ID *id_src,
                                 const int /*flag*/)
{
  PointCloud *pointcloud_dst = (PointCloud *)id_dst;
  const PointCloud *pointcloud_src = (const PointCloud *)id_src;
  pointcloud_dst->mat = static_cast<Material **>(MEM_dupallocN(pointcloud_src->mat));

  CustomData_init_from(
      &pointcloud_src->pdata, &pointcloud_dst->pdata, CD_MASK_ALL, pointcloud_dst->totpoint);
  new (&pointcloud_dst->attribute_storage.wrap())
      blender::bke::AttributeStorage(pointcloud_src->attribute_storage.wrap());

  pointcloud_dst->runtime = new blender::bke::PointCloudRuntime();
  pointcloud_dst->runtime->bounds_cache = pointcloud_src->runtime->bounds_cache;
  pointcloud_dst->runtime->bounds_with_radius_cache =
      pointcloud_src->runtime->bounds_with_radius_cache;
  pointcloud_dst->runtime->bvh_cache = pointcloud_src->runtime->bvh_cache;
  if (pointcloud_src->runtime->bake_materials) {
    pointcloud_dst->runtime->bake_materials =
        std::make_unique<blender::bke::bake::BakeMaterialsList>(
            *pointcloud_src->runtime->bake_materials);
  }

  pointcloud_dst->batch_cache = nullptr;
}

static void pointcloud_free_data(ID *id)
{
  PointCloud *pointcloud = (PointCloud *)id;
  BKE_animdata_free(&pointcloud->id, false);
  BKE_pointcloud_batch_cache_free(pointcloud);
  CustomData_free(&pointcloud->pdata);
  pointcloud->attribute_storage.wrap().~AttributeStorage();
  MEM_SAFE_FREE(pointcloud->mat);
  delete pointcloud->runtime;
}

static void pointcloud_foreach_id(ID *id, LibraryForeachIDData *data)
{
  PointCloud *pointcloud = (PointCloud *)id;
  for (int i = 0; i < pointcloud->totcol; i++) {
    BKE_LIB_FOREACHID_PROCESS_IDSUPER(data, pointcloud->mat[i], IDWALK_CB_USER);
  }
}

static void pointcloud_blend_write(BlendWriter *writer, ID *id, const void *id_address)
{
  using namespace blender;
  using namespace blender::bke;
  PointCloud *pointcloud = (PointCloud *)id;

  ResourceScope scope;
  Vector<CustomDataLayer, 16> point_layers;
  bke::AttributeStorage::BlendWriteData attribute_data{scope};
  attribute_storage_blend_write_prepare(
      pointcloud->attribute_storage.wrap(), {{AttrDomain::Point, &point_layers}}, attribute_data);
  CustomData_blend_write_prepare(
      pointcloud->pdata, AttrDomain::Point, pointcloud->totpoint, point_layers, attribute_data);
  pointcloud->attribute_storage.dna_attributes = attribute_data.attributes.data();
  pointcloud->attribute_storage.dna_attributes_num = attribute_data.attributes.size();

  /* Write LibData */
  BLO_write_id_struct(writer, PointCloud, id_address, &pointcloud->id);
  BKE_id_blend_write(writer, &pointcloud->id);

  /* Direct data */
  CustomData_blend_write(writer,
                         &pointcloud->pdata,
                         point_layers,
                         pointcloud->totpoint,
                         CD_MASK_ALL,
                         &pointcloud->id);
  pointcloud->attribute_storage.wrap().blend_write(*writer, attribute_data);

  BLO_write_pointer_array(writer, pointcloud->totcol, pointcloud->mat);
}

static void pointcloud_blend_read_data(BlendDataReader *reader, ID *id)
{
  PointCloud *pointcloud = (PointCloud *)id;

  /* Geometry */
  CustomData_blend_read(reader, &pointcloud->pdata, pointcloud->totpoint);
  pointcloud->attribute_storage.wrap().blend_read(*reader);

  /* Forward compatibility. To be removed when runtime format changes. */
  blender::bke::pointcloud_convert_storage_to_customdata(*pointcloud);

  /* Materials */
  BLO_read_pointer_array(reader, pointcloud->totcol, (void **)&pointcloud->mat);

  pointcloud->runtime = new blender::bke::PointCloudRuntime();
}

IDTypeInfo IDType_ID_PT = {
    /*id_code*/ PointCloud::id_type,
    /*id_filter*/ FILTER_ID_PT,
    /*dependencies_id_types*/ FILTER_ID_MA,
    /*main_listbase_index*/ INDEX_ID_PT,
    /*struct_size*/ sizeof(PointCloud),
    /*name*/ "PointCloud",
    /*name_plural*/ N_("pointclouds"),
    /*translation_context*/ BLT_I18NCONTEXT_ID_POINTCLOUD,
    /*flags*/ IDTYPE_FLAGS_APPEND_IS_REUSABLE,
    /*asset_type_info*/ nullptr,

    /*init_data*/ pointcloud_init_data,
    /*copy_data*/ pointcloud_copy_data,
    /*free_data*/ pointcloud_free_data,
    /*make_local*/ nullptr,
    /*foreach_id*/ pointcloud_foreach_id,
    /*foreach_cache*/ nullptr,
    /*foreach_path*/ nullptr,
    /*owner_pointer_get*/ nullptr,

    /*blend_write*/ pointcloud_blend_write,
    /*blend_read_data*/ pointcloud_blend_read_data,
    /*blend_read_after_liblink*/ nullptr,

    /*blend_read_undo_preserve*/ nullptr,

    /*lib_override_apply_post*/ nullptr,
};

static void pointcloud_random(PointCloud *pointcloud)
{
  using namespace blender;
  using namespace blender::bke;
  BLI_assert(pointcloud->totpoint == 0);
  pointcloud->totpoint = 400;
  CustomData_realloc(&pointcloud->pdata, 0, pointcloud->totpoint);

  RNG *rng = BLI_rng_new(0);

  MutableAttributeAccessor attributes = pointcloud->attributes_for_write();
  MutableSpan<float3> positions = pointcloud->positions_for_write();
  SpanAttributeWriter<float> radii = attributes.lookup_or_add_for_write_only_span<float>(
      ATTR_RADIUS, AttrDomain::Point);

  for (const int i : positions.index_range()) {
    positions[i] = float3(BLI_rng_get_float(rng), BLI_rng_get_float(rng), BLI_rng_get_float(rng)) *
                       2.0f -
                   1.0f;
    radii.span[i] = 0.05f * BLI_rng_get_float(rng);
  }

  radii.finish();

  BLI_rng_free(rng);
}

template<typename T>
static VArray<T> get_varray_attribute(const PointCloud &pointcloud,
                                      const StringRef name,
                                      const T default_value)
{
  const eCustomDataType type = blender::bke::cpp_type_to_custom_data_type(CPPType::get<T>());

  const T *data = (const T *)CustomData_get_layer_named(&pointcloud.pdata, type, name);
  if (data != nullptr) {
    return VArray<T>::ForSpan(Span<T>(data, pointcloud.totpoint));
  }
  return VArray<T>::ForSingle(default_value, pointcloud.totpoint);
}

template<typename T>
static Span<T> get_span_attribute(const PointCloud &pointcloud, const StringRef name)
{
  const eCustomDataType type = blender::bke::cpp_type_to_custom_data_type(CPPType::get<T>());

  T *data = (T *)CustomData_get_layer_named(&pointcloud.pdata, type, name);
  if (data == nullptr) {
    return {};
  }
  return {data, pointcloud.totpoint};
}

template<typename T>
static MutableSpan<T> get_mutable_attribute(PointCloud &pointcloud,
                                            const StringRef name,
                                            const T default_value = T())
{
  if (pointcloud.totpoint <= 0) {
    return {};
  }
  const eCustomDataType type = blender::bke::cpp_type_to_custom_data_type(CPPType::get<T>());

  T *data = (T *)CustomData_get_layer_named_for_write(
      &pointcloud.pdata, type, name, pointcloud.totpoint);
  if (data != nullptr) {
    return {data, pointcloud.totpoint};
  }
  data = (T *)CustomData_add_layer_named(
      &pointcloud.pdata, type, CD_SET_DEFAULT, pointcloud.totpoint, name);
  MutableSpan<T> span = {data, pointcloud.totpoint};
  if (pointcloud.totpoint > 0 && span.first() != default_value) {
    span.fill(default_value);
  }
  return span;
}

Span<float3> PointCloud::positions() const
{
  return get_span_attribute<float3>(*this, "position");
}
MutableSpan<float3> PointCloud::positions_for_write()
{
  return get_mutable_attribute<float3>(*this, "position");
}

VArray<float> PointCloud::radius() const
{
  return get_varray_attribute<float>(*this, "radius", 0.01f);
}
MutableSpan<float> PointCloud::radius_for_write()
{
  return get_mutable_attribute<float>(*this, "radius", 0.01f);
}

PointCloud *BKE_pointcloud_add(Main *bmain, const char *name)
{
  PointCloud *pointcloud = BKE_id_new<PointCloud>(bmain, name);

  return pointcloud;
}

PointCloud *BKE_pointcloud_add_default(Main *bmain, const char *name)
{
  PointCloud *pointcloud = BKE_id_new<PointCloud>(bmain, name);

  pointcloud_random(pointcloud);

  return pointcloud;
}

PointCloud *BKE_pointcloud_new_nomain(const int totpoint)
{
  PointCloud *pointcloud = static_cast<PointCloud *>(BKE_libblock_alloc(
      nullptr, ID_PT, BKE_idtype_idcode_to_name(ID_PT), LIB_ID_CREATE_LOCALIZE));

  BKE_libblock_init_empty(&pointcloud->id);

  CustomData_realloc(&pointcloud->pdata, 0, totpoint);
  pointcloud->totpoint = totpoint;

  return pointcloud;
}

void BKE_pointcloud_nomain_to_pointcloud(PointCloud *pointcloud_src, PointCloud *pointcloud_dst)
{
  BLI_assert(pointcloud_src->id.tag & ID_TAG_NO_MAIN);

  CustomData_free(&pointcloud_dst->pdata);

  const int totpoint = pointcloud_dst->totpoint = pointcloud_src->totpoint;
  CustomData_init_from(&pointcloud_src->pdata, &pointcloud_dst->pdata, CD_MASK_ALL, totpoint);

  pointcloud_dst->runtime->bounds_cache = pointcloud_src->runtime->bounds_cache;
  pointcloud_dst->runtime->bounds_with_radius_cache =
      pointcloud_src->runtime->bounds_with_radius_cache;
  pointcloud_dst->runtime->bvh_cache = pointcloud_src->runtime->bvh_cache;
  BKE_id_free(nullptr, pointcloud_src);
}

std::optional<blender::Bounds<float3>> PointCloud::bounds_min_max(const bool use_radius) const
{
  using namespace blender;
  using namespace blender::bke;
  if (this->totpoint == 0) {
    return std::nullopt;
  }
  if (use_radius) {
    this->runtime->bounds_with_radius_cache.ensure([&](Bounds<float3> &r_bounds) {
      const VArray<float> radius = this->radius();
      if (const std::optional radius_single = radius.get_if_single()) {
        r_bounds = *this->bounds_min_max(false);
        r_bounds.pad(*radius_single);
        return;
      }
      const Span radius_span = radius.get_internal_span();
      r_bounds = *bounds::min_max_with_radii(this->positions(), radius_span);
    });
  }
  else {
    this->runtime->bounds_cache.ensure(
        [&](Bounds<float3> &r_bounds) { r_bounds = *bounds::min_max(this->positions()); });
  }
  return use_radius ? this->runtime->bounds_with_radius_cache.data() :
                      this->runtime->bounds_cache.data();
}

std::optional<int> PointCloud::material_index_max() const
{
  if (this->totpoint == 0) {
    return std::nullopt;
  }
  std::optional<int> max_material_index = blender::bounds::max<int>(
      this->attributes()
          .lookup_or_default<int>("material_index", blender::bke::AttrDomain::Point, 0)
          .varray);
  if (max_material_index.has_value()) {
    max_material_index = std::clamp(*max_material_index, 0, MAXMAT);
  }
  return max_material_index;
}

void PointCloud::count_memory(blender::MemoryCounter &memory) const
{
  CustomData_count_memory(this->pdata, this->totpoint, memory);
}

blender::bke::AttributeAccessor PointCloud::attributes() const
{
  return blender::bke::AttributeAccessor(this,
                                         blender::bke::pointcloud_attribute_accessor_functions());
}

blender::bke::MutableAttributeAccessor PointCloud::attributes_for_write()
{
  return blender::bke::MutableAttributeAccessor(
      this, blender::bke::pointcloud_attribute_accessor_functions());
}

bool BKE_pointcloud_attribute_required(const PointCloud * /*pointcloud*/,
                                       const blender::StringRef name)
{
  return name == ATTR_POSITION;
}

void pointcloud_copy_parameters(const PointCloud &src, PointCloud &dst)
{
  dst.flag = src.flag;
  MEM_SAFE_FREE(dst.mat);
  dst.mat = MEM_malloc_arrayN<Material *>(src.totcol, __func__);
  dst.totcol = src.totcol;
  MutableSpan(dst.mat, dst.totcol).copy_from(Span(src.mat, src.totcol));
}

/* Dependency Graph */

PointCloud *BKE_pointcloud_copy_for_eval(const PointCloud *pointcloud_src)
{
  return reinterpret_cast<PointCloud *>(
      BKE_id_copy_ex(nullptr, &pointcloud_src->id, nullptr, LIB_ID_COPY_LOCALIZE));
}

static void pointcloud_evaluate_modifiers(Depsgraph *depsgraph,
                                          Scene *scene,
                                          Object *object,
                                          blender::bke::GeometrySet &geometry_set)
{
  /* Modifier evaluation modes. */
  const bool use_render = (DEG_get_mode(depsgraph) == DAG_EVAL_RENDER);
  const int required_mode = use_render ? eModifierMode_Render : eModifierMode_Realtime;
  ModifierApplyFlag apply_flag = use_render ? MOD_APPLY_RENDER : MOD_APPLY_USECACHE;
  const ModifierEvalContext mectx = {depsgraph, object, apply_flag};

  BKE_modifiers_clear_errors(object);

  /* Get effective list of modifiers to execute. Some effects like shape keys
   * are added as virtual modifiers before the user created modifiers. */
  VirtualModifierData virtual_modifier_data;
  ModifierData *md = BKE_modifiers_get_virtual_modifierlist(object, &virtual_modifier_data);

  /* Evaluate modifiers. */
  for (; md; md = md->next) {
    const ModifierTypeInfo *mti = BKE_modifier_get_info((ModifierType)md->type);

    if (!BKE_modifier_is_enabled(scene, md, required_mode)) {
      continue;
    }

    blender::bke::ScopedModifierTimer modifier_timer{*md};

    if (mti->modify_geometry_set) {
      mti->modify_geometry_set(md, &mectx, &geometry_set);
    }
  }
}

static PointCloud *take_pointcloud_ownership_from_geometry_set(
    blender::bke::GeometrySet &geometry_set)
{
  if (!geometry_set.has<blender::bke::PointCloudComponent>()) {
    return nullptr;
  }
  blender::bke::PointCloudComponent &pointcloud_component =
      geometry_set.get_component_for_write<blender::bke::PointCloudComponent>();
  PointCloud *pointcloud = pointcloud_component.release();
  if (pointcloud != nullptr) {
    /* Add back, but as read-only non-owning component. */
    pointcloud_component.replace(pointcloud, blender::bke::GeometryOwnershipType::ReadOnly);
  }
  else {
    /* The component was empty, we can also remove it. */
    geometry_set.remove<blender::bke::PointCloudComponent>();
  }
  return pointcloud;
}

void BKE_pointcloud_data_update(Depsgraph *depsgraph, Scene *scene, Object *object)
{
  /* Free any evaluated data and restore original data. */
  BKE_object_free_derived_caches(object);

  /* Evaluate modifiers. */
  PointCloud *pointcloud = static_cast<PointCloud *>(object->data);
  blender::bke::GeometrySet geometry_set = blender::bke::GeometrySet::from_pointcloud(
      pointcloud, blender::bke::GeometryOwnershipType::ReadOnly);
  pointcloud_evaluate_modifiers(depsgraph, scene, object, geometry_set);

  PointCloud *pointcloud_eval = take_pointcloud_ownership_from_geometry_set(geometry_set);

  /* If the geometry set did not contain a point cloud, we still create an empty one. */
  if (pointcloud_eval == nullptr) {
    pointcloud_eval = BKE_pointcloud_new_nomain(0);
  }

  /* Assign evaluated object. */
  const bool eval_is_owned = pointcloud_eval != pointcloud;
  BKE_object_eval_assign_data(object, &pointcloud_eval->id, eval_is_owned);
  object->runtime->geometry_set_eval = new blender::bke::GeometrySet(std::move(geometry_set));
}

void PointCloud::tag_positions_changed()
{
  this->runtime->bounds_cache.tag_dirty();
  this->runtime->bounds_with_radius_cache.tag_dirty();
  this->runtime->bvh_cache.tag_dirty();
}

void PointCloud::tag_radii_changed()
{
  this->runtime->bounds_with_radius_cache.tag_dirty();
}

/* Draw Cache */

void (*BKE_pointcloud_batch_cache_dirty_tag_cb)(PointCloud *pointcloud, int mode) = nullptr;
void (*BKE_pointcloud_batch_cache_free_cb)(PointCloud *pointcloud) = nullptr;

void BKE_pointcloud_batch_cache_dirty_tag(PointCloud *pointcloud, int mode)
{
  if (pointcloud->batch_cache) {
    BKE_pointcloud_batch_cache_dirty_tag_cb(pointcloud, mode);
  }
}

void BKE_pointcloud_batch_cache_free(PointCloud *pointcloud)
{
  if (pointcloud->batch_cache) {
    BKE_pointcloud_batch_cache_free_cb(pointcloud);
  }
}

namespace blender::bke {

PointCloud *pointcloud_new_no_attributes(int totpoint)
{
  PointCloud *pointcloud = BKE_pointcloud_new_nomain(0);
  pointcloud->totpoint = totpoint;
  CustomData_free_layer_named(&pointcloud->pdata, "position");
  return pointcloud;
}

}  // namespace blender::bke
