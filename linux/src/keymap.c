#include "keymap.h"

#include <linux/input-event-codes.h>

static const unsigned short digits[10] = {
	KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,
};

/* Indexed by VK 'A'..'Z' - 0x41. */
static const unsigned short letters[26] = {
	KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I,
	KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R,
	KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
};

/* Indexed by VK_NUMPAD0.. - 0x60. */
static const unsigned short numpad[10] = {
	KEY_KP0, KEY_KP1, KEY_KP2, KEY_KP3, KEY_KP4,
	KEY_KP5, KEY_KP6, KEY_KP7, KEY_KP8, KEY_KP9,
};

/* Indexed by VK_F1.. - 0x70 (F1..F24). */
static const unsigned short fkeys[24] = {
	KEY_F1,  KEY_F2,  KEY_F3,  KEY_F4,  KEY_F5,  KEY_F6,
	KEY_F7,  KEY_F8,  KEY_F9,  KEY_F10, KEY_F11, KEY_F12,
	KEY_F13, KEY_F14, KEY_F15, KEY_F16, KEY_F17, KEY_F18,
	KEY_F19, KEY_F20, KEY_F21, KEY_F22, KEY_F23, KEY_F24,
};

static const struct {
	unsigned short vk, key;
} misc[] = {
	{0x08, KEY_BACKSPACE}, {0x09, KEY_TAB},        {0x0D, KEY_ENTER},
	{0x13, KEY_PAUSE},     {0x14, KEY_CAPSLOCK},   {0x1B, KEY_ESC},
	{0x20, KEY_SPACE},     {0x21, KEY_PAGEUP},     {0x22, KEY_PAGEDOWN},
	{0x23, KEY_END},       {0x24, KEY_HOME},       {0x25, KEY_LEFT},
	{0x26, KEY_UP},        {0x27, KEY_RIGHT},      {0x28, KEY_DOWN},
	{0x2C, KEY_SYSRQ}, /* VK_SNAPSHOT / Print Screen */
	{0x2D, KEY_INSERT},    {0x2E, KEY_DELETE},
	{0x5B, KEY_LEFTMETA},  {0x5C, KEY_RIGHTMETA},  {0x5D, KEY_COMPOSE},
	{0x6A, KEY_KPASTERISK},{0x6B, KEY_KPPLUS},     {0x6D, KEY_KPMINUS},
	{0x6E, KEY_KPDOT},     {0x6F, KEY_KPSLASH},
	{0x90, KEY_NUMLOCK},   {0x91, KEY_SCROLLLOCK},
	{0xA0, KEY_LEFTSHIFT}, {0xA1, KEY_RIGHTSHIFT},
	{0xA2, KEY_LEFTCTRL},  {0xA3, KEY_RIGHTCTRL},
	{0xA4, KEY_LEFTALT},   {0xA5, KEY_RIGHTALT},
	{0xAD, KEY_MUTE},      {0xAE, KEY_VOLUMEDOWN}, {0xAF, KEY_VOLUMEUP},
	{0xB0, KEY_NEXTSONG},  {0xB1, KEY_PREVIOUSSONG},
	{0xB2, KEY_STOPCD},    {0xB3, KEY_PLAYPAUSE},
	{0xBA, KEY_SEMICOLON}, {0xBB, KEY_EQUAL},      {0xBC, KEY_COMMA},
	{0xBD, KEY_MINUS},     {0xBE, KEY_DOT},        {0xBF, KEY_SLASH},
	{0xC0, KEY_GRAVE},     {0xDB, KEY_LEFTBRACE},{0xDC, KEY_BACKSLASH},
	{0xDD, KEY_RIGHTBRACE}, {0xDE, KEY_APOSTROPHE},
};

int keymap_vk_to_keys(int vk, int out[2])
{
	if (vk >= 0x30 && vk <= 0x39) {
		out[0] = digits[vk - 0x30];
		return 1;
	}
	if (vk >= 0x41 && vk <= 0x5A) {
		out[0] = letters[vk - 0x41];
		return 1;
	}
	if (vk >= 0x60 && vk <= 0x69) {
		out[0] = numpad[vk - 0x60];
		return 1;
	}
	if (vk >= 0x70 && vk <= 0x87) {
		out[0] = fkeys[vk - 0x70];
		return 1;
	}
	/* Generic modifiers: the Windows hook reports the L/R variants, but a
	 * config written with the generic code should still do something sane. */
	if (vk == 0x10) { out[0] = KEY_LEFTSHIFT; out[1] = KEY_RIGHTSHIFT; return 2; }
	if (vk == 0x11) { out[0] = KEY_LEFTCTRL;  out[1] = KEY_RIGHTCTRL;  return 2; }
	if (vk == 0x12) { out[0] = KEY_LEFTALT;   out[1] = KEY_RIGHTALT;   return 2; }

	for (unsigned i = 0; i < sizeof misc / sizeof misc[0]; i++) {
		if (misc[i].vk == vk) {
			out[0] = misc[i].key;
			return 1;
		}
	}
	return 0;
}
