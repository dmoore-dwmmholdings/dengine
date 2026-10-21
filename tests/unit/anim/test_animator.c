#include "anim_fixture.h"

static const de_field_info *field(const de_type_desc *type, const char *name) {
    for (u32 i = 0; i < type->field_count; ++i)
        if (strcmp(type->fields[i].name, name) == 0)
            return &type->fields[i];
    return NULL;
}

TEST_F(anim, registered_components_have_typed_fields_and_constructed_defaults, anim_fixture) {
    const de_type_desc *a = de_type_get(f->types, de_type_find(f->types, "DeAnimator"));
    ASSERT_TRUE(a);
    EXPECT_EQ(a->version, 2u);
    const de_field_info *clip = field(a, "clip"), *time = field(a, "time");
    ASSERT_TRUE(clip && time);
    EXPECT_EQ(clip->kind, DE_FK_ASSET);
    EXPECT_TRUE(strcmp(clip->attrs.nested_type, "anim_clip") == 0);
    EXPECT_TRUE((clip->flags & DE_FIELD_ASSET_REF) != 0);
    EXPECT_TRUE((time->flags & DE_FIELD_EDITOR_HIDDEN) != 0);
    EXPECT_TRUE(strcmp(time->attrs.units, "s") == 0);
    de_component components[] = {de_anim_component(f->anim), de_component_find(f->world, "DeIK"),
                                 de_component_find(f->world, "DeSpriteAnimator")};
    ASSERT_TRUE(components[1].id && components[2].id);
    load_assets(f, 2);
    de_entity entity = spawn_animator(f);
    ASSERT_TRUE(de_entity_add(f->world, entity, components[1]));
    ASSERT_TRUE(de_entity_add(f->world, entity, components[2]));
    anim_frame(f, 0);
    const DeAnimator *animator = de_entity_get_const(f->world, entity, components[0]);
    const DeIK *ik = de_entity_get_const(f->world, entity, components[1]);
    const DeSpriteAnimator *sprite = de_entity_get_const(f->world, entity, components[2]);
    EXPECT_TRUE(animator->speed == 1 && animator->playing && animator->loop);
    EXPECT_TRUE(sprite->speed == 1 && sprite->playing && sprite->loop && !sprite->ping_pong);
    EXPECT_EQ(ik->chain_count, 0u);
    for (u32 i = 0; i < 8; ++i)
        EXPECT_TRUE(ik->chains[i].weight == 1 && ik->chains[i].iterations == 10);
    const de_type_desc *chain = de_type_get(f->types, de_type_find(f->types, "de_ik_chain"));
    ASSERT_TRUE(chain);
    const de_field_info *weight = field(chain, "weight");
    ASSERT_TRUE(weight);
    EXPECT_TRUE(weight->attrs.min == 0 && weight->attrs.max == 1);
    const de_field_info *target = field(chain, "target_position");
    ASSERT_TRUE(target);
    EXPECT_EQ(target->kind, DE_FK_DVEC3);
    ASSERT_EQ(de__anim_register_components(f->anim, f->world), DE_OK);
    EXPECT_EQ(de_anim_component(f->anim).id, components[0].id);
    EXPECT_EQ(de_component_find(f->world, "DeIK").id, components[1].id);
}

