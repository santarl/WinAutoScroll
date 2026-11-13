/* wlautoscroll - Windows virtual-key code -> evdev KEY_* translation */
#ifndef WLAS_KEYMAP_H
#define WLAS_KEYMAP_H

/*
 * Translate a Windows VK_* code (as used by trigger_vk_code) into up to two
 * evdev key codes. Generic VK_SHIFT / VK_CONTROL / VK_MENU map to both the
 * left and right key. Returns the number of codes written to out[] (0 if the
 * VK code has no Linux equivalent in the table).
 */
int keymap_vk_to_keys(int vk, int out[2]);

#endif
