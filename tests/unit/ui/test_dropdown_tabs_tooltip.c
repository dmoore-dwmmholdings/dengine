#include "ui_test_util.h"
#include "ui/widgets/advanced_internal.h"
TEST_F(ui_advanced, dropdown_flips_selects_once_and_cancel_preserves_value,ui_fx){
 de_entity root=ui_node(f,(de_entity){0}),drop=ui_node(f,root);DeUiAnchor *anchor=de_entity_add(f->world,drop,f->ui->ids.anchor);anchor->offset_min=(de_vec2){10,70};anchor->offset_max=(de_vec2){150,95};
 DeUiDropdown *d=de_entity_add(f->world,drop,f->ui->advanced->dropdown);d->option_count=2;d->options[0]=de_name_from("one");d->options[1]=de_name_from("two");ui_frame(f);
 de_ui__dropdown_submit(f->ui,drop);ui_frame(f);
 const de_ui_popup_state *s=de_entity_get_const(f->world,drop,f->ui->advanced->popup);ASSERT_TRUE(s->open);
 const DeUiPopup *popup=de_entity_get_const(f->world,s->popup,f->ui->ids.popup);EXPECT_EQ(popup->placement,DE_UI_POPUP_TOP);EXPECT_TRUE(ui_rect(f,s->popup).y<ui_rect(f,drop).y);
 de_entity second=de_entity_children_next(f->world,de_entity_children_first(f->world,s->popup));
 de_event_reader reader=de_event_reader_create(f->world,f->ui->event);de_ui__button_submit(f->ui,second);ui_frame(f);
 d=de_entity_get(f->world,drop,f->ui->advanced->dropdown);EXPECT_EQ(d->selected,1u);s=de_entity_get_const(f->world,drop,f->ui->advanced->popup);EXPECT_FALSE(s->open);
 const de_ui_event *event;u32 changed=0;while((event=de_event_next(f->world,&reader)))if(event->kind==DE_UI_EV_CHANGED&&event->target.bits==drop.bits)++changed;EXPECT_EQ(changed,1u);
 de_ui__dropdown_submit(f->ui,drop);de_ui__dropdown_cancel(f->ui,drop);d=de_entity_get(f->world,drop,f->ui->advanced->dropdown);EXPECT_EQ(d->selected,1u);
}
TEST_F(ui_advanced, tabs_generate_headers_hide_inactive_pages_and_emit_change,ui_fx){
 de_entity root=ui_node(f,(de_entity){0}),tabs=ui_node(f,root),one=ui_node(f,tabs),two=ui_node(f,tabs);
 de_entity_add(f->world,tabs,f->ui->advanced->tabs);ui_node_edit(f,tabs)->direction=DE_UI_COLUMN;ui_node_edit(f,tabs)->width=(de_ui_len){DE_UI_PX,200};ui_node_edit(f,tabs)->height=(de_ui_len){DE_UI_PX,90};
 ui_frame(f);const de_ui_tab_state *state=de_entity_get_const(f->world,tabs,f->ui->advanced->tab_state);ASSERT_TRUE(state);EXPECT_EQ(state->count,2u);EXPECT_EQ(de_entity_children_count(f->world,state->headers),2u);
 EXPECT_TRUE(ui_rect(f,one).w>0);EXPECT_FLOAT_EQ(ui_rect(f,two).w,0,0);
 de_event_reader reader=de_event_reader_create(f->world,f->ui->event);de_ui__tabs_increment(f->ui,tabs);ui_frame(f);
 EXPECT_FLOAT_EQ(ui_rect(f,one).w,0,0);EXPECT_TRUE(ui_rect(f,two).w>0);
 const de_ui_event *event;u32 changed=0;while((event=de_event_next(f->world,&reader)))if(event->kind==DE_UI_EV_CHANGED&&event->target.bits==tabs.bits)++changed;EXPECT_EQ(changed,1u);
}
TEST_F(ui_advanced, tooltip_delay_focus_safe_bounds_and_press_hide,ui_fx){
 de_ui_set_viewport(f->ui,400,100,(de_window_insets){.left=10,.right=10,.top=5,.bottom=5});
 de_entity root=ui_node(f,(de_entity){0}),e=ui_node(f,root);DeUiAnchor *anchor=de_entity_add(f->world,e,f->ui->ids.anchor);anchor->offset_min=(de_vec2){350,70};anchor->offset_max=(de_vec2){390,90};
 DeUiTooltip *t=de_entity_add(f->world,e,f->ui->advanced->tooltip);t->delay=.2f;t->text=de_name_from("tip");
 DeUiInteractionState *interaction=de_entity_add(f->world,e,f->ui->ids.state);interaction->focused=true;
 ASSERT_EQ(de_world_run_schedule(f->world,DE_SCHEDULE_POST_UPDATE,.1f,0),DE_OK);
 const de_ui_tooltip_state *s=de_entity_get_const(f->world,e,f->ui->advanced->tooltip_state);ASSERT_TRUE(s);EXPECT_EQ(s->popup.bits,0u);
 ASSERT_EQ(de_world_run_schedule(f->world,DE_SCHEDULE_POST_UPDATE,.11f,0),DE_OK);s=de_entity_get_const(f->world,e,f->ui->advanced->tooltip_state);ASSERT_TRUE(s->popup.bits);
 de_vec4 rect=ui_rect(f,s->popup);EXPECT_TRUE(rect.x>=10&&rect.x+rect.z<=390);EXPECT_TRUE(rect.y>=5&&rect.y+rect.w<=95);
 interaction=de_entity_get(f->world,e,f->ui->ids.state);interaction->pressed=true;ui_frame(f);s=de_entity_get_const(f->world,e,f->ui->advanced->tooltip_state);EXPECT_EQ(s->popup.bits,0u);
}
