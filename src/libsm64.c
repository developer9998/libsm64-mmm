#ifndef SM64_LIB_EXPORT
#define SM64_LIB_EXPORT
#endif

#include "libsm64.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#include "decomp/include/PR/os_cont.h"
#include "decomp/engine/math_util.h"
#include "decomp/include/sm64.h"
#include "decomp/shim.h"
#include "decomp/memory.h"
#include "decomp/global_state.h"
#include "decomp/game/mario.h"
#include "decomp/game/object_stuff.h"
#include "decomp/engine/surface_collision.h"
#include "decomp/engine/graph_node.h"
#include "decomp/engine/geo_layout.h"
#include "decomp/game/rendering_graph_node.h"
#include "decomp/mario/geo.inc.h"
#include "decomp/game/platform_displacement.h"
#include "decomp/include/seq_ids.h"

#include "debug_print.h"
#include "load_surfaces.h"
#include "gfx_adapter.h"
#include "load_anim_data.h"
#include "load_tex_data.h"
#include "obj_pool.h"

static struct AllocOnlyPool* s_mario_geo_pool = NULL;
static struct GraphNode* s_mario_graph_node = NULL;

static bool s_init_global = false;
static bool s_init_one_mario = false;

struct MarioInstance
{
    struct GlobalState* globalState;
};
struct ObjPool s_mario_instance_pool = { 0, 0 };

static void update_button(bool on, u16 button)
{
    gController.buttonPressed &= ~button;

    if (on)
    {
        if ((gController.buttonDown & button) == 0)
            gController.buttonPressed |= button;

        gController.buttonDown |= button;
    }
    else
    {
        gController.buttonDown &= ~button;
    }
}

static struct Area* allocate_area(void)
{
    struct Area* result = malloc(sizeof(struct Area));
    memset(result, 0, sizeof(struct Area));

    result->flags = 1;
    result->camera = malloc(sizeof(struct Camera));
    memset(result->camera, 0, sizeof(struct Camera));

    return result;
}

static void free_area(struct Area* area)
{
    free(area->camera);
    free(area);
}

SM64_LIB_FN void sm64_global_init(uint8_t* rom, uint8_t* outTexture, SM64DebugPrintFunctionPtr debugPrintFunction)
{
    if (s_init_global)
        sm64_global_terminate();

    s_init_global = true;
    g_debug_print_func = debugPrintFunction;

    load_mario_textures_from_rom(rom, outTexture);
    load_mario_anims_from_rom(rom);

    memory_init();
}

SM64_LIB_FN void sm64_global_terminate(void)
{
    if (!s_init_global) return;

    global_state_bind(NULL);

    if (s_init_one_mario)
    {
        for (int i = 0; i < s_mario_instance_pool.size; ++i)
            if (s_mario_instance_pool.objects[i] != NULL)
                sm64_mario_delete(i);

        obj_pool_free_all(&s_mario_instance_pool);
    }

    s_init_global = false;
    s_init_one_mario = false;

    if (s_mario_geo_pool)
    {
        alloc_only_pool_free(s_mario_geo_pool);
        s_mario_geo_pool = NULL;
    }

    surfaces_unload_all();
    unload_mario_anims();
    memory_terminate();
}

SM64_LIB_FN void sm64_static_surfaces_load(const struct SM64Surface* surfaceArray, uint32_t numSurfaces)
{
    surfaces_load_static(surfaceArray, numSurfaces);
}

SM64_LIB_FN int32_t sm64_mario_create(int16_t x, int16_t y, int16_t z)
{
    int32_t marioIndex = obj_pool_alloc_index(&s_mario_instance_pool, sizeof(struct MarioInstance));
    struct MarioInstance* newInstance = s_mario_instance_pool.objects[marioIndex];

    newInstance->globalState = global_state_create();
    global_state_bind(newInstance->globalState);

    if (!s_init_one_mario)
    {
        s_init_one_mario = true;
        s_mario_geo_pool = alloc_only_pool_init();
        s_mario_graph_node = process_geo_layout(s_mario_geo_pool, mario_geo_ptr);
    }

    gCurrSaveFileNum = 1;
    gMarioObject = hack_allocate_mario();
    gCurrentArea = allocate_area();
    gCurrentObject = gMarioObject;

    gMarioSpawnInfoVal.startPos[0] = x;
    gMarioSpawnInfoVal.startPos[1] = y;
    gMarioSpawnInfoVal.startPos[2] = z;

    gMarioSpawnInfoVal.startAngle[0] = 0;
    gMarioSpawnInfoVal.startAngle[1] = 0;
    gMarioSpawnInfoVal.startAngle[2] = 0;

    gMarioSpawnInfoVal.areaIndex = 0;
    gMarioSpawnInfoVal.activeAreaIndex = 0;
    gMarioSpawnInfoVal.behaviorArg = 0;
    gMarioSpawnInfoVal.behaviorScript = NULL;
    gMarioSpawnInfoVal.unk18 = NULL;
    gMarioSpawnInfoVal.next = NULL;

    init_mario_from_save_file();

    if (init_mario() < 0)
    {
        sm64_mario_delete(marioIndex);
        return -1;
    }

    set_mario_action(gMarioState, ACT_SPAWN_SPIN_AIRBORNE, 0);
    find_floor(x, y, z, &gMarioState->floor);

    return marioIndex;
}

