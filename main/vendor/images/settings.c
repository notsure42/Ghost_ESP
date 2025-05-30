#ifdef __has_include
    #if __has_include("lvgl.h")
        #ifndef LV_LVGL_H_INCLUDE_SIMPLE
            #define LV_LVGL_H_INCLUDE_SIMPLE
        #endif
    #endif
#endif

#if defined(LV_LVGL_H_INCLUDE_SIMPLE)
    #include "lvgl.h"
#else
    #include "lvgl/lvgl.h"
#endif


#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

#ifndef LV_ATTRIBUTE_IMG_SETTINGS
#define LV_ATTRIBUTE_IMG_SETTINGS
#endif

const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST LV_ATTRIBUTE_IMG_SETTINGS uint8_t settings_map[] = {
  0x00, 0x00, 0x00, 0x00, 	/*Color of index 0*/
  0x00, 0x00, 0x00, 0xff, 	/*Color of index 1*/

  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
  0x00, 0x00, 0x07, 0xf8, 0x00, 0x00, 0x00, 
  0x00, 0x00, 0x0f, 0xfc, 0x00, 0x00, 0x00, 
  0x00, 0x00, 0x0f, 0xfc, 0x00, 0x00, 0x00, 
  0x00, 0x00, 0x0f, 0xfc, 0x00, 0x00, 0x00, 
  0x00, 0x0f, 0x0f, 0xfc, 0x3c, 0x00, 0x00, 
  0x00, 0x1f, 0x9f, 0xfe, 0x7e, 0x00, 0x00, 
  0x00, 0x3f, 0xff, 0xff, 0xff, 0x00, 0x00, 
  0x00, 0x7f, 0xff, 0xff, 0xff, 0x80, 0x00, 
  0x00, 0xff, 0xff, 0xff, 0xff, 0xc0, 0x00, 
  0x00, 0xff, 0xff, 0xff, 0xff, 0xc0, 0x00, 
  0x00, 0xff, 0xff, 0xff, 0xff, 0xc0, 0x00, 
  0x00, 0x7f, 0xff, 0xff, 0xff, 0x80, 0x00, 
  0x00, 0x7f, 0xff, 0xff, 0xff, 0x80, 0x00, 
  0x00, 0x3f, 0xff, 0xff, 0xff, 0x00, 0x00, 
  0x00, 0x3f, 0xfe, 0x1f, 0xff, 0x00, 0x00, 
  0x00, 0x7f, 0xf8, 0x07, 0xff, 0x80, 0x00, 
  0x07, 0xff, 0xf0, 0x03, 0xff, 0xf8, 0x00, 
  0x0f, 0xff, 0xe0, 0x01, 0xff, 0xfc, 0x00, 
  0x0f, 0xff, 0xe0, 0x01, 0xff, 0xfc, 0x00, 
  0x0f, 0xff, 0xc0, 0x00, 0xff, 0xfc, 0x00, 
  0x0f, 0xff, 0xc0, 0x00, 0xff, 0xfc, 0x00, 
  0x0f, 0xff, 0xc0, 0x00, 0xff, 0xfc, 0x00, 
  0x0f, 0xff, 0xc0, 0x00, 0xff, 0xfc, 0x00, 
  0x0f, 0xff, 0xe0, 0x01, 0xff, 0xfc, 0x00, 
  0x0f, 0xff, 0xe0, 0x01, 0xff, 0xfc, 0x00, 
  0x03, 0xff, 0xf0, 0x03, 0xff, 0xf0, 0x00, 
  0x00, 0x3f, 0xf8, 0x07, 0xff, 0x00, 0x00, 
  0x00, 0x3f, 0xfe, 0x1f, 0xff, 0x00, 0x00, 
  0x00, 0x3f, 0xff, 0xff, 0xff, 0x00, 0x00, 
  0x00, 0x7f, 0xff, 0xff, 0xff, 0x80, 0x00, 
  0x00, 0xff, 0xff, 0xff, 0xff, 0xc0, 0x00, 
  0x00, 0xff, 0xff, 0xff, 0xff, 0xc0, 0x00, 
  0x00, 0xff, 0xff, 0xff, 0xff, 0xc0, 0x00, 
  0x00, 0xff, 0xff, 0xff, 0xff, 0xc0, 0x00, 
  0x00, 0x7f, 0xff, 0xff, 0xff, 0x80, 0x00, 
  0x00, 0x3f, 0xff, 0xff, 0xff, 0x00, 0x00, 
  0x00, 0x1f, 0x8f, 0xfc, 0x7e, 0x00, 0x00, 
  0x00, 0x0e, 0x0f, 0xfc, 0x1c, 0x00, 0x00, 
  0x00, 0x00, 0x0f, 0xfc, 0x00, 0x00, 0x00, 
  0x00, 0x00, 0x0f, 0xfc, 0x00, 0x00, 0x00, 
  0x00, 0x00, 0x0f, 0xfc, 0x00, 0x00, 0x00, 
  0x00, 0x00, 0x07, 0xf8, 0x00, 0x00, 0x00, 
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
};

const lv_img_dsc_t settings_icon = {
  .header.cf = LV_IMG_CF_INDEXED_1BIT,
  .header.always_zero = 0,
  .header.reserved = 0,
  .header.w = 50,
  .header.h = 50,
  .data_size = 358,
  .data = settings_map,
};
