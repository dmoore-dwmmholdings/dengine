#include "advanced_internal.h"
#include <math.h>
static u32 bounded(const char *s,u32 cap){u32 n=0;while(n<cap&&s[n])++n;return n;}
static u32 boundaries(const DeUiTextInput *in,u32 out[257]) {return de_text_grapheme_offsets((de_str){in->buffer,in->length},out,257);}
static u32 snap(const DeUiTextInput *in,u32 byte){u32 b[257],n=boundaries(in,b),last=0;for(u32 i=0;i<n&&b[i]<=byte;++i)last=b[i];return last;}
bool de_ui__input_replace(DeUiTextInput *in,de_ui_editor *state,de_str text) {
 u32 lo=in->caret<state->anchor?in->caret:state->anchor,hi=in->caret>state->anchor?in->caret:state->anchor;
 lo=snap(in,lo);hi=snap(in,hi);u32 room=255-(in->length-(hi-lo));
 u32 n=text.len<room?(u32)text.len:room;
 /* OS text is UTF8. Never truncate inside a multibyte sequence at the byte cap. */
 while(n<text.len&&n&&((u8)text.ptr[n]&0xc0)==0x80)--n;
 if(!n&&lo==hi)return false;
 memmove(in->buffer+lo+n,in->buffer+hi,in->length-hi);
 if(n)memcpy(in->buffer+lo,text.ptr,n);
 in->length=in->length-(hi-lo)+n;in->buffer[in->length]=0;in->caret=state->anchor=lo+n;state->blink=0;return true;
}
static bool space(const char *s,u32 byte){u8 c=(u8)s[byte];return c==' '||c=='\t'||c=='\n'||c=='\r';}
void de_ui__input_move(DeUiTextInput *in,de_ui_editor *state,i32 dir,bool extend,bool word) {
 u32 b[257],n=boundaries(in,b),i=0;while(i+1<n&&b[i]<in->caret)++i;
 if(!extend&&state->anchor!=in->caret&&!word)in->caret=dir<0?(in->caret<state->anchor?in->caret:state->anchor):(in->caret>state->anchor?in->caret:state->anchor);
 else if(dir<0){if(i)--i;if(word){while(i&&space(in->buffer,b[i]))--i;while(i&&!space(in->buffer,b[i-1]))--i;}in->caret=b[i];}
 else {if(i+1<n)++i;if(word){while(i+1<n&&!space(in->buffer,b[i]))++i;while(i+1<n&&space(in->buffer,b[i]))++i;}in->caret=b[i];}
 if(!extend)state->anchor=in->caret;state->blink=0;
}
static de_text_layout *layout(de_ui *ui,de_entity e,const char *s,u32 n,f32 width) {
 const DeUiText *t=de_entity_get_const(ui->world,e,ui->ids.text);if(!t||!ui->desc.text)return NULL;
 de_text_style style=t->style;style.size_px*=ui->scale;style.letter_spacing*=ui->scale;
 de_text_layout *l=NULL;de_text_layout_build(ui->desc.text,&(de_text_layout_desc){.utf8={s,n},.style=&style,.max_width=width},ui->alloc,&l);return l;
}
void de_ui__input_refresh(de_ui *ui,de_entity e) {
 de_ui_advanced *a=ui->advanced;DeUiTextInput *in=de_entity_get(ui->world,e,a->input);de_ui_editor *s=de_entity_get(ui->world,e,a->editor);
 if(!in||!s)return;de_text_input_state typed={0};
 if(a->active_input.bits==e.bits&&ui->desc.input)de_text_input_get(ui->desc.input,&typed);
 u32 used=0,b[257],n=boundaries(in,b);s->preedit_start=s->preedit_end=0;
 for(u32 i=0;i<n;++i){
 if(b[i]==in->caret){s->displayed_caret=used;s->preedit_start=used;
 if(typed.composition){u32 count=bounded(typed.composition,1023);memcpy(s->display+used,typed.composition,count);used+=count;}s->preedit_end=used;}
 if(i+1==n)break;
 if(in->password){memcpy(s->display+used,"\xe2\x80\xa2",3);used+=3;}else{u32 count=b[i+1]-b[i];memcpy(s->display+used,in->buffer+b[i],count);used+=count;}}
 s->display[used]=0;
 DeUiText *t=de_entity_get(ui->world,e,ui->ids.text);if(t)t->literal=(de_str){s->display,used};
 /* The ECS may move the editor storage. prepare refreshes this borrowed view every frame. */
}
void de_ui__input_prepare(de_ui *ui,de_entity e) {
 de_ui_advanced *a=ui->advanced;
 if(!de_entity_has(ui->world,e,a->editor))de_entity_add(ui->world,e,a->editor);
 if(!de_entity_has(ui->world,e,ui->ids.text))de_entity_add(ui->world,e,ui->ids.text);
 if(!de_entity_has(ui->world,e,ui->ids.state))de_entity_add(ui->world,e,ui->ids.state);
 de_ui__advanced_tag(ui,e,DE_UI_WIDGET_INPUT);
 if(!de_entity_has(ui->world,e,ui->widgets->focusable))de_entity_add(ui->world,e,ui->widgets->focusable);
 DeUiNode *node=de_entity_get(ui->world,e,ui->ids.node);node->clip=true;
 DeUiTextInput *in=de_entity_get(ui->world,e,a->input);de_ui_editor *s=de_entity_get(ui->world,e,a->editor);
 in->length=bounded(in->buffer,255);in->buffer[in->length]=0;in->caret=snap(in,in->caret>in->length?in->length:in->caret);
 if(!s->initialized){s->anchor=in->caret;s->initialized=true;}s->anchor=snap(in,s->anchor>in->length?in->length:s->anchor);
 de_ui__input_refresh(ui,e);
}
void de_ui__input_submit(de_ui *ui,de_entity e) {
 de_ui_advanced *a=ui->advanced;de_ui_widget_table *table=de_ui__widget_table(ui);
 if(table->request_focus)table->request_focus(ui,e);
 if(a->active_input.bits!=e.bits){
 if(a->active_input.bits){DeUiInteractionState *old=de_entity_get(ui->world,a->active_input,ui->ids.state);if(old)old->focused=false;}
 a->active_input=e;DeUiInteractionState *s=de_entity_add(ui->world,e,ui->ids.state);if(s)s->focused=true;
 if(ui->desc.input)de_text_input_start(ui->desc.input,NULL);
 }
}
void de_ui__input_cancel(de_ui *ui,de_entity e) {
 if(ui->advanced->active_input.bits!=e.bits)return;
 ui->advanced->active_input=(de_entity){0};DeUiInteractionState *s=de_entity_get(ui->world,e,ui->ids.state);if(s)s->focused=false;
 if(ui->desc.input)de_text_input_stop(ui->desc.input);
}
void de_ui__input_geometry(de_ui *ui,de_entity e,de_vec2 *offset,f32 *width,void *user) {
 (void)user;de_ui_advanced *a=ui->advanced;
 const DeUiTextInput *in=de_entity_get_const(ui->world,e,a->input);
 const de_ui_editor *s=de_entity_get_const(ui->world,e,a->editor);if(!in||!s)return;
 offset->x=4*ui->scale-s->scroll_x;offset->y=4*ui->scale;
 *width=in->multiline?fmaxf(1,*width-8*ui->scale):0;
}
static u32 display_offset(const DeUiTextInput *in,u32 byte){if(!in->password)return byte;u32 b[257],n=boundaries(in,b),i=0;while(i+1<n&&b[i]<byte)++i;return i*3;}
static void selection_word(DeUiTextInput *in,de_ui_editor *s) {
 u32 b[257],n=boundaries(in,b),i=0;while(i+1<n&&b[i]<in->caret)++i;u32 lo=i,hi=i;
 while(lo&&!space(in->buffer,b[lo-1]))--lo;while(hi+1<n&&!space(in->buffer,b[hi]))++hi;
 s->anchor=b[lo];in->caret=b[hi];
}
void de_ui__input_frame(de_ui *ui,de_entity e,f32 dt) {
 de_ui_advanced *a=ui->advanced;de_input *input=ui->desc.input;if(!input)return;
 if(!de_ui__widget_enabled(ui,e)){de_ui__input_cancel(ui,e);return;}
 if(!de_entity_has(ui->world,e,a->editor))return;
 de_ui_widget_table *table=de_ui__widget_table(ui);de_ui_pointer_state *p=&ui->widgets->pointer;
 bool clicked=de_mouse_pressed(input,DE_MOUSE_LEFT),hit=p->hovered.bits==e.bits;
 if(clicked&&hit)de_ui__input_submit(ui,e);
 if(table->focused){de_entity focus=table->focused(ui);if(focus.bits==e.bits&&a->active_input.bits!=e.bits)de_ui__input_submit(ui,e);else if(focus.bits!=e.bits&&a->active_input.bits==e.bits)de_ui__input_cancel(ui,e);}
 else if(clicked&&!hit&&a->active_input.bits==e.bits)de_ui__input_cancel(ui,e);
 if(a->active_input.bits!=e.bits)return;
 DeUiTextInput *in=de_entity_get(ui->world,e,a->input);de_ui_editor *s=de_entity_get(ui->world,e,a->editor);
 const DeUiNode *node=de_entity_get_const(ui->world,e,ui->ids.node);if(!node)return;
 f32 width=in->multiline?fmaxf(1,node->computed_rect.z-8*ui->scale):0;
 s->blink+=isfinite(dt)&&dt>0?dt:0;
 bool shift=de_key_down(input,DE_KEY_LSHIFT)||de_key_down(input,DE_KEY_RSHIFT);
 bool ctrl=de_key_down(input,DE_KEY_LCTRL)||de_key_down(input,DE_KEY_RCTRL)||de_key_down(input,DE_KEY_LGUI)||de_key_down(input,DE_KEY_RGUI);
 bool word=ctrl||de_key_down(input,DE_KEY_LALT)||de_key_down(input,DE_KEY_RALT);
 if(clicked&&hit)s->dragging=true;
 if(s->dragging&&de_mouse_down(input,DE_MOUSE_LEFT)){
 de_text_layout *l=layout(ui,e,in->buffer,in->length,width);if(l){de_vec2 point={p->position.x-node->computed_rect.x-4*ui->scale+s->scroll_x,p->position.y-node->computed_rect.y-4*ui->scale};
 u32 byte=de_text_layout_hit_test(l,point);de_text_layout_free(l);in->caret=snap(in,byte);if(clicked&&!shift)s->anchor=in->caret;
 if(clicked&&s->last_click>0&&a->time-s->last_click<.35f)selection_word(in,s);if(clicked)s->last_click=a->time;}}
 if(!de_mouse_down(input,DE_MOUSE_LEFT))s->dragging=false;
 if(de_key_pressed(input,DE_KEY_LEFT))de_ui__input_move(in,s,-1,shift,word);
 if(de_key_pressed(input,DE_KEY_RIGHT))de_ui__input_move(in,s,1,shift,word);
 if(de_key_pressed(input,DE_KEY_HOME)){in->caret=0;if(!shift)s->anchor=0;}
 if(de_key_pressed(input,DE_KEY_END)){in->caret=in->length;if(!shift)s->anchor=in->caret;}
 if(ctrl&&de_key_pressed(input,DE_KEY_A)){s->anchor=0;in->caret=in->length;}
 bool changed=false;
 if(ctrl&&(de_key_pressed(input,DE_KEY_C)||de_key_pressed(input,DE_KEY_X))&&!in->password){u32 lo=in->caret<s->anchor?in->caret:s->anchor,hi=in->caret>s->anchor?in->caret:s->anchor;char copy[256];memcpy(copy,in->buffer+lo,hi-lo);copy[hi-lo]=0;a->clipboard_set(copy);
 if(de_key_pressed(input,DE_KEY_X))changed|=de_ui__input_replace(in,s,(de_str){"",0});}
 if(ctrl&&de_key_pressed(input,DE_KEY_V)){char paste[256];size_t count=a->clipboard_get(paste,sizeof paste);if(count>=sizeof paste)count=sizeof paste-1;changed|=de_ui__input_replace(in,s,(de_str){paste,count});}
 if(de_key_pressed(input,DE_KEY_BACKSPACE)||de_key_pressed(input,DE_KEY_DELETE)){
 if(s->anchor==in->caret)de_ui__input_move(in,s,de_key_pressed(input,DE_KEY_BACKSPACE)?-1:1,true,word);
 changed|=de_ui__input_replace(in,s,(de_str){"",0});}
 de_text_input_state typed;de_text_input_get(input,&typed);
 if(typed.text_bytes)changed|=de_ui__input_replace(in,s,(de_str){typed.text,typed.text_bytes});
 if(de_key_pressed(input,DE_KEY_RETURN)){if(in->multiline)changed|=de_ui__input_replace(in,s,(de_str){"\n",1});else de_ui__widget_event(ui,DE_UI_EV_SUBMIT,e,(de_name){0},0);}
 if(de_key_pressed(input,DE_KEY_ESCAPE))de_ui__input_cancel(ui,e);
 if(changed)de_ui__widget_event(ui,DE_UI_EV_CHANGED,e,(de_name){0},(f32)in->length);
 de_ui__input_refresh(ui,e);
 de_text_layout *l=layout(ui,e,s->display,(u32)strlen(s->display),width);
 if(l){de_vec4 c=de_text_layout_caret(l,s->displayed_caret);de_text_layout_free(l);
 if(!in->multiline){f32 room=fmaxf(1,node->computed_rect.z-10*ui->scale);if(c.x-s->scroll_x>room)s->scroll_x=c.x-room;if(c.x-s->scroll_x<0)s->scroll_x=c.x;s->scroll_x=fmaxf(0,s->scroll_x);}
 de_platform_text_area area={(i32)(node->computed_rect.x+c.x-s->scroll_x),(i32)(node->computed_rect.y+c.y),2,(i32)c.w,0};
 if(ui->desc.window.id){u32 pw=0,ph=0,lw=0,lh=0;de_window_size_pixels(ui->desc.window,&pw,&ph);de_window_size_points(ui->desc.window,&lw,&lh);if(pw&&ph){area.x=(i32)((f32)area.x*lw/pw);area.y=(i32)((f32)area.y*lh/ph);area.h=(i32)((f32)area.h*lh/ph);}}
 de_text_input_area(input,&area);}
}
void de_ui__input_draw(de_ui *ui,de_entity e,de_vec4 r,de_ui_draw_emitter *em) {
 de_ui_advanced *a=ui->advanced;if(a->active_input.bits!=e.bits)return;
 const DeUiTextInput *in=de_entity_get_const(ui->world,e,a->input);const de_ui_editor *s=de_entity_get_const(ui->world,e,a->editor);if(!in||!s)return;
 de_text_layout *l=layout(ui,e,s->display,(u32)strlen(s->display),in->multiline?fmaxf(1,r.z-8*ui->scale):0);if(!l)return;
 f32 x=r.x+4*ui->scale-s->scroll_x,y=r.y+4*ui->scale;
 if(in->caret!=s->anchor){u32 lo=in->caret<s->anchor?in->caret:s->anchor,hi=in->caret>s->anchor?in->caret:s->anchor;
 u32 b[257],n=boundaries(in,b);for(u32 i=0;i+1<n;++i)if(b[i]>=lo&&b[i]<hi){de_vec4 c=de_text_layout_caret(l,display_offset(in,b[i])),d=de_text_layout_caret(l,display_offset(in,b[i+1]));
 f32 left=fminf(c.x,d.x),right=fmaxf(c.x,d.x);de_ui__emit_rect(em,(de_vec4){x+left,y+c.y,fmaxf(2,right-left),c.w},(de_vec4){.18f,.4f,.8f,.5f},(de_vec4){0},0,0);}}
 if(s->preedit_end>s->preedit_start){de_vec4 c=de_text_layout_caret(l,s->preedit_start),d=de_text_layout_caret(l,s->preedit_end);de_ui__emit_rect(em,(de_vec4){x+fminf(c.x,d.x),y+c.y+c.w-2,fabsf(d.x-c.x),1},(de_vec4){1,1,1,1},(de_vec4){0},0,0);}
 if(fmodf(s->blink,1)<.6f){de_vec4 c=de_text_layout_caret(l,s->displayed_caret);de_ui__emit_rect(em,(de_vec4){x+c.x,y+c.y,1,c.w},(de_vec4){1,1,1,1},(de_vec4){0},0,0);}
 de_text_layout_free(l);
}
