#pragma once

/*
 * Native/unit builds do not use the ESP-IDF component compile definitions and
 * retain qualification helpers by default. Customer firmware profiles set
 * this definition explicitly to zero in main/CMakeLists.txt.
 */
#ifndef D1L_ENABLE_QUALIFICATION_HOOKS
#define D1L_ENABLE_QUALIFICATION_HOOKS 1
#endif

#if D1L_ENABLE_QUALIFICATION_HOOKS != 0 && D1L_ENABLE_QUALIFICATION_HOOKS != 1
#error "D1L_ENABLE_QUALIFICATION_HOOKS must be exactly 0 or 1"
#endif
