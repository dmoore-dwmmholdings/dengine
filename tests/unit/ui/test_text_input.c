#include "ui_test_util.h"
#include "ui/widgets/advanced_internal.h"
#include "../input/input_test_util.h"
typedef struct edit_fx {ui_fx base;de_input *input;de_entity editor;} edit_fx;
static void edit_fx_setup(edit_fx *f){ui_fx_setup(&f->base);f->input=input_test_create(NULL);ASSERT_TRUE(f->input);f->base.ui->desc.input=f->input;
 f->editor=ui_node(&f->base,(de_entity){0});DeUiNode *n=ui_node_edit(&f->base,f->editor);n->width=(de_ui_len){DE_UI_PX,200};n->height=(de_ui_len){DE_UI_PX,40};
 de_entity_add(f->base.world,f->editor,f->base.ui->advanced->input);ui_frame(&f->base);de_ui__input_submit(f->base.ui,f->editor);}
static void edit_fx_teardown(edit_fx *f){ui_fx_teardown(&f->base);de_input_destroy(f->input);}
static void edit_frame(edit_fx *f){input_test_frame(f->input);ASSERT_EQ(de_world_run_schedule(f->base.world,DE_SCHEDULE_PRE_UPDATE,INPUT_TEST_DT,0),DE_OK);ui_frame(&f->base);}
static DeUiTextInput *editing(edit_fx *f){return de_entity_get(f->base.world,f->editor,f->base.ui->advanced->input);}
TEST_F(ui_text_input, injected_japanese_preedit_then_commit_no_buffer_mutation,edit_fx){
 input_test_text_edit(f->input,"にほん",2,1);edit_frame(f);
 DeUiTextInput *in=editing(f);EXPECT_EQ(in->length,0u);EXPECT_EQ(in->buffer[0],0);
 const de_ui_editor *state=de_entity_get_const(f->base.world,f->editor,f->base.ui->advanced->editor);
 EXPECT_TRUE(strstr(state->display,"にほん")!=NULL);EXPECT_TRUE(state->preedit_end>state->preedit_start);
 input_test_text_edit(f->input,"",0,0);input_test_text(f->input,"日本");edit_frame(f);
 in=editing(f);EXPECT_EQ(in->length,6u);EXPECT_TRUE(strcmp(in->buffer,"日本")==0);
 state=de_entity_get_const(f->base.world,f->editor,f->base.ui->advanced->editor);EXPECT_EQ(state->preedit_start,state->preedit_end);
}
TEST(ui_text_input, grapheme_selection_word_navigation_and_255_byte_cap){
 DeUiTextInput in={0};de_ui_editor state={0};
 const char *utf="a\xcc\x81👨‍👩‍👧‍👦b";
 ASSERT_TRUE(de_ui__input_replace(&in,&state,(de_str){utf,strlen(utf)}));EXPECT_EQ(in.length,29u);
 de_ui__input_move(&in,&state,-1,false,false);EXPECT_EQ(in.caret,28u);
 de_ui__input_move(&in,&state,-1,true,false);EXPECT_EQ(in.caret,3u);EXPECT_EQ(state.anchor,28u);
 ASSERT_TRUE(de_ui__input_replace(&in,&state,(de_str){"",0}));EXPECT_TRUE(strcmp(in.buffer,"a\xcc\x81""b")==0);
 memset(&in,0,sizeof in);memset(&state,0,sizeof state);char ascii[254];memset(ascii,'x',253);ascii[253]=0;
 ASSERT_TRUE(de_ui__input_replace(&in,&state,(de_str){ascii,253}));
 EXPECT_FALSE(de_ui__input_replace(&in,&state,(de_str){"日",3}));EXPECT_EQ(in.length,253u);
 ASSERT_TRUE(de_ui__input_replace(&in,&state,(de_str){"ab",2}));EXPECT_EQ(in.length,255u);EXPECT_EQ(in.buffer[255],0);
 memset(&in,0,sizeof in);memset(&state,0,sizeof state);de_ui__input_replace(&in,&state,(de_str){"one two three",13});
 de_ui__input_move(&in,&state,-1,false,true);EXPECT_EQ(in.caret,8u);
 de_ui__input_move(&in,&state,-1,true,true);EXPECT_EQ(in.caret,4u);EXPECT_EQ(state.anchor,8u);
}
static char clip[256];static de_result clip_set(const char *s){strncpy(clip,s,255);clip[255]=0;return DE_OK;}
static size_t clip_get(char *out,size_t cap){size_t n=strlen(clip);if(cap){size_t c=n<cap-1?n:cap-1;memcpy(out,clip,c);out[c]=0;}return n;}
TEST_F(ui_text_input, injected_select_copy_cut_paste_password_and_multiline_submit,edit_fx){
 f->base.ui->advanced->clipboard_set=clip_set;f->base.ui->advanced->clipboard_get=clip_get;
 input_test_text(f->input,"hello");edit_frame(f);input_test_key(f->input,DE_KEY_LCTRL,true);input_test_key(f->input,DE_KEY_A,true);edit_frame(f);
 input_test_key(f->input,DE_KEY_A,false);input_test_key(f->input,DE_KEY_C,true);edit_frame(f);EXPECT_TRUE(strcmp(clip,"hello")==0);
 input_test_key(f->input,DE_KEY_C,false);input_test_key(f->input,DE_KEY_X,true);edit_frame(f);EXPECT_EQ(editing(f)->length,0u);
 input_test_key(f->input,DE_KEY_X,false);input_test_key(f->input,DE_KEY_V,true);edit_frame(f);EXPECT_TRUE(strcmp(editing(f)->buffer,"hello")==0);
 input_test_key(f->input,DE_KEY_V,false);input_test_key(f->input,DE_KEY_LCTRL,false);editing(f)->password=true;edit_frame(f);
 const de_ui_editor *state=de_entity_get_const(f->base.world,f->editor,f->base.ui->advanced->editor);EXPECT_TRUE(strstr(state->display,"hello")==NULL);EXPECT_EQ(strlen(state->display),15u);
 de_event_reader reader=de_event_reader_create(f->base.world,f->base.ui->event);input_test_key(f->input,DE_KEY_RETURN,true);edit_frame(f);
 const de_ui_event *ev;u32 submits=0;while((ev=de_event_next(f->base.world,&reader)))if(ev->kind==DE_UI_EV_SUBMIT)++submits;EXPECT_EQ(submits,1u);
 input_test_key(f->input,DE_KEY_RETURN,false);edit_frame(f);editing(f)->multiline=true;input_test_key(f->input,DE_KEY_RETURN,true);edit_frame(f);EXPECT_TRUE(strchr(editing(f)->buffer,'\n')!=NULL);
}