typedef struct asset_mapping {
    bool bad_type;
    u32 encoded, decoded;
} asset_mapping;
static u32 asset_kind(const char *type) {
    if (type && strcmp(type, "skeleton") == 0)
        return 1;
    if (type && strcmp(type, "anim_graph") == 0)
        return 2;
    if (type && strcmp(type, "anim_clip") == 0)
        return 3;
    return 0;
}
static u64 encode_asset(u32 handle, const char *type, void *user) {
    asset_mapping *m = user;
    u32 kind = asset_kind(type);
    m->bad_type |= kind == 0;
    ++m->encoded;
    return ((u64)kind << 48) | handle;
}
static u32 decode_asset(u64 id, const char *type, void *user) {
    asset_mapping *m = user;
    m->bad_type |= (id >> 48) != asset_kind(type);
    ++m->decoded;
    return (u32)(id & 0xffff) + 1000;
}
TEST_F(anim, serialized_animator_assets_resolve_to_new_runtime_handles, anim_fixture) {
    const de_type_id id = de_type_find(f->types, "DeAnimator");
    const DeAnimator input = {.skeleton = {11},
                              .graph = {22},
                              .clip = {33},
                              .speed = -2.5f,
                              .time = .125f,
                              .playing = true,
                              .loop = true};
    for (u32 format = 0; format < 2; ++format) {
        u8 data[4096];
        de_mem_writer mw;
        de_mem_writer_init_fixed(&mw, data, sizeof data);
        de_writer wr = de_writer_from_mem(&mw);
        asset_mapping mapping = {0};
        de_ser_context ctx = {
            .asset_to_id = encode_asset, .id_to_asset = decode_asset, .user = &mapping};
        de_ser_options options = {.format = format == 0 ? DE_SER_TEXT : DE_SER_BINARY,
                                  .required_flags = DE_FIELD_SERIALIZE,
                                  .ctx = &ctx};
        ASSERT_EQ(de_serialize_object(f->types, id, &input, &options, &wr), DE_OK);
        DeAnimator output;
        ASSERT_EQ(de_type_construct(f->types, id, &output), DE_OK);
        de_mem_reader mr;
        de_mem_reader_init(&mr, data, mw.size);
        de_reader rd = de_reader_from_mem(&mr);
        ASSERT_EQ(de_deserialize_object(f->types, id, &output, &options, &rd), DE_OK);
        EXPECT_FALSE(mapping.bad_type);
        EXPECT_TRUE(mapping.encoded >= 3 && mapping.decoded >= 3);
        EXPECT_EQ(output.skeleton.id, 1011u);
        EXPECT_EQ(output.graph.id, 1022u);
        EXPECT_EQ(output.clip.id, 1033u);
        EXPECT_TRUE(output.speed == input.speed && output.time == input.time && output.loop);
    }
}

TEST_F(anim, ik_nested_chains_and_sprite_fields_roundtrip, anim_fixture) {
    de_type_id ik_type = de_type_find(f->types, "DeIK");
    DeIK ik;
    ASSERT_EQ(de_type_construct(f->types, ik_type, &ik), DE_OK);
    ik.chain_count = 2;
    ik.chains[0].kind = DE_IK_FABRIK;
    ik.chains[0].target_position = (de_dvec3){1000000000.25, 3, 4};
    ik.chains[0].root_bone = de_name_from("hip");
    ik.chains[0].tip_bone = de_name_from("foot");
    ik.chains[1].kind = DE_IK_LOOK_AT;
    ik.chains[1].weight = .25f;
    u8 data[16384];
    de_mem_writer mw;
    de_mem_writer_init_fixed(&mw, data, sizeof data);
    de_writer wr = de_writer_from_mem(&mw);
    de_ser_options options = {.format = DE_SER_TEXT, .required_flags = DE_FIELD_SERIALIZE};
    ASSERT_EQ(de_serialize_object(f->types, ik_type, &ik, &options, &wr), DE_OK);
    DeIK output;
    ASSERT_EQ(de_type_construct(f->types, ik_type, &output), DE_OK);
    de_mem_reader mr;
    de_mem_reader_init(&mr, data, mw.size);
    de_reader rd = de_reader_from_mem(&mr);
    ASSERT_EQ(de_deserialize_object(f->types, ik_type, &output, &options, &rd), DE_OK);
    EXPECT_EQ(output.chain_count, 2u);
    EXPECT_EQ(output.chains[0].kind, DE_IK_FABRIK);
    EXPECT_TRUE(output.chains[0].target_position.x == 1000000000.25);
    EXPECT_EQ(output.chains[0].root_bone.hash, ik.chains[0].root_bone.hash);
    EXPECT_TRUE(output.chains[1].weight == .25f);
    de_type_id sprite_type = de_type_find(f->types, "DeSpriteAnimator");
    DeSpriteAnimator sprite;
    ASSERT_EQ(de_type_construct(f->types, sprite_type, &sprite), DE_OK);
    sprite.clip.id = 99;
    sprite.tag = de_name_from("walk");
    sprite.ping_pong = true;
    de_mem_writer_reset(&mw);
    ASSERT_EQ(de_serialize_object(f->types, sprite_type, &sprite, &options, &wr), DE_OK);
    DeSpriteAnimator result;
    ASSERT_EQ(de_type_construct(f->types, sprite_type, &result), DE_OK);
    de_mem_reader_init(&mr, data, mw.size);
    rd = de_reader_from_mem(&mr);
    ASSERT_EQ(de_deserialize_object(f->types, sprite_type, &result, &options, &rd), DE_OK);
    EXPECT_EQ(result.clip.id, 99u);
    EXPECT_EQ(result.tag.hash, sprite.tag.hash);
    EXPECT_TRUE(result.ping_pong && result.playing && result.speed == 1);
}

