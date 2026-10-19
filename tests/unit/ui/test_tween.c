#include "style_test_util.h"
TEST_F(ui_tween, linear_replacement_reflection_path_and_invalid_types, ui_fx) {
    de_entity e = ui_node(f, (de_entity){0});
    DeUiStyle *s = de_entity_add(f->world, e, f->ui->ids.style);
    s->opacity = 1;
    de_ui_tween(f->ui, e, "opacity", 0, .5f, DE_UI_EASE_LINEAR);
    style_step(f, .25f);
    EXPECT_FLOAT_EQ(style_get(f, e).opacity, .5f, .00001f);
    de_ui_tween(f->ui, e, "opacity", 1, .5f, DE_UI_EASE_LINEAR);
    style_step(f, .25f);
    EXPECT_FLOAT_EQ(style_get(f, e).opacity, .75f, .00001f);
    style_step(f, .25f);
    EXPECT_FLOAT_EQ(style_get(f, e).opacity, 1, .00001f);
    ASSERT_EQ(de_ui__tween_path(f->ui, e, "DeUiNode.width.value", 100, .5f, DE_UI_EASE_LINEAR),
              DE_OK);
    style_step(f, .25f);
    const DeUiNode *n = de_entity_get_const(f->world, e, f->ui->ids.node);
    EXPECT_FLOAT_EQ(n->width.value, 50, .00001f);
    EXPECT_EQ(de_ui__tween_path(f->ui, e, "DeUiNode.clip", 1, 1, DE_UI_EASE_LINEAR),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_ui__tween_path(f->ui, e, "DeUiStyle.missing", 1, 1, DE_UI_EASE_LINEAR),
              DE_ERR_NOT_FOUND);
}
TEST_F(ui_tween, state_color_transition_reaches_target_and_easing, ui_fx) {
    ASSERT_EQ(style_install(f->ui, "[style.button]\nbackground='#000'\ntransition=0.12\neasing='"
                                   "quad-in'\n[style.\"button:hover\"]\nbackground='#fff'"),
              DE_OK);
    de_entity e = style_button(f, (de_entity){0}, NULL);
    style_step(f, 0);
    DeUiInteractionState *s = de_entity_add(f->world, e, f->ui->ids.state);
    s->hovered = true;
    style_step(f, .06f);
    EXPECT_FLOAT_EQ(style_get(f, e).background.x, .25f, .00001f);
    style_step(f, .06f);
    EXPECT_FLOAT_EQ(style_get(f, e).background.x, 1, .00001f);
    EXPECT_EQ(de_ui_get_style_stats(f->ui).active_tweens, 0u);
    for (u32 easing = 0; easing <= DE_UI_EASE_ELASTIC_OUT; ++easing) {
        EXPECT_FLOAT_EQ(de_ui__ease((de_ui_easing)easing, 0), 0, .00001f);
        EXPECT_FLOAT_EQ(de_ui__ease((de_ui_easing)easing, 1), 1, .00001f);
        EXPECT_TRUE(isfinite(de_ui__ease((de_ui_easing)easing, .37f)));
    }
    EXPECT_FLOAT_EQ(de_ui__ease(DE_UI_EASE_CUBIC_IN, .5f), .125f, .00001f);
}
TEST_F(ui_tween, interrupted_state_transition_restores_authored_unset_property, ui_fx) {
    ASSERT_EQ(style_install(f->ui,
                            "[style.button]\ntransition=0.12\n[style.\"button:hover\"]\nopacity=0"),
              DE_OK);
    de_entity e = style_button(f, (de_entity){0}, NULL);
    style_step(f, 0);
    DeUiInteractionState *s = de_entity_add(f->world, e, f->ui->ids.state);
    s->hovered = true;
    style_step(f, .06f);
    EXPECT_FLOAT_EQ(style_get(f, e).opacity, .5f, .00001f);
    s = de_entity_get(f->world, e, f->ui->ids.state);
    s->hovered = false;
    style_step(f, .06f);
    EXPECT_FLOAT_EQ(style_get(f, e).opacity, .75f, .00001f);
    style_step(f, .06f);
    EXPECT_FLOAT_EQ(style_get(f, e).opacity, 1, .00001f);
}
