#include "advanced_internal.h"
static void activate(de_ui *ui,de_entity e,u32 active) {
 de_ui_advanced *a=ui->advanced;DeUiTabs *tabs=de_entity_get(ui->world,e,a->tabs);const de_ui_tab_state *state=de_entity_get_const(ui->world,e,a->tab_state);
 if(!tabs||!state||!state->count)return;active=active>=state->count?state->count-1:active;
 if(tabs->active!=active){tabs->active=active;de_ui__widget_event(ui,DE_UI_EV_CHANGED,e,(de_name){0},(f32)active);}
}
void de_ui__tabs_increment(de_ui *ui,de_entity e){const DeUiTabs *t=de_entity_get_const(ui->world,e,ui->advanced->tabs);if(t)activate(ui,e,t->active+1);}
void de_ui__tabs_decrement(de_ui *ui,de_entity e){const DeUiTabs *t=de_entity_get_const(ui->world,e,ui->advanced->tabs);if(t)activate(ui,e,t->active?t->active-1:0);}
void de_ui__tabs_submit(de_ui *ui,de_entity e){de_ui__tabs_increment(ui,e);}
void de_ui__tabs_prepare(de_ui *ui,de_entity e) {
 de_ui_advanced *a=ui->advanced;de_ui__advanced_tag(ui,e,DE_UI_WIDGET_TABS);
 if(!de_entity_has(ui->world,e,ui->widgets->focusable))de_entity_add(ui->world,e,ui->widgets->focusable);
 if(!de_entity_has(ui->world,e,a->tab_state))de_entity_add(ui->world,e,a->tab_state);
 de_ui_tab_state *state=de_entity_get(ui->world,e,a->tab_state);de_entity headers=state->headers;
 if(!de_entity_alive(ui->world,headers)){
 headers=de_ui__advanced_node(ui,e,0,32);DeUiNode *n=de_entity_get(ui->world,headers,ui->ids.node);n->direction=DE_UI_ROW;n->width=(de_ui_len){DE_UI_PERCENT,100};
 de_ui_item_state *marker=de_entity_add(ui->world,headers,a->header);marker->owner=e;
 DeUiAnchor *anchor=de_entity_add(ui->world,headers,ui->ids.anchor);*anchor=(DeUiAnchor){.anchor_max={1,0},.offset_max={0,32}};
 state=de_entity_get(ui->world,e,a->tab_state);state->headers=headers;
 }
 u32 count=0;for(de_entity c=de_entity_children_first(ui->world,e);c.bits;c=de_entity_children_next(ui->world,c))if(c.bits!=headers.bits)++count;
 DeUiTabs *tabs=de_entity_get(ui->world,e,a->tabs);if(tabs->active>=count)tabs->active=count?count-1:0;u32 active=tabs->active;
 state=de_entity_get(ui->world,e,a->tab_state);
 if(state->count!=count){
 for(de_entity c=de_entity_children_first(ui->world,headers);c.bits;){de_entity next=de_entity_children_next(ui->world,c);de_entity_despawn_recursive(ui->world,c);c=next;}
 u32 i=0;for(de_entity page=de_entity_children_first(ui->world,e);page.bits;page=de_entity_children_next(ui->world,page)){
 if(page.bits==headers.bits)continue;DeUiText label={0};const DeUiText *src=de_entity_get_const(ui->world,page,ui->ids.text);if(src)label=*src;
 de_entity button=de_ui__advanced_node(ui,headers,100,32);de_entity_add(ui->world,button,ui->widgets->button);
 DeUiText *text=de_entity_add(ui->world,button,ui->ids.text);*text=label;
 de_ui_item_state *item=de_entity_add(ui->world,button,a->header);*item=(de_ui_item_state){.owner=e,.index=i++};}
 }
 u32 i=0;for(de_entity c=de_entity_children_first(ui->world,e);c.bits;c=de_entity_children_next(ui->world,c)){
 if(c.bits==headers.bits)continue;
 if(!de_entity_has(ui->world,c,ui->ids.anchor))de_entity_add(ui->world,c,ui->ids.anchor);
 DeUiAnchor *anchor=de_entity_get(ui->world,c,ui->ids.anchor);*anchor=(DeUiAnchor){.anchor_max={1,1},.offset_min={0,32}};
 DeUiNode *n=de_entity_get(ui->world,c,ui->ids.node);if(n)n->display=i==active?DE_UI_FLEX:DE_UI_NONE;++i;}
 state=de_entity_get(ui->world,e,a->tab_state);state->count=count;state->active=active;
}
void de_ui__tabs_event(de_ui *ui,const de_ui_event *event) {
 if(event->kind!=DE_UI_EV_CLICK)return;const de_ui_item_state *item=de_entity_get_const(ui->world,event->target,ui->advanced->header);
 if(item)activate(ui,item->owner,item->index);
}
