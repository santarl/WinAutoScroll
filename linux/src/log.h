/* wlautoscroll - tiny stderr logger (journald picks it up under systemd) */
#ifndef WLAS_LOG_H
#define WLAS_LOG_H

extern int wlas_verbose;

void wlas_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define LOGE(...) wlas_log(0, __VA_ARGS__)
#define LOGI(...) wlas_log(1, __VA_ARGS__)
#define LOGD(...) wlas_log(2, __VA_ARGS__)

#endif