TEST_F(anim, version_one_raw_handles_remain_readable, anim_fixture) {
    de_type_registry *old;
    ASSERT_EQ(de_type_registry_create(&f->allocator, &old), DE_OK);
    static const de_field_info fields[] = {
        DE_FIELD(DeAnimator, skeleton, DE_FK_U32, DE_FIELD_DEFAULT),
        DE_FIELD(DeAnimator, graph, DE_FK_U32, DE_FIELD_DEFAULT),
        DE_FIELD(DeAnimator, clip, DE_FK_U32, DE_FIELD_DEFAULT),
        DE_FIELD(DeAnimator, speed, DE_FK_F32, DE_FIELD_DEFAULT),
        DE_FIELD(DeAnimator, time, DE_FK_F32, DE_FIELD_DEFAULT),
        DE_FIELD(DeAnimator, playing, DE_FK_BOOL, DE_FIELD_DEFAULT),
        DE_FIELD(DeAnimator, loop, DE_FK_BOOL, DE_FIELD_DEFAULT),
        DE_FIELD(DeAnimator, root_motion, DE_FK_BOOL, DE_FIELD_DEFAULT),
        DE_FIELD(DeAnimator, deterministic, DE_FK_BOOL, DE_FIELD_DEFAULT)};
    de_type_desc old_desc = DE_TYPE_DESC(DeAnimator, 1, fields);
    de_type_id old_id = de_type_register(old, &old_desc);
    ASSERT_TRUE(old_id.id);
    DeAnimator input = {
        .skeleton = {1048577}, .graph = {1048578}, .clip = {1048579}, .speed = -.5f, .time = .25f};
    for (u32 format = 0; format < 2; ++format) {
        u8 data[4096];
        de_mem_writer mw;
        de_mem_writer_init_fixed(&mw, data, sizeof data);
        de_writer wr = de_writer_from_mem(&mw);
        de_ser_options options = {.format = format == 0 ? DE_SER_TEXT : DE_SER_BINARY};
        /* Migration needs stored field metadata; Shipping's default binary is compact. */
        if (format) {
            de_ser_binary_options binary = {.schema = DE_SER_BINARY_SELF_DESCRIBING};
            ASSERT_EQ(de_serialize_binary(old, old_id, &input, &options, &binary, &wr), DE_OK);
        } else
            ASSERT_EQ(de_serialize_object(old, old_id, &input, &options, &wr), DE_OK);
        de_type_id id = de_type_find(f->types, "DeAnimator");
        DeAnimator output;
        ASSERT_EQ(de_type_construct(f->types, id, &output), DE_OK);
        de_mem_reader mr;
        de_mem_reader_init(&mr, data, mw.size);
        de_reader rd = de_reader_from_mem(&mr);
        ASSERT_EQ(de_deserialize_object(f->types, id, &output, &options, &rd), DE_OK);
        EXPECT_EQ(output.skeleton.id, input.skeleton.id);
        EXPECT_EQ(output.graph.id, input.graph.id);
        EXPECT_EQ(output.clip.id, input.clip.id);
        EXPECT_TRUE(output.speed == input.speed && output.time == input.time);
        EXPECT_EQ(output.playing, input.playing);
        EXPECT_EQ(output.loop, input.loop);
        EXPECT_EQ(output.root_motion, input.root_motion);
        EXPECT_EQ(output.deterministic, input.deterministic);
        if (format) {
            /* Cooked compact records intentionally reject a changed schema. */
            de_mem_writer_reset(&mw);
            de_ser_binary_options compact = {.schema = DE_SER_BINARY_COMPACT};
            ASSERT_EQ(de_serialize_binary(old, old_id, &input, &options, &compact, &wr), DE_OK);
            de_mem_reader_init(&mr, data, mw.size);
            rd = de_reader_from_mem(&mr);
            EXPECT_EQ(de_deserialize_object(f->types, id, &output, &options, &rd),
                      DE_ERR_VERSION_MISMATCH);
        }
    }
    de_type_registry_destroy(old);
}

