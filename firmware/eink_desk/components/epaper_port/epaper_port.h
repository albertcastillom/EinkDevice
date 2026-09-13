#ifndef EPAPER_PORT_H
#define EPAPER_PORT_H

#include <stdint.h>

typedef uint8_t UBYTE;
typedef uint16_t UWORD;
typedef uint32_t UDOUBLE;

#define EPD_WIDTH 800
#define EPD_HEIGHT 480
#define EPD_SIZE_MONO 48000
#define EPD_SIZE_4GRAY 96000

#ifdef __cplusplus
extern "C" {
#endif

void epaper_port_init(void);
void EPD_Init(void);
void EPD_Init_Fast(void);
void EPD_Init_Partial(void);
void EPD_Init_4GRAY(void);
void EPD_Clear(void);
void EPD_Clear_Black(void);
void EPD_Display(const UBYTE *image);
void EPD_Display_Base(const UBYTE *image);
void EPD_Display_Fast(const UBYTE *image);
void EPD_Display_Fast_Base(const UBYTE *image);
void EPD_Display_OneShot(const UBYTE *image);
void EPD_Display_Partial(
    const UBYTE *image,
    UWORD x_start,
    UWORD y_start,
    UWORD x_end,
    UWORD y_end
);
void EPD_Display_4Gray(const UBYTE *image);
void EPD_Sleep(void);

#ifdef __cplusplus
}
#endif

#endif