SM64_LIB_FN void sm64_mario_tick(int32_t marioId, const struct SM64MarioInputs* inputs, struct SM64MarioState* outState, struct SM64MarioGeometryBuffers* outBuffers)
{
    if (marioId >= s_mario_instance_pool.size || s_mario_instance_pool.objects[marioId] == NULL)
    {
        DEBUG_PRINT("Tried to tick non-existant Mario with ID: %u", marioId);
        return;
    }

    global_state_bind(((struct MarioInstance*)s_mario_instance_pool.objects[marioId])->globalState);

    update_button(inputs->buttonA, A_BUTTON);
    update_button(inputs->buttonB, B_BUTTON);
    update_button(inputs->buttonZ, Z_TRIG);

    gMarioState->area->camera->yaw = atan2s(inputs->camLookZ, inputs->camLookX);

    gController.stickX = -64.0f * inputs->stickX;
    gController.stickY = 64.0f * inputs->stickY;
    gController.stickMag = sqrtf(gController.stickX * gController.stickX + gController.stickY * gController.stickY);

    apply_mario_platform_displacement();
    bhv_mario_update();
    update_mario_platform(); // TODO platform grabbed here and used next tick could be a use-after-free

    gfx_adapter_bind_output_buffers(outBuffers);

    geo_process_root_hack_single_node(s_mario_graph_node);

    gAreaUpdateCounter++;

    outState->health = gMarioState->health;
    vec3f_copy(outState->position, gMarioState->pos);
    vec3f_copy(outState->velocity, gMarioState->vel);
    outState->faceAngle = (float)gMarioState->faceAngle[1] / 32768.0f * 3.14159f;
}

SM64_LIB_FN void sm64_mario_delete(int32_t marioId)
{
    if (marioId >= s_mario_instance_pool.size || s_mario_instance_pool.objects[marioId] == NULL)
    {
        DEBUG_PRINT("Tried to delete non-existant Mario with ID: %u", marioId);
        return;
    }

    struct GlobalState* globalState = ((struct MarioInstance*)s_mario_instance_pool.objects[marioId])->globalState;
    global_state_bind(globalState);

    free(gMarioObject);
    free_area(gCurrentArea);

    global_state_delete(globalState);
    obj_pool_free_index(&s_mario_instance_pool, marioId);
}

SM64_LIB_FN void sm64_set_mario_action(int32_t marioId, uint32_t action)
{
    if (marioId >= s_mario_instance_pool.size || s_mario_instance_pool.objects[marioId] == NULL)
    {
        DEBUG_PRINT("Failed to set action of a non-existent Mario with ID: %u", marioId);
        return;
    }

    struct GlobalState* globalState = ((struct MarioInstance*)s_mario_instance_pool.objects[marioId])->globalState;
    global_state_bind(globalState);

    set_mario_action(gMarioState, action, 0);
}

SM64_LIB_FN void sm64_set_mario_state(int32_t marioId, uint32_t flags)
{
    if (marioId >= s_mario_instance_pool.size || s_mario_instance_pool.objects[marioId] == NULL)
    {
        DEBUG_PRINT("Failed to set state of a non-existent Mario with ID: %u", marioId);
        return;
    }

    struct GlobalState* globalState = ((struct MarioInstance*)s_mario_instance_pool.objects[marioId])->globalState;
    global_state_bind(globalState);

    gMarioState->flags = flags;
}

SM64_LIB_FN void sm64_set_mario_position(int32_t marioId, float x, float y, float z)
{
    if (marioId >= s_mario_instance_pool.size || s_mario_instance_pool.objects[marioId] == NULL)
    {
        DEBUG_PRINT("Failed to set position of a non-existent Mario with ID: %u", marioId);
        return;
    }

    struct GlobalState* globalState = ((struct MarioInstance*)s_mario_instance_pool.objects[marioId])->globalState;
    global_state_bind(globalState);

    gMarioState->pos[0] = x;
    gMarioState->pos[1] = y;
    gMarioState->pos[2] = z;
    vec3f_copy(gMarioState->marioObj->header.gfx.pos, gMarioState->pos);
}

SM64_LIB_FN void sm64_set_mario_angle(int32_t marioId, int16_t x, int16_t y, int16_t z)
{
    if (marioId >= s_mario_instance_pool.size || s_mario_instance_pool.objects[marioId] == NULL)
    {
        DEBUG_PRINT("Failed to set angle of a non-existent Mario with ID: %u", marioId);
        return;
    }

    struct GlobalState* globalState = ((struct MarioInstance*)s_mario_instance_pool.objects[marioId])->globalState;
    global_state_bind(globalState);

    vec3s_set(gMarioState->faceAngle, x, y, z);
    vec3s_set(gMarioState->marioObj->header.gfx.angle, 0, gMarioState->faceAngle[1], 0);
}

