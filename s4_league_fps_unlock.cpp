#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <cstdint>
#include <cstdlib>
#include <pthread.h>
#include <cstring>
#include <cmath>

// mingw don't provide a mprotect wrap
#include <memoryapi.h>

#define ENABLE_LOGGING 1

#if ENABLE_LOGGING
FILE *log_file = NULL;
char log_buf[500];
pthread_mutex_t log_mutex;

#define LOG(...) \
{ \
	pthread_mutex_lock(&log_mutex); \
	snprintf(log_buf, sizeof(log_buf), __VA_ARGS__); \
	int _len = strlen(log_buf); \
	if(_len + 1 < sizeof(log_buf)){ \
		log_buf[_len] = '\n'; \
		log_buf[_len + 1] = '\0'; \
	} \
	if(log_file != NULL){ \
		fprintf(log_file, "%s", log_buf); \
		fflush(log_file); \
	}else{ \
		printf("warning: log_file is NULL, logging to stdout \n"); \
		printf("%s", log_buf); \
	} \
	pthread_mutex_unlock(&log_mutex); \
}

#else // ENABLE_LOGGING
#define LOG(...)
#endif //ENABLE_LOGGING

#define VERBOSE 0
#if VERBOSE
	#define LOG_VERBOSE(...) LOG(__VA_ARGS__)
#else // VERBOSE
	#define LOG_VERBOSE(...)
#endif //VERBOSE

// __sync_synchronize() is not enough..?
#define INIT_MEM_FENCE() \
static bool _mem_fence_ready = 0; \
static pthread_mutex_t _mem_fence; \
if(!_mem_fence_ready){ \
	if(pthread_mutex_init(&_mem_fence, NULL)){ \
		LOG("failed to initialize mem fence for %s", __FUNCTION__); \
		exit(1); \
	} \
	_mem_fence_ready = true; \
}

#define MEM_FENCE() \
pthread_mutex_lock(&_mem_fence); \
pthread_mutex_unlock(&_mem_fence);

uint32_t frametime;
uint8_t weapon_slot;
float set_drop_val;

struct __attribute__ ((packed)) time_context{
	double unknown;
	double last_t;
	double delta_t;
	float delta_t_modifier;
};

struct __attribute__ ((packed)) game_context{
	// 0x48 debug toggle? 1 byte
	// 0x49 fps limiter toggle 1 byte
	// 0x4c unknown 1 byte
	// 0x4a unknown 1 byte
	uint8_t unknown[0x48];
	uint8_t online_verbose_toggle;
	uint8_t fps_limiter_toggle;
};

// static at 0x01642e28
static struct game_context *(*fetch_game_context)(void) = (struct game_context *(*)(void)) 0x004ad790;
static void (__attribute__((thiscall)) *update_time_delta)(struct time_context *ctx) = (void (__attribute__((thiscall)) *)(struct time_context *ctx)) 0x00ff7f30;

// contains actor state
struct ctx_01642f30{
	uint8_t unknown[0xb0];
	uint8_t actor_state;
};
static struct ctx_01642f30 *(*fetch_ctx_01642f30)(void) = (struct ctx_01642f30 *(*)(void)) 0x004ae0a0;

// this is a looong function with a lot of branches, but it seems to use the SetDrop value during a jump attack
struct ctx_fun_005e4020{
	uint8_t unknown[0x2cc + 0x4];
	float set_drop_val;
};
static void (__attribute__((thiscall)) *orig_fun_005e4020)(void *, uint32_t);
void __attribute__((thiscall)) patched_fun_005e4020(struct ctx_fun_005e4020 *ctx, uint32_t param_1){
	orig_fun_005e4020(ctx, param_1);
	if((void *)0x0051f508 == __builtin_return_address(1)){
		set_drop_val = ctx->set_drop_val;
		LOG_VERBOSE("%s: updating player set_drop_val to %f", __FUNCTION__, set_drop_val);
	}
	LOG_VERBOSE("%s: ctx 0x%08x, param_1 %u, set_drop_val %f, 0x%08x -> 0x%08x -> 0x%08x", __FUNCTION__, ctx, param_1, ctx->set_drop_val, __builtin_return_address(2), __builtin_return_address(1), __builtin_return_address(0));
}

