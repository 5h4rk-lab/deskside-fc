// APP_DIAG: measurement and diagnostics hooks (scripted touch, frame-time trace,
// full-repaint and part-hiding commands). Off in the release env `app`: no serial
// byte can inject a touch there. On in `app_diag` and `app_debug` (platformio.ini).
#pragma once

#ifndef APP_DIAG
#define APP_DIAG 0
#endif
