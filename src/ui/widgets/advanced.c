#include "advanced_internal.h"
#include "ui/style/stylesheet_internal.h"
#include <math.h>
de_component de_ui__advanced_component(de_ui *ui,const char *name,u32 size,u32 align,
 const de_field_info *fields,u32 count,bool authored) {
 de_type_registry *r=de_world_types(ui->world); de_type_id id=de_type_find(r,name);
 if(!id.id && de_type_register_ex(r,&(de_type_desc){.name=name,.size=size,.align=align,
 .version=1,.is_pod=true,.fields=fields,.field_count=count},&id,NULL)!=DE_OK) return (de_component){0};
 return de_component_register(ui->world,id,authored?DE_COMP_SERIALIZE|DE_COMP_SNAPSHOT:0);
}
#define F(T,m,k) DE_FIELD_EX(T,m,k,DE_FIELD_DEFAULT,{0})
#define A(T,m,k,n) DE_FIELD_EX(T,m,DE_FK_FIXED_ARRAY,DE_FIELD_DEFAULT,{0,0,NULL,NULL,k,n})
#define REG(field,T,fields,auth) a->field=de_ui__advanced_component(ui,#T,sizeof(T),_Alignof(T),fields,sizeof(fields)/sizeof((fields)[0]),auth)
#define RUN(field,T) a->field=de_ui__advanced_component(ui,#T,sizeof(T),_Alignof(T),NULL,0,false)
static void reflect(de_ui *ui) {
 de_ui_advanced *a=ui->advanced;
 static const de_field_info input[]={A(DeUiTextInput,buffer,"u8",256),F(DeUiTextInput,length,DE_FK_U32),F(DeUiTextInput,caret,DE_FK_U32),F(DeUiTextInput,password,DE_FK_BOOL),F(DeUiTextInput,multiline,DE_FK_BOOL)};
 static const de_field_info drop[]={F(DeUiDropdown,selected,DE_FK_U32),F(DeUiDropdown,option_count,DE_FK_U32),A(DeUiDropdown,options,"name",32)};
 static const de_field_info scroll[]={F(DeUiScroll,offset,DE_FK_VEC2),F(DeUiScroll,horizontal,DE_FK_BOOL),F(DeUiScroll,vertical,DE_FK_BOOL)};
 static const de_field_info list[]={F(DeUiList,item_count,DE_FK_U32),F(DeUiList,item_height,DE_FK_F32),F(DeUiList,item_template,DE_FK_ASSET)};
 static const de_field_info tabs[]={F(DeUiTabs,active,DE_FK_U32)};
 static const de_field_info tip[]={F(DeUiTooltip,text,DE_FK_NAME),F(DeUiTooltip,delay,DE_FK_F32)};
 static const de_field_info focus[]={F(DeUiFocusable,tab_index,DE_FK_I32),F(DeUiFocusable,up,DE_FK_ENTITY),F(DeUiFocusable,down,DE_FK_ENTITY),F(DeUiFocusable,left,DE_FK_ENTITY),F(DeUiFocusable,right,DE_FK_ENTITY)};
 ui->widgets->focusable=de_ui__advanced_component(ui,"DeUiFocusable",sizeof(DeUiFocusable),_Alignof(DeUiFocusable),focus,5,true);
 REG(input,DeUiTextInput,input,true); REG(dropdown,DeUiDropdown,drop,true); REG(scroll,DeUiScroll,scroll,true);
 REG(list,DeUiList,list,true); REG(tabs,DeUiTabs,tabs,true); REG(tooltip,DeUiTooltip,tip,true);
 RUN(editor,de_ui_editor); RUN(popup,de_ui_popup_state); RUN(scroll_state,de_ui_scroll_state);
 RUN(list_state,de_ui_list_state); RUN(item,de_ui_item_state);
 a->header=de_ui__advanced_component(ui,"de_ui_header_state",sizeof(de_ui_item_state),_Alignof(de_ui_item_state),NULL,0,false);
 RUN(tab_state,de_ui_tab_state); RUN(tooltip_state,de_ui_tooltip_state);
}
#undef F
#undef A
#undef REG
#undef RUN
void de_ui__advanced_tag(de_ui *ui,de_entity e,de_ui_widget_type type) {
 const DeUiWidgetType *old=de_entity_get_const(ui->world,e,ui->style->widget);
 if(!old || old->type!=type) {DeUiWidgetType *tag=de_entity_add(ui->world,e,ui->style->widget);if(tag)tag->type=type;}
}
de_entity de_ui__advanced_node(de_ui *ui,de_entity parent,f32 w,f32 h) {
 de_entity e=de_entity_spawn(ui->world);if(!e.bits)return e;
 DeUiNode *n=de_entity_add(ui->world,e,ui->ids.node);
 if(!n){de_entity_despawn(ui->world,e);return (de_entity){0};}
 n->width=(de_ui_len){DE_UI_PX,w};n->height=(de_ui_len){DE_UI_PX,h};n->shrink=0;
 if(parent.bits)de_entity_set_parent(ui->world,e,parent);
 return e;
}
static void collect(const de_query_chunk *c,void *user) {
 de_ui_advanced *a=user;for(u32 i=0;i<c->count&&a->count<a->ui->capacity;++i)a->entities[a->count++]=c->entities[i];
}
static void prepare(de_system_ctx *ctx) {
 de_ui_advanced *a=ctx->user;de_ui *ui=a->ui;a->count=0;
 f32 dt=isfinite(ctx->dt)&&ctx->dt>0?ctx->dt:0;a->time+=dt;
 de_query_each(ui->world,ctx->queries[0],collect,a);
 for(u32 i=0;i<a->count;++i){de_entity e=a->entities[i];
 if(de_entity_has(ui->world,e,a->input))de_ui__input_prepare(ui,e);
 if(de_entity_has(ui->world,e,a->dropdown))de_ui__dropdown_prepare(ui,e);
 if(de_entity_has(ui->world,e,a->scroll))de_ui__scroll_prepare(ui,e,dt);
 if(de_entity_has(ui->world,e,a->list))de_ui__list_prepare(ui,e);
 if(de_entity_has(ui->world,e,a->tabs))de_ui__tabs_prepare(ui,e);
 if(de_entity_has(ui->world,e,a->tooltip))de_ui__tooltip_prepare(ui,e,dt);
 }
 const de_ui_event *event;while((event=de_event_next(ui->world,&a->events))){de_ui_event copy=*event;de_ui__dropdown_event(ui,&copy);de_ui__tabs_event(ui,&copy);}
 /* Structural reconciliation may move editor components; refresh borrowed display views last. */
 for(u32 i=0;i<a->count;++i)if(de_entity_alive(ui->world,a->entities[i])&&de_entity_has(ui->world,a->entities[i],a->editor))de_ui__input_refresh(ui,a->entities[i]);
}
static void input_frame(de_system_ctx *ctx) {
 de_ui_advanced *a=ctx->user;de_ui *ui=a->ui;a->count=0;
 de_query_each(ui->world,ctx->queries[0],collect,a);
 for(u32 i=0;i<a->count;++i){de_entity e=a->entities[i];
 if(de_entity_has(ui->world,e,a->input))de_ui__input_frame(ui,e,ctx->dt);
 if(de_entity_has(ui->world,e,a->scroll))de_ui__scroll_frame(ui,e,ctx->dt);
 }
 if(ui->desc.input && !a->ui->widgets->table.request_focus && de_key_pressed(ui->desc.input,DE_KEY_ESCAPE))
 for(u32 i=0;i<a->count;++i)if(de_entity_has(ui->world,a->entities[i],a->dropdown))de_ui__dropdown_cancel(ui,a->entities[i]);
}
static void draw(de_ui *ui,de_entity e,de_vec4 r,de_ui_draw_emitter *em,void *user) {
 de_ui_advanced *a=user;if(a->base_draw)a->base_draw(ui,e,r,em,a->base_user);
 if(de_entity_has(ui->world,e,a->input))de_ui__input_draw(ui,e,r,em);
 if(de_entity_has(ui->world,e,a->scroll))de_ui__scroll_draw(ui,e,r,em);
}
de_result de_ui__advanced_init(de_ui *ui) {
 de_ui_advanced *a=DE_NEW(ui->alloc,de_ui_advanced,DE_MEM_UI);if(!a)return DE_ERR_OUT_OF_MEMORY;
 memset(a,0,sizeof *a);ui->advanced=a;a->ui=ui;a->clipboard_set=de_platform_clipboard_set;a->clipboard_get=de_platform_clipboard_get;
 a->entities=DE_ALLOC(ui->alloc,(u64)ui->capacity*sizeof(de_entity),_Alignof(de_entity),DE_MEM_UI);
 return a->entities?DE_OK:DE_ERR_OUT_OF_MEMORY;
}
void de_ui__advanced_detach(de_ui *ui) {
 de_ui_advanced *a=ui->advanced;if(!a)return;
 if(ui->world){if(a->prepare.id)de_system_remove(ui->world,a->prepare);if(a->input_system.id)de_system_remove(ui->world,a->input_system);}
 a->prepare=a->input_system=(de_system){0};a->active_input=(de_entity){0};
 if(ui->desc.input)de_text_input_stop(ui->desc.input);
}
void de_ui__advanced_shutdown(de_ui *ui) {
 de_ui_advanced *a=ui->advanced;if(!a)return;de_ui__advanced_detach(ui);
 if(a->entities)DE_FREE(ui->alloc,a->entities,(u64)ui->capacity*sizeof(de_entity));DE_FREE(ui->alloc,a,sizeof *a);ui->advanced=NULL;
}
void de_ui__advanced_attach(de_ui *ui) {
 de_ui_advanced *a=ui->advanced;reflect(ui);a->events=de_event_reader_create(ui->world,ui->event);
 de_ui_widget_table *t=de_ui__widget_table(ui);
 t->entries[DE_UI_WIDGET_INPUT].submit=de_ui__input_submit;t->entries[DE_UI_WIDGET_INPUT].cancel=de_ui__input_cancel;
 t->entries[DE_UI_WIDGET_DROPDOWN].submit=de_ui__dropdown_submit;t->entries[DE_UI_WIDGET_DROPDOWN].cancel=de_ui__dropdown_cancel;
 t->entries[DE_UI_WIDGET_TABS].submit=de_ui__tabs_submit;t->entries[DE_UI_WIDGET_TABS].increment=de_ui__tabs_increment;t->entries[DE_UI_WIDGET_TABS].decrement=de_ui__tabs_decrement;
 t->scroll_into_view=de_ui__scroll_into_view;t->refresh_text=de_ui__input_refresh;
 de_query_term term={ui->ids.node,DE_READ};de_query_desc q={.terms=&term,.term_count=1};
 const char *before[]={"ui.widgets",DE_UI_STYLE_SYSTEM,DE_UI_LAYOUT_SYSTEM
#if defined(DE_UI_NAVIGATION)
 ,"ui.focus"
#endif
 };
 a->prepare=de_system_add(ui->world,&(de_system_desc){.name="ui.advanced",.schedule=DE_SCHEDULE_POST_UPDATE,.fn=prepare,.user=a,.flags=DE_SYSTEM_MAIN_THREAD|DE_SYSTEM_EXCLUSIVE,.queries=&q,.query_count=1,.before=before,.before_count=sizeof(before)/sizeof(before[0])});
 const char *after[]={"ui.pointer"};a->input_system=de_system_add(ui->world,&(de_system_desc){.name="ui.edit",.schedule=DE_SCHEDULE_PRE_UPDATE,.fn=input_frame,.user=a,.flags=DE_SYSTEM_MAIN_THREAD|DE_SYSTEM_EXCLUSIVE,.queries=&q,.query_count=1,.after=after,.after_count=1});
 struct de_ui_render *r=ui->render;if(r){a->base_draw=r->widget_draw;a->base_user=r->widget_user;de_ui__set_widget_draw(ui,draw,a);}
 de_ui__set_text_geometry(ui,de_ui__input_geometry,a);
 ui->content_geometry=de_ui__scroll_content;
}