SM64_LIB_FN void sm64_set_mario_velocity(int32_t marioId, float x, float y, float z)
{
    if (marioId >= s_mario_instance_pool.size || s_mario_instance_pool.objects[marioId] == NULL)
    {
        DEBUG_PRINT("Failed to set velocity of a non-existent Mario with ID: %u", marioId);
        return;
    }

    struct GlobalState* globalState = ((struct MarioInstance*)s_mario_instance_pool.objects[marioId])->globalState;
    global_state_bind(globalState);

    gMarioState->vel[0] = x;
    gMarioState->vel[1] = y;
    gMarioState->vel[2] = z;
}

SM64_LIB_FN void sm64_set_mario_forward_velocity(int32_t marioId, float vel)
{
    if (marioId >= s_mario_instance_pool.size || s_mario_instance_pool.objects[marioId] == NULL)
    {
        DEBUG_PRINT("Failed to set forwards velocity of a non-existent Mario with ID: %u", marioId);
        return;
    }

    struct GlobalState* globalState = ((struct MarioInstance*)s_mario_instance_pool.objects[marioId])->globalState;
    global_state_bind(globalState);

    gMarioState->forwardVel = vel;
}

SM64_LIB_FN void sm64_set_mario_health(int32_t marioId, uint16_t health)
{
    if (marioId >= s_mario_instance_pool.size || s_mario_instance_pool.objects[marioId] == NULL)
    {
        DEBUG_PRINT("Failed to set health of a non-existent Mario with ID: %u", marioId);
        return;
    }

    struct GlobalState* globalState = ((struct MarioInstance*)s_mario_instance_pool.objects[marioId])->globalState;
    global_state_bind(globalState);

    gMarioState->health = health;
    gMarioState->hurtCounter = 0;
    gMarioState->healCounter = 0;
}

SM64_LIB_FN void sm64_set_mario_invincibility(int32_t marioId, int16_t timer)
{
    if (marioId >= s_mario_instance_pool.size || s_mario_instance_pool.objects[marioId] == NULL)
    {
        DEBUG_PRINT("Failed to set invincibility timer of a non-existent Mario with ID: %u", marioId);
        return;
    }

    struct GlobalState* globalState = ((struct MarioInstance*)s_mario_instance_pool.objects[marioId])->globalState;
    global_state_bind(globalState);

    gMarioState->invincTimer = timer;
}

SM64_LIB_FN void sm64_mario_interact_cap( int32_t marioId, uint32_t capFlag, uint16_t capTime)
{
	struct GlobalState *globalState = ((struct MarioInstance *)s_mario_instance_pool.objects[ marioId ])->globalState;
    global_state_bind( globalState );
	
	uint16_t capMusic = 0;
	if(gMarioState->action != ACT_GETTING_BLOWN && capFlag != 0)
	{
		gMarioState->flags &= ~MARIO_CAP_ON_HEAD & ~MARIO_CAP_IN_HAND;
		gMarioState->flags |= capFlag;
		
		switch(capFlag)
		{
			case MARIO_VANISH_CAP:
				if(capTime == 0) capTime = 600;
				break;
			case MARIO_METAL_CAP:
                if(capTime == 0) capTime = 600;
                break;
            case MARIO_WING_CAP:
                if(capTime == 0) capTime = 1800;
                break;
		}
		
		if (capTime > gMarioState->capTimer) {
            gMarioState->capTimer = capTime;
        }
		
		if ((gMarioState->action & ACT_FLAG_IDLE) || gMarioState->action == ACT_WALKING) {
            gMarioState->flags |= MARIO_CAP_IN_HAND;
            set_mario_action(gMarioState, ACT_PUTTING_ON_CAP, 0);
        } else {
            gMarioState->flags |= MARIO_CAP_ON_HEAD;
        }
	}
}

SM64_LIB_FN uint32_t sm64_surface_object_create(const struct SM64SurfaceObject* surfaceObject)
{
    uint32_t id = surfaces_load_object(surfaceObject);
    return id;
}

SM64_LIB_FN void sm64_surface_object_move(uint32_t objectId, const struct SM64ObjectTransform* transform)
{
    surface_object_update_transform(objectId, transform);
}

SM64_LIB_FN void sm64_surface_object_delete(uint32_t objectId)
{
    // A mario standing on the platform that is being destroyed will have a pointer to freed memory if we don't clear it.
    for (int i = 0; i < s_mario_instance_pool.size; ++i)
    {
        if (s_mario_instance_pool.objects[i] == NULL)
            continue;

        struct GlobalState* state = ((struct MarioInstance*)s_mario_instance_pool.objects[i])->globalState;
        if (state->mgMarioObject->platform == surfaces_object_get_transform_ptr(objectId))
            state->mgMarioObject->platform = NULL;
    }

    surfaces_unload_object(objectId);
}