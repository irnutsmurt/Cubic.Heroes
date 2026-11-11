// A tree whose SPU images are not lifted (no build/gen/spu, or another executable's images): no generated SPU
// code (spu_native.h). Each image's call declines, so its host keeps the lifted task (water_task_init,
// waver_game_init).
#include "spu_native.h"

#define SPU_NATIVE_ABSENT(ns)                                                                                       \
    namespace ns {                                                                                                  \
    void load(uint8_t*) {}                                                                                          \
    spu_native::Result call(uint8_t*, u128*, uint32_t, const spu_native::Host&, spu_native::Outcome*)              \
    {                                                                                                               \
        return spu_native::Result::declined;                                                                        \
    }                                                                                                               \
    bool has_entry(uint32_t) { return false; }                                                                      \
    const char* const kSource = nullptr;                                                                            \
    }

SPU_NATIVE_ABSENT(water_gen)
SPU_NATIVE_ABSENT(waver_gen)