TEST_F(anim, legacy_handle_outside_u32_range_is_rejected, anim_fixture) {
    const char *records[] = {"__type=\"DeAnimator\"\n__version=1\nskeleton=4294967296\n",
                             "__type=\"DeAnimator\"\n__version=1\nclip=-1\n"};
    for (u32 i = 0; i < sizeof records / sizeof records[0]; ++i) {
        de_type_id id = de_type_find(f->types, "DeAnimator");
        DeAnimator output;
        ASSERT_EQ(de_type_construct(f->types, id, &output), DE_OK);
        de_mem_reader mr;
        de_mem_reader_init(&mr, records[i], strlen(records[i]));
        de_reader rd = de_reader_from_mem(&mr);
        de_ser_options options = {.format = DE_SER_TEXT};
        EXPECT_EQ(de_deserialize_object(f->types, id, &output, &options, &rd),
                  DE_ERR_VERSION_MISMATCH);
    }
}

TEST_F(anim, failed_attachment_is_retryable_and_does_not_publish_partial_state, anim_fixture) {
    u32 failures = 0;
    bool completed = false;
    for (i64 allowance = 0; allowance < 160; ++allowance) {
        de_world *world;
        ASSERT_EQ(
            de_world_create(
                &(de_world_desc){.allocator = &f->allocator, .types = f->types, .tasks = f->tasks},
                &world),
            DE_OK);
        de_anim_system *anim;
        ASSERT_EQ(de_anim_create(&f->allocator, f->assets, &anim), DE_OK);
        de_tracking_allocator_fail_after(&f->tracking, allowance);
        de_anim_attach(anim, world);
        de_tracking_allocator_fail_after(&f->tracking, -1);
        bool attached = anim->world != NULL;
        if (!attached) {
            ++failures;
            EXPECT_EQ(de_anim_component(anim).id, 0u);
            EXPECT_EQ(anim->sample.id, 0u);
            EXPECT_EQ(anim->finalize.id, 0u);
            EXPECT_EQ(anim->resource.id, 0u);
            de_type_id resource = de_type_find(f->types, "DeAnimSystemRef");
            EXPECT_EQ(de_resource_find(world, resource).id, 0u);
            de_anim_attach(anim, world);
        }
        ASSERT_TRUE(anim->world == world);
        ASSERT_TRUE(anim->sample.id && anim->finalize.id && de_anim_component(anim).id);
        ASSERT_EQ(de_world_run_schedule(world, DE_SCHEDULE_POST_UPDATE, 0, 0), DE_OK);
        de_anim_destroy(anim);
        de_world_destroy(world);
        if (attached) {
            completed = true;
            break;
        }
    }
    EXPECT_TRUE(failures > 0 && completed);
    printf("animation attach allocator-failure sweep: %u failed/recovered boundaries\n", failures);
}

TEST_F(anim, second_animation_system_cannot_replace_world_owner, anim_fixture) {
    load_assets(f, 2);
    de_entity entity = spawn_animator(f);
    de_anim_system *other;
    ASSERT_EQ(de_anim_create(&f->allocator, f->assets, &other), DE_OK);
    de_anim_attach(other, f->world);
    EXPECT_FALSE(other->world);
    EXPECT_EQ(de_anim_component(other).id, 0u);
    de_anim_destroy(other);
    anim_frame(f, .1f);
    de_transform bone;
    ASSERT_TRUE(de_anim_bone_transform(f->world, entity, de_name_from("joint0"), &bone));
    EXPECT_TRUE(fabs(bone.position.x - .2) < 1e-5);
}
