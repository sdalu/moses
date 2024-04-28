#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <stdio.h>

#include "integration.h"
#include "lvgl.h"



extern const lv_image_dsc_t analog_inputs_blank;



static void event_handler(lv_event_t * e)
{
    lv_obj_t * obj = lv_event_get_target(e);
    LV_UNUSED(obj);
    LV_LOG_USER("Button %d clicked", (int)lv_obj_get_index(obj));
}




int main(void)
{
    integration_init();

    for (int i = 0 ; i < 50 ; i++) {
    float temperature, pressure, humidity;
    if (get_tph(&temperature, &pressure, &humidity) < 0) {
	printf("Failed reading environmental informatons\n");
    } else {
	printf("Temperature:   %0.2f deg C\n", temperature);
	printf("Pressure:      %0.2f \n", pressure);
	printf("Humiditye:     %0.2f \n", humidity);
    }
    usleep(500000);
    }


    
    /* set screen background to white */
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_100, 0);




    lv_obj_t * win = lv_win_create(scr);
    lv_obj_t * btn = lv_win_add_button(win, LV_SYMBOL_LEFT, 20);
    lv_obj_add_event_cb(btn, event_handler, LV_EVENT_CLICKED, NULL);

    lv_win_add_title(win, "A title");

    btn = lv_win_add_button(win, LV_SYMBOL_RIGHT, 20);
    lv_obj_add_event_cb(btn, event_handler, LV_EVENT_CLICKED, NULL);

    btn = lv_win_add_button(win, LV_SYMBOL_CLOSE, 20);
    lv_obj_add_event_cb(btn, event_handler, LV_EVENT_CLICKED, NULL);

    lv_obj_t * cont  = lv_win_get_content(win);  /*Content can be added here*/
    lv_obj_t * obj = lv_label_create(cont);


    
    //lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0xff0000), LV_PART_MAIN);

#if 0
    lv_obj_t * icon = lv_image_create(lv_screen_active());
    
    /*From variable*/
    lv_image_set_src(icon, &analog_inputs_blank);
    //lv_image_set_scale(icon, 1);
    lv_obj_set_align(icon, LV_ALIGN_CENTER);
#endif


#if 0

    LV_IMAGE_DECLARE(img_wink_png);
    lv_obj_t * img;

    img = lv_image_create(lv_screen_active());
    lv_image_set_src(img, &img_wink_png);
    lv_obj_align(img, LV_ALIGN_LEFT_MID, 20, 0);

    img = lv_image_create(lv_screen_active());
    /* Assuming a File system is attached to letter 'A'
     * E.g. set LV_USE_FS_STDIO 'A' in lv_conf.h */
    lv_image_set_src(img, "A:lvgl/examples/libs/lodepng/wink.png");
    lv_obj_align(img, LV_ALIGN_RIGHT_MID, -20, 0);

#endif
    
    /* create label */
    //obj = lv_label_create(scr);
    lv_obj_set_align(obj, LV_ALIGN_CENTER);
    lv_obj_set_height(obj, LV_SIZE_CONTENT);
    lv_obj_set_width(obj, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(obj, &lv_font_montserrat_14, 0);
    //lv_obj_set_style_text_color(obj, lv_color_black(), 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(0xff0000), 0);
    
    lv_label_set_text(obj, "Hello World!");

    
    for(;;) {
	lv_timer_handler();
	usleep(5000);
    }
    
    

    return 0;
}
