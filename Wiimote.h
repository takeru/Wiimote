#ifndef _WIIMOTE_H_
#define _WIIMOTE_H_

#include <cstdint>

typedef void (* wiimote_callback_fun_t)(uint8_t number, uint8_t *, size_t, void *);

typedef struct {
    wiimote_callback_fun_t fun;
    void *data;
} wiimote_callback_t;

class Wiimote {
    public:
        static void init();
        static void handle();
        static void register_callback(uint8_t number, wiimote_callback_fun_t cb, void *data);
};

#endif
