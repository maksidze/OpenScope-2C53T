/*
 * ESP Activity host.
 *
 * Activities are rendered by pinned LVGL 9.5.0 when ESP_LINK_LVGL is built.
 * The native code below draws only the trusted launcher/disconnect boundary;
 * it is not a second private remote renderer. Runtime XML is abstracted at
 * this boundary but not accepted yet: the v9.5 changelog explicitly says the
 * XML engine was removed from the main repository for development elsewhere,
 * and tag 85aa60d has no public lv_xml runtime C API in src/. Phase 1 therefore
 * creates a bounded retained LVGL object model from
 * versioned records. Replacing record creation with the eventual pinned XML
 * loader does not change UART, lifecycle, POWER, or subject-update semantics.
 */
#include "esp_activity.h"
#include "esp_link.h"
#include "esp_link_protocol.h"
#include "shared_mem.h"
#include "lcd.h"
#include "font.h"
#include "theme.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <string.h>

#ifdef ESP_LINK_LVGL
#include "lvgl.h"
#endif

#define APP_MAX 6u
#define APP_NAME_MAX 28u
#define UI_MSG_MAX 384u
#define UI_QUEUE_LEN 3u
#define DRAW_ROWS 12u
#define DRAW_BYTES (LCD_WIDTH * DRAW_ROWS * sizeof(uint16_t))
_Static_assert(SHMEM_POOL_SIZE - DRAW_BYTES == 111104u,
               "lv_conf.h LV_MEM_SIZE must match the Activity pool layout");

typedef struct { uint16_t id; char name[APP_NAME_MAX]; } app_item_t;
typedef struct { uint16_t length,transaction; uint8_t data[UI_MSG_MAX]; } ui_message_t;
typedef struct {
    app_item_t apps[APP_MAX];
    ui_message_t messages[UI_QUEUE_LEN];
    volatile uint8_t msg_head, msg_tail;
    volatile uint16_t last_ui_transaction;
    uint8_t app_count, selected;
    uint32_t token;
    uint32_t catalog_request_ms;
    bool active, link_lost, needs_start, needs_stop;
    bool launcher_dirty, catalog_requested;
    volatile uint8_t local_key;
    volatile uint8_t local_key_pending;
#ifdef ESP_LINK_LVGL
    lv_display_t *display;
    lv_indev_t *keypad;
    lv_group_t *group;
    lv_obj_t *screen;
    struct { uint16_t id; uint8_t kind; lv_obj_t *obj; lv_chart_series_t *series; } subjects[8];
    uint8_t subject_count;
    uint32_t last_tick;
#endif
} activity_state_t;

static activity_state_t *a;