static void hook_fun_005e4020(){
	LOG("hooking fun_005e4020");

	uint8_t intended_trampoline[] = {
		// space for original instruction
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		// MOV eax,0x005e402a
		0xb8, 0x2a, 0x40, 0x5e, 0x00,
		// JMP eax
		0xff, 0xe0
	};
	memcpy((void *)intended_trampoline, (void *)0x005e4020, 10);

	uint8_t intended_patch[] = {
		// MOV eax, patched_fun_005e4020
		0xb8, 0, 0, 0, 0,
		// JMP eax
		0xff, 0xe0,
		// nop nop nop
		0x90, 0x90, 0x90
	};
	*(uint32_t *)&intended_patch[1] = (uint32_t)patched_fun_005e4020;

	orig_fun_005e4020 = (void (__attribute__((thiscall)) *)(void*, uint32_t))VirtualAlloc(NULL, sizeof(intended_trampoline), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	memcpy((void *)orig_fun_005e4020, intended_trampoline, sizeof(intended_trampoline));
	DWORD old_protect;
	VirtualProtect((void *)orig_fun_005e4020, sizeof(intended_trampoline), PAGE_EXECUTE_READ, &old_protect);

	memcpy((void *)0x005e4020, intended_patch, sizeof(intended_patch));
}

// it seems that weapon slot switch of all actors goes here
struct __attribute__((packed)) switch_weapon_slot_ctx{
	uint8_t unknown[0x24];
	uint8_t weapon_slot;
};
static void (__attribute__((thiscall)) *orig_switch_weapon_slot)(void*, uint32_t);
void __attribute__((thiscall)) patched_switch_weapon_slot(struct switch_weapon_slot_ctx *ctx, uint32_t param_1){
	INIT_MEM_FENCE();
	orig_switch_weapon_slot(ctx, param_1);
	MEM_FENCE();
	void *ret_addr =  __builtin_return_address(0);
	if(ret_addr == (void *)0x00b9a188 || ret_addr == (void *)0x007edf3e){
		weapon_slot = ctx->weapon_slot;
	}
	LOG("%s: ctx 0x%08x, param_1 %u, weapon slot switched to %u, 0x%08x -> 0x%08x", __FUNCTION__, ctx, param_1, weapon_slot, __builtin_return_address(1), __builtin_return_address(0));
}
static void hook_switch_weapon_slot(){
	LOG("hooking switch_weapon_slot");

	uint8_t intended_trampoline[] = {
		// space for original instruction
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		// MOV eax,0x00b9990a
		0xb8, 0x0a, 0x99, 0xb9, 0x00,
		// JMP eax
		0xff, 0xe0
	};
	memcpy((void *)intended_trampoline, (void *)0x00b99900, 10);

	uint8_t intended_patch[] = {
		// MOV eax, patched_switch_weapon_slot
		0xb8, 0, 0, 0, 0,
		// JMP eax
		0xff, 0xe0,
		// nop nop nop
		0x90, 0x90, 0x90
	};
	*(uint32_t *)&intended_patch[1] = (uint32_t)patched_switch_weapon_slot;

	orig_switch_weapon_slot = (void (__attribute__((thiscall)) *)(void*, uint32_t))VirtualAlloc(NULL, sizeof(intended_trampoline), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	memcpy((void *)orig_switch_weapon_slot, intended_trampoline, sizeof(intended_trampoline));
	DWORD old_protect;
	VirtualProtect((void *)orig_switch_weapon_slot, sizeof(intended_trampoline), PAGE_EXECUTE_READ, &old_protect);

	memcpy((void *)0x00b99900, intended_patch, sizeof(intended_patch));
}

// it seems that all intended movement delta goes here
struct __attribute__ ((packed)) move_actor_by_ctx{
	// float 0x118 + 0x684 holds a move_actor_exact_ctx
	uint8_t unknown[0x118 + 0x684];
	float x;
	float y;
	float z;
};
static void (__attribute__((thiscall)) *orig_move_actor_by)(void*, float, float, float);
void __attribute__((thiscall)) patched_move_actor_by(struct move_actor_by_ctx *ctx, float param_1, float param_2, float param_3){
	const double orig_fixed_frametime = 1.66666666666666678509045596002E1;

	void *ret_addr = __builtin_return_address(0);

	struct ctx_01642f30* actx = fetch_ctx_01642f30();

	LOG_VERBOSE("%s: ctx 0x%08x, param_1 %f, param_2 %f, param_3 %f", __FUNCTION__, ctx, param_1, param_2, param_3);
	LOG_VERBOSE("%s: called from 0x%08x -> 0x%08x -> 0x%08x", __FUNCTION__, __builtin_return_address(2), __builtin_return_address(1), ret_addr);
	LOG_VERBOSE("%s: actx->actor_state %u", __FUNCTION__, actx->actor_state);

	// various fixes

	float y = param_2;

	// 0x00b81247 -> in air + holding shift, provides shift + directional movement, but only after the ability was activated
	// 0x00b8110d -> in air + directional movement, also provides directional movement during flight activation

	// flight takes half a second to get into activated state
	// shooting/reloading state overrides flying state, so it is not a good indicator

	// 0x006256f8 -> in air, free fall/jump, provides directional movement
	// 0x00625cd9 -> in air, free fall/jump, provides directional movement
	// 0x006295a4 -> in air, free fall/jump while shooting, provides directional movement
	// 0x006353a7 -> x/z movement on ground, and walking onto jump pad.....
	// 0x006173e3 -> sprining, and sprinting onto jump pad...
	// 0x00626298 -> landing

	static bool flying = false;

	if(ret_addr == (void *)0x006256f8 || ret_addr == (void *)0x00625cd9 || ret_addr == (void* )0x006295a4){
		flying = false;
	}

	if(ret_addr == (void *)0x006353a7 || ret_addr == (void *)0x006173e3){
		flying = false;
	}

	if(ret_addr == (void *)0x00626298){
		flying = false;
	}

	// on ground
	if(ret_addr == (void *)0x00526f0e){
		flying = false;
	}

	// in air
	if(ret_addr == (void*)0x00527467){
		// wall jump
		if(actx->actor_state == 9){
			flying = false;
		}

		// blow/hit fly away, wall hit
		if(actx->actor_state == 16 || actx->actor_state == 17){
			flying = false;
		}

		// fly
		if(flying && param_2 > 0.0001){
			// scaled, trying not to change the behavior too hard
			// there is something funky with the gradual speed gain vs framerate however
			float modifier = (orig_fixed_frametime / frametime);
			float frametime_float = frametime * 1.0;
			if(frametime_float < orig_fixed_frametime){
				// whenever there's an increasing curve it gets weird
				// it's basically area of a smoother curve vs a less smooth curve
				float frametime_diff_ratio = (orig_fixed_frametime - frametime_float) / orig_fixed_frametime;
				modifier = modifier * (1.0 - 0.4 * frametime_diff_ratio);
			}

			y = param_2 * modifier;
			LOG_VERBOSE("%s: applying fly speed fix, y %f, y/param_2 %f", __FUNCTION__, y, modifier);
		}

		// scythe uppercut
		if(actx->actor_state == 63){
			// approx, would allow different servers with different lua values to work
			if(param_2 > 0){
				y = param_2 * (1.0 - 0.15 * std::abs(16.0 - frametime) / 6.0);
			}
			LOG_VERBOSE("%s: applying scythe uppercut speed fix, y %f, y/param_2 %f, scythe_time %u", __FUNCTION__, y, y / param_2, scythe_time);
		}

		// not the best way to identify a PS, but I don't see any other weapons using a -50000 drop value in lua
		// this absolutely do not work if a server don't use -50000
		// the PS drop makes one big drop frame on any framerate, but the speed on that single frame is scaled...
		static bool first_ps_drop_frame = true;
		if(actx->actor_state == 45 && set_drop_val == -50000){
			// a little under the expected drop speed
			float drop_cutoff = (-750.0) * (frametime / orig_fixed_frametime);
			if(param_2 < drop_cutoff){
				if(first_ps_drop_frame){
					// spike the first drop frame to the 60fps value
					// rare but there could be extra frames before
					y = -850.0;
					LOG_VERBOSE("%s: applying ps drop speed fix, y/param_2 %f", __FUNCTION__, y / param_2);
					first_ps_drop_frame = false;
				}else{
					// 0 the rest if any, rare but happens
					y = 0.0;
				}
			}
		}else{
			first_ps_drop_frame = true;
		}

		if(param_2 > 0.0001){
			flying = true;
		}
	}

	LOG("%s: ctx 0x%08x, param_1 %f, param_2 %f, param_3 %f", __FUNCTION__, ctx, param_1, param_2, param_3);
	LOG("%s: called from 0x%08x -> 0x%08x -> 0x%08x", __FUNCTION__, __builtin_return_address(2), __builtin_return_address(1), __builtin_return_address(0));
	LOG("%s: actx->actor_state %u, frametime %u", __FUNCTION__, actx->actor_state, frametime);

	orig_move_actor_by(ctx, param_1, y, param_3);
}

static void hook_move_actor_by(){
	LOG("hooking move_actor_by");

	uint8_t intended_trampoline[] = {
		// space for original instruction
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		// MOV eax,0x0051c2fa
		0xb8, 0xfa, 0xc2, 0x51, 0x00,
		// JMP eax
		0xff, 0xe0
	};
	memcpy((void *)intended_trampoline, (void *)0x0051c2f0, 10);

	uint8_t intended_patch[] = {
		// MOV eax, patched_move_actor_by
		0xb8, 0, 0, 0, 0,
		// JMP eax
		0xff, 0xe0,
		// nop nop nop
		0x90, 0x90, 0x90
	};
	*(uint32_t *)&intended_patch[1] = (uint32_t)patched_move_actor_by;

	orig_move_actor_by = (void (__attribute__((thiscall)) *)(void*, float, float, float))VirtualAlloc(NULL, sizeof(intended_trampoline), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	memcpy((void *)orig_move_actor_by, intended_trampoline, sizeof(intended_trampoline));
	DWORD old_protect;
	VirtualProtect((void *)orig_move_actor_by, sizeof(intended_trampoline), PAGE_EXECUTE_READ, &old_protect);

	memcpy((void *)0x0051c2f0, intended_patch, sizeof(intended_patch));
}

// it seems that every intended coord change goes here, then it gets processed before applied
struct __attribute__ ((packed)) move_actor_exact_ctx{
	// float 0x684 x, 0x688 y, 0x68c z
	uint8_t unknown[0x684];
	float x;
	float y;
	float z;
};
static void (__attribute__((thiscall)) *orig_move_actor_exact)(void*, float, float, float, uint32_t);
void __attribute__((thiscall)) patched_move_actor_exact(struct move_actor_exact_ctx *ctx, float param_1, float param_2, float param_3, uint32_t param_4){
	INIT_MEM_FENCE()
	float before_x = ctx->x;
	float before_y = ctx->y;
	float before_z = ctx->z;

	MEM_FENCE();

	orig_move_actor_exact(ctx, param_1, param_2, param_3, param_4);

	MEM_FENCE();

	void * ret_addr = __builtin_return_address(0);

	float after_x = ctx->x;
	float after_y = ctx->y;
	float after_z = ctx->z;

	float delta_x = after_x - before_x;
	float delta_y = after_y - before_y;
	float delta_z = after_z - before_z;

	LOG_VERBOSE("%s: ctx 0x%08x, param_1 %f, param_2 %f, param_3 %f, param_4 %u", __FUNCTION__, ctx, param_1, param_2, param_3, param_4);
	LOG_VERBOSE("%s: %f->%f %f->%f %f->%f", __FUNCTION__, before_x, after_x, before_y, after_y, before_z, after_z);
	LOG_VERBOSE("%s: %f %f %f", __FUNCTION__, after_x - before_x, after_y - before_y, after_z - before_z);
	LOG_VERBOSE("%s: return addr: 0x%08x", __FUNCTION__, ret_addr);
}

static void hook_move_actor_exact(){
	LOG("hooking move_actor_exact()");

	uint8_t intended_trampoline[] = {
		// space for original instruction
		0, 0, 0, 0, 0, 0, 0, 0, 0,
		// MOV eax,0x007b0189
		0xb8, 0x89, 0x01, 0x7b, 0x00,
		// JMP eax
		0xff, 0xe0
	};
	memcpy((void *)intended_trampoline, (void *)0x007b0180, 9);

	uint8_t intended_patch[] = {
		// MOV eax, patched_move_actor_exact
		0xb8, 0, 0, 0, 0,
		// JMP eax
		0xff, 0xe0,
		// nop nop
		0x90, 0x90
	};
	*(uint32_t *)&intended_patch[1] = (uint32_t)patched_move_actor_exact;

	orig_move_actor_exact = (void (__attribute__((thiscall)) *)(void*, float, float, float, uint32_t))VirtualAlloc(NULL, sizeof(intended_trampoline), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	memcpy((void *)orig_move_actor_exact, intended_trampoline, sizeof(intended_trampoline));
	DWORD old_protect;
	VirtualProtect((void *)orig_move_actor_exact, sizeof(intended_trampoline), PAGE_EXECUTE_READ, &old_protect);

	memcpy((void *)0x007b0180, intended_patch, sizeof(intended_patch));
}

// function at 0x00871970, not essentially game tick
static void (__attribute__((thiscall)) *orig_game_tick)(void *);
void __attribute__((thiscall)) patched_game_tick(void *tick_ctx){
	LOG_VERBOSE("game tick function hook fired");

	const static float orig_speed_dampener = 0.015;
	const static double orig_fixed_frametime = 1.66666666666666678509045596002E1;
	static float *speed_dampener = (float *)0x015f4210;

	static struct time_context tctx;

	struct game_context *ctx = fetch_game_context();
	LOG_VERBOSE("game context at 0x%08x", (uint32_t)ctx);
	ctx->fps_limiter_toggle = 0;
	orig_game_tick(tick_ctx);

	update_time_delta(&tctx);
	*speed_dampener = tctx.delta_t * orig_speed_dampener / orig_fixed_frametime;
	frametime = round(tctx.delta_t);

	LOG_VERBOSE("delta_t: %f, speed_dampener: %f", tctx.delta_t, *speed_dampener);
}
static void hook_game_tick(){
	LOG("hooking game tick");
	uint8_t intended_trampoline[] = {
		// original 9 bytes
		0, 0, 0, 0, 0, 0, 0, 0, 0,
		// MOV EAX,0x00871979
		0xb8, 0x79, 0x19, 0x87, 0x00,
		// JMP EAX
		0xff, 0xe0
	};
	memcpy(intended_trampoline, (void *)0x00871970, 9);
	DWORD old_protect;
	orig_game_tick = (void (__attribute__((thiscall)) *)(void *)) VirtualAlloc(NULL, sizeof(intended_trampoline), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if(orig_game_tick == NULL){
		LOG("Failed allocating executable memory while preparing trampoline");
		return;
	}
	memcpy((void *)orig_game_tick, intended_trampoline, sizeof(intended_trampoline));
	VirtualProtect((void *)orig_game_tick, sizeof(intended_trampoline), PAGE_EXECUTE_READ, &old_protect);

	uint32_t patched_function_location = (uint32_t)patched_game_tick;
	uint8_t *patch_location = (uint8_t*)0x00871970;
	uint8_t intended_patch[] = {
		// MOV EAX,patched_game_tick
		0xb8, 0, 0, 0, 0,
		// JMP EAX
		0xff, 0xe0,
		// nop nop
		0x90, 0x90
	};
	memcpy((void *)&intended_patch[1], (void *)&patched_function_location, 4);
	memcpy((void *)0x00871970, intended_patch, sizeof(intended_patch));
}

static void patch_min_frametime(double min_frametime){
	LOG("patching minimal frametime to %f", min_frametime);
	double *min_frametime_const = (double *)0x013d33a0;
	*min_frametime_const = min_frametime;
}

static void experinmental_static_patches(){
	LOG("applying experimental patches");
}

static void *main_thread(void *arg){
	return NULL;
}

__attribute__((constructor))
int init(){
	#if ENABLE_LOGGING
	log_file = fopen("s4_league_fps_unlock.log", "w");
	if(pthread_mutex_init(&log_mutex, NULL)){
		printf("logger mutex init failed\n");
		return 0;
	}
	#endif // ENABLE_LOGGING

	LOG("mhmm library loaded");

	hook_game_tick();
	//hook_move_actor_exact();
	hook_move_actor_by();
	//hook_switch_weapon_slot();
	hook_fun_005e4020();

	experinmental_static_patches();

	LOG("now starting main thread");
	pthread_t thread;
	pthread_create(&thread, NULL, main_thread, NULL);

	LOG("gcc constructor ending");
	return 0;
}
