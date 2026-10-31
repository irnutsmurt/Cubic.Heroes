// Native replacements for the game's SPU tasks (port/native/bullet_cd_task.cpp).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Register them with the runtime per DOTHEROES_NATIVE_CD; before the game runs.
void dotheroes_native_init(void);

#ifdef __cplusplus
}
#endif