static uint16_t rd16(const uint8_t *p){return (uint16_t)p[0]|((uint16_t)p[1]<<8);}
static uint32_t rd32(const uint8_t *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static bool take_string(const uint8_t **p,uint16_t *left,char *out,size_t cap)
{
    if(*left<1)return false;
    uint8_t n=*(*p)++;(*left)--;
    if(n>*left)return false;
    size_t copy=n<cap-1?n:cap-1;
    memcpy(out,*p,copy);out[copy]=0;*p+=n;*left-=n;
    return true;
}

bool esp_activity_init(void)
{
    a=pvPortMalloc(sizeof(*a));if(a){memset(a,0,sizeof(*a));a->launcher_dirty=true;}
    return a != NULL;
}

void esp_activity_deinit(void)
{
    if(!a)return;
    /* This rollback is used only during pre-scheduler link bring-up, before an
     * Activity can own the shared pool or create LVGL objects. Runtime teardown
     * remains display-task-only in lv_stop(). */
    vPortFree(a);a=NULL;
}

size_t esp_activity_heap_required(void)
{
    /* One heap_4 allocation: 8-byte header followed by 8-byte alignment. */
    return (sizeof(activity_state_t)+15u)&~(size_t)7u;
}

void esp_activity_receive_app_list(const uint8_t *p,uint16_t n)
{
    if(!a||n<2||p[0]!=OSLP_RAW_ABI_VERSION)return;
    taskENTER_CRITICAL();a->app_count=0;uint8_t count=p[1];p+=2;n-=2;
    for(uint8_t i=0;i<count&&i<APP_MAX&&n>=2;i++){
        app_item_t *it=&a->apps[a->app_count];it->id=rd16(p);p+=2;n-=2;
        if(!take_string(&p,&n,it->name,sizeof(it->name)))break;
        char discard[2];if(!take_string(&p,&n,discard,sizeof(discard))||!take_string(&p,&n,discard,sizeof(discard)))break;
        a->app_count++;
    }
    if(a->selected>=a->app_count)a->selected=0;
    a->catalog_requested=false;
    a->launcher_dirty=true;
    taskEXIT_CRITICAL();
}

void esp_activity_receive_start(const uint8_t *p,uint16_t n)
{
    if(!a||n<7||p[0]!=OSLP_RAW_ABI_VERSION)return;
    taskENTER_CRITICAL();a->token=rd32(p+3);a->active=a->token!=0;a->needs_start=a->active;a->link_lost=false;a->msg_head=a->msg_tail=0;a->last_ui_transaction=0;taskEXIT_CRITICAL();
}

void esp_activity_receive_state(const uint8_t *p,uint16_t n,uint16_t transaction)
{
    if(!a||!a->active||n<6||n>UI_MSG_MAX||p[0]!=OSLP_RAW_ABI_VERSION||rd32(p+2)!=a->token)return;
    if(transaction && transaction==a->last_ui_transaction){esp_link_ui_ack(transaction);return;}
    taskENTER_CRITICAL();
    for(uint8_t i=a->msg_tail;i!=a->msg_head;i=(uint8_t)((i+1u)%UI_QUEUE_LEN))
        if(transaction && a->messages[i].transaction==transaction){taskEXIT_CRITICAL();return;}
    uint8_t next=(uint8_t)((a->msg_head+1u)%UI_QUEUE_LEN);
    if(next!=a->msg_tail){ui_message_t *m=&a->messages[a->msg_head];m->length=n;m->transaction=transaction;memcpy(m->data,p,n);a->msg_head=next;}taskEXIT_CRITICAL();
}

void esp_activity_link_ready(void)
{
    if(!a)return;
    taskENTER_CRITICAL();
    a->link_lost=false;
    a->app_count=0;
    a->selected=0;
    a->catalog_requested=false;
    a->catalog_request_ms=0;
    a->launcher_dirty=true;
    taskEXIT_CRITICAL();
}
void esp_activity_link_lost(void){if(a){a->link_lost=true;a->active=false;a->needs_stop=true;a->token=0;a->app_count=0;a->catalog_requested=false;a->launcher_dirty=true;}}
void esp_activity_stop_local(void){if(a){a->active=false;a->needs_stop=true;a->token=0;}}
bool esp_activity_active(void){return a&&a->active;}
uint32_t esp_activity_token(void){return a?a->token:0;}

void *esp_activity_lvgl_heap(void)
{
    uint8_t *pool=shared_mem_get(SHMEM_OWNER_ESP_ACTIVITY);
    return pool?pool+DRAW_BYTES:NULL;
}
size_t esp_activity_lvgl_heap_size(void){return SHMEM_POOL_SIZE-DRAW_BYTES;}

void esp_activity_draw_launcher(void)
{
    const theme_t *th=theme_get();lcd_fill_rect(0,16,LCD_WIDTH,LCD_HEIGHT-32,th->background);
    font_draw_string_center(160,20,"ESP32 Apps",th->text_primary,th->background,&font_large);
    if(!a||!esp_link_started()){
        const esp_link_stats_t *st=esp_link_stats();
        const char *why=st->heap_failures?"ESP link: no RTOS heap":
                        (st->task_create_failures?"ESP link: task failed":"ESP link disabled");
        font_draw_string_center(160,92,why,th->warning,th->background,&font_medium);
        font_draw_string_center(160,118,"ESP transport not started",th->text_secondary,th->background,&font_small);return;
    }
    if(!esp_link_available()){
        const esp_link_stats_t *st=esp_link_stats();char line[48];
        font_draw_string_center(160,76,"Waiting for ESP32 HELLO",th->warning,th->background,&font_medium);
        snprintf(line,sizeof(line),"AT32 RX:%lu TX:%lu",(unsigned long)st->rx_bytes,(unsigned long)st->tx_bytes);
        font_draw_string_center(160,106,line,th->text_secondary,th->background,&font_small);
        snprintf(line,sizeof(line),"frame/CRC errors: %lu",(unsigned long)(st->framing_errors+st->crc_or_frame_errors));
        font_draw_string_center(160,126,line,th->text_secondary,th->background,&font_small);
        snprintf(line,sizeof(line),"IRQ R:%lu T:%lu E:%lu",(unsigned long)st->rx_irq_count,
                 (unsigned long)st->tx_irq_count,(unsigned long)st->error_irq_count);
        font_draw_string_center(160,146,line,th->text_secondary,th->background,&font_small);
        snprintf(line,sizeof(line),"STS:%02lX C1:%04lX B:%u M:%u",(unsigned long)(st->last_status&0xFFu),
                 (unsigned long)st->last_control1,(unsigned)st->baud_divider,(unsigned)st->uart8_gmux);
        font_draw_string_center(160,166,line,th->text_secondary,th->background,&font_small);
#ifdef ESP_LINK_BRIDGE
        snprintf(line,sizeof(line),"USART2 PA2/PA3 %lu baud",(unsigned long)ESP_LINK_BAUD);
#else
        snprintf(line,sizeof(line),"PE:%02X PE0:%u  TX20/RX21 crossed",(unsigned)st->pe01_config,
                 (unsigned)st->pe0_level);
#endif
        font_draw_string_center(160,192,line,th->text_secondary,th->background,&font_small);return;
    }
    if(!a->app_count){font_draw_string_center(160,92,"Loading applications...",th->text_secondary,th->background,&font_medium);return;}
    for(uint8_t i=0;i<a->app_count;i++){
        uint16_t y=(uint16_t)(50+i*25);uint16_t bg=i==a->selected?th->menu_selected_bg:th->background;
        if(i==a->selected)lcd_fill_rect(14,y-2,292,22,bg);
        font_draw_string(20,y,i==a->selected?">":" ",th->highlight,bg,&font_medium);
        font_draw_string(38,y,a->apps[i].name,th->text_primary,bg,&font_medium);
    }
    font_draw_string_center(160,218,"OK: open   MENU: back",th->text_secondary,th->background,&font_small);
}

void esp_activity_launcher_move(int d){if(a&&a->app_count){int x=(int)a->selected+d;if(x<0)x=a->app_count-1;if(x>=a->app_count)x=0;a->selected=(uint8_t)x;}}
void esp_activity_launcher_start_selected(void){if(a&&a->selected<a->app_count)esp_link_request_app_start(a->apps[a->selected].id);}
void esp_activity_input_key(uint8_t key)
{
    if(!a||!a->active)return;
    a->local_key=key;a->local_key_pending=2;
    esp_link_send_key(key,0,0);esp_link_send_key(key,1,0);
}

#ifdef ESP_LINK_LVGL
static void flush_cb(lv_display_t *display,const lv_area_t *area,uint8_t *px)
{
    uint16_t w=(uint16_t)(area->x2-area->x1+1),h=(uint16_t)(area->y2-area->y1+1);
    lcd_transaction_begin();
    lcd_set_window((uint16_t)area->x1,(uint16_t)area->y1,w,h);
    lcd_write_pixels((const uint16_t *)px,(uint32_t)w*h);
    lcd_transaction_end();
    lv_display_flush_ready(display);
}
static void input_read(lv_indev_t *indev,lv_indev_data_t *data)
{
    (void)indev;uint8_t key=a->local_key;
    if(key==10)data->key=LV_KEY_UP;else if(key==11)data->key=LV_KEY_DOWN;
    else if(key==12)data->key=LV_KEY_LEFT;else if(key==13)data->key=LV_KEY_RIGHT;
    else if(key==14)data->key=LV_KEY_ENTER;else data->key=LV_KEY_NEXT;
    if(a->local_key_pending==2){data->state=LV_INDEV_STATE_PRESSED;a->local_key_pending=1;}
    else {data->state=LV_INDEV_STATE_RELEASED;a->local_key_pending=0;}
}
static void lv_start(void)
{
    if(shared_mem_owner()!=SHMEM_OWNER_NONE&&shared_mem_owner()!=SHMEM_OWNER_ESP_ACTIVITY){a->active=false;a->link_lost=true;return;}
    uint8_t *pool=shared_mem_acquire(SHMEM_OWNER_ESP_ACTIVITY);
    /* Ownership can change between the optimistic check and acquire if a
     * native feature claims the lifecycle pool. Never pass NULL to LVGL's
     * draw-buffer or heap port; fail closed and leave the native UI intact. */
    if(!pool){a->active=false;a->link_lost=true;a->needs_stop=false;return;}
    lv_init();
    a->display=lv_display_create(LCD_WIDTH,LCD_HEIGHT);lv_display_set_flush_cb(a->display,flush_cb);
    lv_display_set_buffers(a->display,pool,NULL,DRAW_BYTES,LV_DISPLAY_RENDER_MODE_PARTIAL);
    a->group=lv_group_create();a->keypad=lv_indev_create();lv_indev_set_type(a->keypad,LV_INDEV_TYPE_KEYPAD);lv_indev_set_read_cb(a->keypad,input_read);lv_indev_set_group(a->keypad,a->group);
    a->screen=lv_obj_create(NULL);
    lv_obj_set_style_pad_all(a->screen,0,0);lv_obj_remove_flag(a->screen,LV_OBJ_FLAG_SCROLLABLE);
    lv_screen_load(a->screen);a->subject_count=0;a->last_tick=(uint32_t)(xTaskGetTickCount()*portTICK_PERIOD_MS);
}
static void lv_stop(void)
{
    if(a->display){lv_deinit();a->display=NULL;a->screen=NULL;a->group=NULL;a->keypad=NULL;}
    if(shared_mem_owner()==SHMEM_OWNER_ESP_ACTIVITY)shared_mem_release();
}
static lv_obj_t *subject(uint16_t id,uint8_t kind,lv_chart_series_t **series){for(uint8_t i=0;i<a->subject_count;i++)if(a->subjects[i].id==id&&a->subjects[i].kind==kind){if(series)*series=a->subjects[i].series;return a->subjects[i].obj;}return NULL;}
static void grid_delete(lv_event_t *e){lv_free(lv_event_get_user_data(e));}
static void grid_define(const uint8_t *p,uint16_t n)
{
    if(n<12 || a->subject_count>=8)return;
    uint16_t id=rd16(p);int16_t x=(int16_t)rd16(p+2),y=(int16_t)rd16(p+4);
    int16_t w=(int16_t)rd16(p+6),h=(int16_t)rd16(p+8);uint8_t cols=p[10],count=p[11];
    if(!cols||cols>10||!count||count>50||w<=0||h<=0||x<0||y<0||x+w>LCD_WIDTH||y+h>LCD_HEIGHT)return;
    p+=12;n-=12;
    const uint8_t *check=p;uint16_t remain=n;
    for(uint8_t i=0;i<count;i++){
        if(!remain||!*check||*check>60||*check>=remain)return;
        uint8_t len=*check++;remain--;
        for(uint8_t j=0;j<len;j++)if(check[j]==0||check[j]=='\n')return;
        check+=len;remain-=len;
    }
    if(remain)return;
    size_t slots=count+(count-1u)/cols+1u;
    char **map=lv_malloc(slots*sizeof(*map)+n);
    if(!map)return;
    char *text=(char *)(map+slots);uint8_t m=0;
    for(uint8_t i=0;i<count;i++){
        if(i && i%cols==0)map[m++]="\n";
        uint8_t len=*p++;map[m++]=text;memcpy(text,p,len);text[len]=0;text+=len+1;p+=len;
    }
    map[m]="";
    lv_obj_t *o=lv_buttonmatrix_create(a->screen);
    lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);
    lv_obj_set_style_pad_all(o,1,0);lv_obj_set_style_pad_row(o,2,0);lv_obj_set_style_pad_column(o,2,0);
    lv_obj_set_style_pad_all(o,0,LV_PART_ITEMS);
    lv_obj_set_style_border_width(o,0,LV_PART_ITEMS);
    lv_obj_set_style_radius(o,2,LV_PART_ITEMS);
    lv_buttonmatrix_set_map(o,(const char *const *)map);
    lv_buttonmatrix_set_one_checked(o,true);
    lv_buttonmatrix_set_button_ctrl(o,0,LV_BUTTONMATRIX_CTRL_CHECKED);
    lv_obj_add_event_cb(o,grid_delete,LV_EVENT_DELETE,map);
    a->subjects[a->subject_count++]=(typeof(a->subjects[0])){id,3,o,NULL};
}
static void process_ui(const uint8_t *p,uint16_t n)
{
    if(n<6)return;
    uint8_t msg=p[1];p+=6;n-=6;
    if(msg==1&&n>=3){uint16_t id=rd16(p);(void)id;p+=2;n-=2;char title[48];if(take_string(&p,&n,title,sizeof(title))){lv_obj_clean(a->screen);a->subject_count=0;lv_obj_t*l=lv_label_create(a->screen);lv_label_set_text(l,title);lv_obj_align(l,LV_ALIGN_TOP_MID,0,3);}}
    else if(msg==2&&n>=12&&a->subject_count<8){uint16_t id=rd16(p);int16_t x=(int16_t)rd16(p+2),y=(int16_t)rd16(p+4),w=(int16_t)rd16(p+6),h=(int16_t)rd16(p+8);p+=11;n-=11;char text[96];if(w>0&&h>0&&x>=0&&y>=0&&x+w<=LCD_WIDTH&&y+h<=LCD_HEIGHT&&take_string(&p,&n,text,sizeof(text))){lv_obj_t*l=lv_label_create(a->screen);lv_label_set_text(l,text);lv_obj_set_pos(l,x,y);lv_obj_set_size(l,w,h);a->subjects[a->subject_count++]=(typeof(a->subjects[0])){id,1,l,NULL};}}
    else if(msg==3&&n>=12&&a->subject_count<8){uint16_t id=rd16(p);int16_t x=(int16_t)rd16(p+2),y=(int16_t)rd16(p+4),w=(int16_t)rd16(p+6),h=(int16_t)rd16(p+8);uint16_t cap=rd16(p+10);if(w>0&&h>0&&cap<=192&&x>=0&&y>=0&&x+w<=LCD_WIDTH&&y+h<=LCD_HEIGHT){lv_obj_t*c=lv_chart_create(a->screen);lv_obj_set_pos(c,x,y);lv_obj_set_size(c,w,h);lv_chart_set_point_count(c,cap);lv_chart_series_t*srs=lv_chart_add_series(c,lv_palette_main(LV_PALETTE_CYAN),LV_CHART_AXIS_PRIMARY_Y);a->subjects[a->subject_count++]=(typeof(a->subjects[0])){id,2,c,srs};}}
    else if(msg==5)grid_define(p,n);
    else if(msg==0x10&&n>=3){uint16_t id=rd16(p);p+=2;n-=2;char text[128];lv_obj_t*o=subject(id,1,NULL);if(o&&take_string(&p,&n,text,sizeof(text)))lv_label_set_text(o,text);}
    else if(msg==0x11&&n>=7){uint16_t id=rd16(p);float v;memcpy(&v,p+2,4);p+=6;n-=6;char unit[16],text[48];lv_obj_t*o=subject(id,1,NULL);if(o&&take_string(&p,&n,unit,sizeof(unit))){lv_snprintf(text,sizeof(text),"%.3f %s",(double)v,unit);lv_label_set_text(o,text);}}
    else if(msg==0x12&&n>=4){uint16_t id=rd16(p),count=rd16(p+2);p+=4;n-=4;lv_chart_series_t*srs=NULL;lv_obj_t*o=subject(id,2,&srs);if(o&&srs&&count<=192&&n>=count*2u){lv_chart_set_point_count(o,count);for(uint16_t i=0;i<count;i++)lv_chart_set_next_value(o,srs,(int16_t)rd16(p+i*2u));}}
    else if(msg==0x13&&n==4){lv_obj_t*o=subject(rd16(p),3,NULL);uint16_t index=rd16(p+2);if(o&&lv_buttonmatrix_get_button_text(o,index)){lv_buttonmatrix_clear_button_ctrl_all(o,LV_BUTTONMATRIX_CTRL_CHECKED);lv_buttonmatrix_set_button_ctrl(o,index,LV_BUTTONMATRIX_CTRL_CHECKED);}}
}
#endif

