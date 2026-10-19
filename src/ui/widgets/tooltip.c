#include "advanced_internal.h"
#include <math.h>
void de_ui__tooltip_prepare(de_ui *ui,de_entity e,f32 dt) {
 de_ui_advanced *a=ui->advanced;if(!de_entity_has(ui->world,e,a->tooltip_state))de_entity_add(ui->world,e,a->tooltip_state);
 const DeUiTooltip *tip=de_entity_get_const(ui->world,e,a->tooltip);DeUiTooltip value=*tip;
 const DeUiInteractionState *interaction=de_entity_get_const(ui->world,e,ui->ids.state);
 bool shown=interaction&&(interaction->hovered||interaction->focused)&&!interaction->pressed&&de_ui__widget_enabled(ui,e);
 de_ui_tooltip_state *s=de_entity_get(ui->world,e,a->tooltip_state);
 if(!shown){s->elapsed=0;de_entity popup=s->popup;s->popup=(de_entity){0};if(de_entity_alive(ui->world,popup))de_entity_despawn_recursive(ui->world,popup);return;}
 s->elapsed+=dt;if(s->elapsed<fmaxf(0,value.delay)||de_entity_alive(ui->world,s->popup))return;
 const DeUiText *source=de_entity_get_const(ui->world,e,ui->ids.text);DeUiText text=source?*source:(DeUiText){0};
 de_entity popup=de_ui__advanced_node(ui,(de_entity){0},240,40);if(!popup.bits)return;
 DeUiNode *n=de_entity_get(ui->world,popup,ui->ids.node);n->z_order=2000;n->safe_area=true;
 DeUiText *label=de_entity_add(ui->world,popup,ui->ids.text);*label=text;label->loc_id=value.text;label->literal=(de_str){0};label->text[0]=0;
 DeUiPopup *place=de_entity_add(ui->world,popup,ui->ids.popup);place->anchor=e;place->placement=DE_UI_POPUP_BOTTOM;
 DeUiStyle *style=de_entity_add(ui->world,popup,ui->ids.style);style->background=(de_vec4){.025f,.035f,.065f,1};style->border_width=1;style->border_color=(de_vec4){.4f,.5f,.7f,1};style->corner_radius=4;
 de_ui__advanced_tag(ui,popup,DE_UI_WIDGET_TOOLTIP);s=de_entity_get(ui->world,e,a->tooltip_state);s->popup=popup;
}