void esp_activity_display_leave(void)
{
    if(!a)return;
    /* Remote mode changes can leave Wi-Fi/LVGL without visiting the launcher.
     * Teardown remains display-task-owned and must release the shared arena
     * before a scope FFT/persistence view tries to acquire it. No LCD redraw. */
#ifdef ESP_LINK_LVGL
    if(a->display)lv_stop();
#endif
    a->active=false;a->needs_start=false;a->needs_stop=false;
    a->msg_tail=a->msg_head;a->token=0;a->local_key_pending=0;
}

void esp_activity_display_service(void)
{
    if(!a)return;
    const uint32_t service_now=(uint32_t)(xTaskGetTickCount()*portTICK_PERIOD_MS);
    /* Transport requests must be made while UART8 owns PE0/PE1, never from
     * esp_activity_draw_launcher(): that renderer normally runs inside an XMC
     * transaction. Retry once per second until a catalog response arrives so
     * one frame lost at the XMC/UART hand-off cannot strand the launcher. */
    if(!a->active && esp_link_available() && a->app_count==0 &&
       (!a->catalog_requested || service_now-a->catalog_request_ms>=1000u)) {
        a->catalog_requested=true;
        a->catalog_request_ms=service_now;
        esp_link_request_app_list();
    }
#ifdef ESP_LINK_LVGL
    bool stopped=false;
    if(a->needs_stop){a->needs_stop=false;lv_stop();stopped=true;}
    if(a->needs_start){a->needs_start=false;lv_start();}
    if(a->display&&shared_mem_owner()!=SHMEM_OWNER_ESP_ACTIVITY){a->active=false;a->link_lost=true;lv_stop();}
    while(a->display&&a->msg_tail!=a->msg_head){ui_message_t*m=&a->messages[a->msg_tail];process_ui(m->data,m->length);a->last_ui_transaction=m->transaction;esp_link_ui_ack(m->transaction);a->msg_tail=(uint8_t)((a->msg_tail+1u)%UI_QUEUE_LEN);}
    if(a->display){uint32_t now=(uint32_t)(xTaskGetTickCount()*portTICK_PERIOD_MS);lv_tick_inc(now-a->last_tick);a->last_tick=now;lv_timer_handler();}
    else if(stopped||a->launcher_dirty){
        a->launcher_dirty=false;
        lcd_transaction_begin();
        esp_activity_draw_launcher();
        lcd_transaction_end();
    }
#else
    if(a->needs_start||a->needs_stop||a->launcher_dirty){a->needs_start=false;a->needs_stop=false;a->launcher_dirty=false;a->active=false;lcd_transaction_begin();esp_activity_draw_launcher();lcd_transaction_end();}
#endif
}
