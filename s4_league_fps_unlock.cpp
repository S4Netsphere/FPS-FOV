#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <cstdint>
#include <cstdlib>
#include <pthread.h>
#include <cstring>
#include <cmath>

#include "json.hpp"
#include <fstream>

#include <time.h>

#include <windows.h>

// mingw don't provide a mprotect wrap
#include <memoryapi.h>

// processes and threads
#include <processthreadsapi.h>
#include <tlhelp32.h>

// module information
#include <psapi.h>
#include <libloaderapi.h>

#define SUSPEND_BEFORE_HOOKING 1
#define ENABLE_LOGGING 0
#define SIMPLIFIED_SLEEP 0

// http://undocumented.ntinternals.net/index.html?page=UserMode%2FUndocumented%20Functions%2FNT%20Objects%2FThread%2FNtDelayExecution.html
extern "C"
NTSYSAPI
NTSTATUS
NTAPI
NtDelayExecution(
    BOOLEAN Alertable,
    PLARGE_INTEGER DelayInterval
    );

// http://undocumented.ntinternals.net/index.html?page=UserMode%2FUndocumented%20Functions%2FTime%2FNtSetTimerResolution.html
extern "C"
NTSYSAPI
NTSTATUS
NTAPI
NtQueryTimerResolution(
    PULONG MaximumTime,
    PULONG MinimumTime,
    PULONG CurrentTime
    );


// http://undocumented.ntinternals.net/index.html?page=UserMode%2FUndocumented%20Functions%2FTime%2FNtSetTimerResolution.html
extern "C"
NTSYSAPI
NTSTATUS
NTAPI
NtSetTimerResolution(
    ULONG DesiredTime,
    BOOLEAN SetResolution,
    PULONG ActualTime
    );

static ULONG min_nt_delay_100ns;

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

#define PATCH_UINT32(l, v){ \
	uint32_t _val = (uint32_t)(v); \
	patch_memory((uint8_t *)(l), (uint8_t *)&_val, 4); \
}

static pthread_mutex_t config_mutex;
struct config{
	int max_framerate;
	int field_of_view;
	int center_field_of_view;
	int sprint_field_of_view;
	bool framelimiter_full_busy_loop;
	int framelimiter_busy_loop_buffer_100ns;
};

static float frametime;
static uint64_t frametime_accumulated = 0;
static uint8_t weapon_slot;
static float set_drop_val;

struct config config = {
	.max_framerate = 300,
	.field_of_view = 60,
	.center_field_of_view = 66,
	.sprint_field_of_view = 80,
	.framelimiter_full_busy_loop = false,
	.framelimiter_busy_loop_buffer_100ns = 15000,
};

static uint32_t target_frametime_ns = (1 * 1000 * 1000 * 1000) / config.max_framerate;
static uint32_t target_frametime_100ns = target_frametime_ns / 100;

static void patch_memory(uint8_t *location, uint8_t *buffer, uint32_t size){
	DWORD orig_protect;
	DWORD old_protect;
	VirtualProtect((void *)location, size, PAGE_EXECUTE_READWRITE, &orig_protect);
	memcpy(location, buffer, size);
	VirtualProtect((void *)location, size, orig_protect, &old_protect);
}

static void parse_config(){
	const char *config_file_name = "s4_league_fps_unlock.json";
	std::ifstream config_file(config_file_name);
	if(!config_file.good()){
		LOG("failed opening %s for reading", config_file_name);
		return;
	}

	nlohmann::json parsed_config_file;
	try{
		parsed_config_file = nlohmann::json::parse(config_file);
	}catch(nlohmann::json::exception e){
		LOG("failed parsing %s, %s", config_file_name, e.what());
		return;
	}

	struct config staging_config;
	memcpy(&staging_config, &config, sizeof(struct config));

	try{
		if(!parsed_config_file["max_framerate"].is_number()){
			LOG("failed reading max_framerate from %s", config_file_name)
		}else{
			staging_config.max_framerate = parsed_config_file["max_framerate"];
			if(staging_config.max_framerate > 0){
				LOG_VERBOSE("setting max framerate to %d", staging_config.max_framerate);
			}else{
				LOG_VERBOSE("allowing game to go as fast as it can");
			}
		}
		if(!parsed_config_file["field_of_view"].is_number()){
			LOG("failed reading field_of_view from %s, ", config_file_name)
		}else{
			staging_config.field_of_view = parsed_config_file["field_of_view"];
			LOG_VERBOSE("setting field of view to %d", staging_config.field_of_view);
		}
		if(!parsed_config_file["center_field_of_view"].is_number()){
			LOG("failed reading center_field_of_view from %s, ", config_file_name)
		}else{
			staging_config.center_field_of_view = parsed_config_file["center_field_of_view"];
			LOG_VERBOSE("setting center field of view to %d", staging_config.center_field_of_view);
		}
		if(!parsed_config_file["sprint_field_of_view"].is_number()){
			LOG("failed reading sprint_field_of_view from %s, ", config_file_name)
		}else{
			staging_config.sprint_field_of_view = parsed_config_file["sprint_field_of_view"];
			LOG_VERBOSE("setting sprint field of view to %d", staging_config.sprint_field_of_view);
		}
		if(!parsed_config_file["framelimiter_full_busy_loop"].is_boolean()){
			LOG("failed reading framelimiter_full_busy_loop from %s, ", config_file_name)
		}else{
			staging_config.framelimiter_full_busy_loop = parsed_config_file["framelimiter_full_busy_loop"];
			LOG_VERBOSE("setting framelimiter full busy loop to %s", staging_config.framelimiter_full_busy_loop ? "true" : "false");
		}
		if(!parsed_config_file["framelimiter_busy_loop_buffer_100ns"].is_number()){
			LOG("failed reading framelimiter_busy_loop_buffer_100ns from %s, ", config_file_name)
		}else{
			staging_config.framelimiter_busy_loop_buffer_100ns = parsed_config_file["framelimiter_busy_loop_buffer_100ns"];
			LOG_VERBOSE("setting framelimiter busy loop buffer (100ns) to %s", framelimiter_busy_loop_buffer_100ns);
		}
	}catch(nlohmann::json::exception e){
		LOG("failed reading %s after parsing, %s", config_file_name, e.what());
	}

	if(memcmp(&config, &staging_config, sizeof(struct config)) != 0){
		pthread_mutex_lock(&config_mutex);
		memcpy(&config, &staging_config, sizeof(struct config));
		if(config.max_framerate > 0){
			target_frametime_ns = (1 * 1000 * 1000 * 1000) / config.max_framerate;
			target_frametime_100ns = target_frametime_ns / 100;
		}
		pthread_mutex_unlock(&config_mutex);
	}
}

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

static struct game_context *(*fetch_game_context)(void) = (struct game_context *(*)(void)) 0x004aeb70;
static void (__attribute__((thiscall)) *update_time_delta)(struct time_context *ctx) = (void (__attribute__((thiscall)) *)(struct time_context *ctx)) 0x010ada90;

// contains actor state
struct ctx_01b29540{
	uint8_t unknown[0xe0];
	uint8_t actor_state;
	uint8_t unknown_2[0x3];
	uint32_t actor_substate_1;
	uint32_t actor_substate_2;
};
static struct ctx_01b29540 *(*fetch_ctx_01b29540)(void) = (struct ctx_01b29540 *(*)(void)) 0x004af440;

struct funny_value{
	uint32_t value_xor;
	uint32_t value_xor_flip;
	uint32_t xor_key;
};

static uint32_t get_funny_value(struct funny_value *x){
	return x->value_xor ^ x->xor_key;
}

static void set_funny_value(struct funny_value *x, uint32_t *value){
	x->value_xor = *value ^ x->xor_key;
	x->value_xor_flip = ~x->xor_key;
}

// hook random spread calculation
struct ctx_calculate_random_spread{
	uint8_t unknown[0x12c];
	uint32_t shooting; // 0x12c
	uint8_t unknown_2[0x18];
	uint32_t spread_type; // 0x148
	uint8_t unknown_3[0x24];
	struct funny_value inner_spread_recovery; // 0x170
	struct funny_value inner_spread_change; // 0x17c
	struct funny_value inner_verdict; // 0x188
	uint8_t unknown_4[0x18];
	struct funny_value outer_spread_recovery; // 0x1ac
	struct funny_value outer_spread_change; // 0x1b8
	struct funny_value outer_verdict; // 0x1c4
};

static void (__attribute__((thiscall)) *orig_calculate_weapon_spread)(struct ctx_calculate_random_spread *, uint32_t, uint8_t);
void __attribute__((thiscall)) patched_calculate_weapon_spread(struct ctx_calculate_random_spread *ctx, uint32_t frametime_param, uint8_t param_2){
	uint32_t orig_inner_spread_recovery = get_funny_value(&ctx->inner_spread_recovery);
	uint32_t orig_outer_spread_recovery = get_funny_value(&ctx->outer_spread_recovery);
	uint32_t orig_inner_spread_change = get_funny_value(&ctx->inner_spread_change);
	uint32_t orig_outer_spread_change = get_funny_value(&ctx->outer_spread_change);

	if(ctx->spread_type == 2){
		// scale change up before the function
		const double orig_fixed_frametime = 1.66666666666666678509045596002E1; // this gives a split of 16 and 17 frametimes given it's s4
		float orig_frametime_divided_by_frametime = orig_fixed_frametime / (frametime_param * 1.0);
		float new_inner_spread_change = *(float *)&orig_inner_spread_change * orig_frametime_divided_by_frametime;
		float new_outer_spread_change = *(float *)&orig_outer_spread_change * orig_frametime_divided_by_frametime;
		set_funny_value(&ctx->inner_spread_change, (uint32_t *)&new_inner_spread_change);
		set_funny_value(&ctx->outer_spread_change, (uint32_t *)&new_outer_spread_change);
	}

	orig_calculate_weapon_spread(ctx, frametime_param, param_2);

	set_funny_value(&ctx->inner_spread_recovery, &orig_inner_spread_recovery);
	set_funny_value(&ctx->outer_spread_recovery, &orig_outer_spread_recovery);
	set_funny_value(&ctx->inner_spread_change, &orig_inner_spread_change);
	set_funny_value(&ctx->outer_spread_change, &orig_outer_spread_change);

	#if ENABLE_LOGGING
	uint32_t inner_verdict = get_funny_value(&(ctx->inner_verdict));
	float inner_verdict_f = *(float *)&inner_verdict;
	uint32_t outer_verdict = get_funny_value(&(ctx->outer_verdict));
	float outer_verdict_f = *(float *)&outer_verdict;
	#endif
	LOG_VERBOSE("%s: ctx 0x%08x", __FUNCTION__, ctx);
	LOG_VERBOSE("%s: frametime_param: %u, param_2: %u", __FUNCTION__, frametime_param, param_2);
	LOG_VERBOSE("%s: inner_verdict: %f, outer_verdict: %f", __FUNCTION__, inner_verdict_f, outer_verdict_f);
	LOG_VERBOSE("%s: ret chain 0x%08x -> 0x%08x -> 0x%08x -> 0x%08x", __FUNCTION__, __builtin_return_address(0), __builtin_return_address(1), __builtin_return_address(2), __builtin_return_address(3));

	return;
}

static void hook_calculate_weapon_spread(uint32_t address_offset){
	LOG("hooking calculate_weapon_spread");

	uint8_t intended_trampoline[] = {
		// space for original instruction
		0, 0, 0, 0, 0, 0, 0, 0, 0,
		// MOV eax,005970a9
		0xb8, 0xa9, 0x70, 0x59, 0x00,
		// JMP eax
		0xff, 0xe0
	};
	*(uint32_t *)&intended_trampoline[10] = 0x005970a9 + address_offset;
	memcpy((void *)intended_trampoline, (void *)(0x005970a0 + address_offset), 9);

	uint8_t intended_patch[] = {
		// MOV eax, patched_calculate_weapon_spread
		0xb8, 0, 0, 0, 0,
		// JMP eax
		0xff, 0xe0,
		// nop nop
		0x90, 0x90
	};
	*(uint32_t *)&intended_patch[1] = (uint32_t)patched_calculate_weapon_spread;

	orig_calculate_weapon_spread = (void (__attribute__((thiscall)) *)(struct ctx_calculate_random_spread *, uint32_t, uint8_t))VirtualAlloc(NULL, sizeof(intended_trampoline), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	memcpy((void *)orig_calculate_weapon_spread, intended_trampoline, sizeof(intended_trampoline));
	DWORD old_protect;
	VirtualProtect((void *)orig_calculate_weapon_spread, sizeof(intended_trampoline), PAGE_EXECUTE_READ, &old_protect);

	patch_memory((uint8_t *)(0x005970a0 + address_offset), intended_patch, sizeof(intended_patch));
}

// can change active fov by hooking this
struct ctx_fun_00780b20{
	uint8_t unknown[0x158];
	float target_fov;
};
static void (__attribute__((thiscall)) *orig_fun_00780b20)(void *, uint32_t);
void __attribute__((thiscall)) patched_fun_00780b20(struct ctx_fun_00780b20 *ctx, uint32_t param_1){
	float orig_fov = ctx->target_fov;
	pthread_mutex_lock(&config_mutex);
	if(ctx->target_fov == 60.0){
		ctx->target_fov = config.field_of_view;
	}else if(ctx->target_fov == 66.0){
		ctx->target_fov = config.center_field_of_view;
	}else if(ctx->target_fov == 80.0){
		ctx->target_fov = config.sprint_field_of_view;
	}
	pthread_mutex_unlock(&config_mutex);
	LOG_VERBOSE("%s: ctx 0x%08x, current fov %f, override fov %f", __FUNCTION__, ctx, orig_fov, ctx->target_fov);
	orig_fun_00780b20(ctx, param_1);
	ctx->target_fov = orig_fov;
}

static void hook_fun_00780b20(uint32_t address_offset){
	LOG("hooking fun_00780b20");

	uint8_t intended_trampoline[] = {
		// space for original instruction
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		// MOV eax,00780b2a
		0xb8, 0x2a, 0x0b, 0x78, 0x00,
		// JMP eax
		0xff, 0xe0
	};
	*(uint32_t *)&intended_trampoline[11] = 0x00780b2a + address_offset;
	memcpy((void *)intended_trampoline, (void *)(0x00780b20 + address_offset), 10);

	uint8_t intended_patch[] = {
		// MOV eax, patched_fun_00780b20
		0xb8, 0, 0, 0, 0,
		// JMP eax
		0xff, 0xe0,
		// nop nop nop
		0x90, 0x90, 0x90
	};
	*(uint32_t *)&intended_patch[1] = (uint32_t)patched_fun_00780b20;

	orig_fun_00780b20 = (void (__attribute__((thiscall)) *)(void*, uint32_t))VirtualAlloc(NULL, sizeof(intended_trampoline), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	memcpy((void *)orig_fun_00780b20, intended_trampoline, sizeof(intended_trampoline));
	DWORD old_protect;
	VirtualProtect((void *)orig_fun_00780b20, sizeof(intended_trampoline), PAGE_EXECUTE_READ, &old_protect);

	patch_memory((uint8_t *)(0x00780b20 + address_offset), intended_patch, sizeof(intended_patch));
}

// this is a looong function with a lot of branches, but it seems to use the SetDrop value during a jump attack
struct ctx_fun_005efcb0{
	uint8_t unknown[0x2cc + 0x4];
	float set_drop_val;
};
static void (__attribute__((thiscall)) *orig_fun_005efcb0)(void *, uint32_t);
void __attribute__((thiscall)) patched_fun_005efcb0(struct ctx_fun_005efcb0 *ctx, uint32_t param_1){
	orig_fun_005efcb0(ctx, param_1);
	if((void *)0x0052438c == __builtin_return_address(1)){
		set_drop_val = ctx->set_drop_val;
		LOG_VERBOSE("%s: updating player set_drop_val to %f", __FUNCTION__, set_drop_val);
	}
	LOG_VERBOSE("%s: ctx 0x%08x, param_1 %u, set_drop_val %f, 0x%08x -> 0x%08x -> 0x%08x", __FUNCTION__, ctx, param_1, ctx->set_drop_val, __builtin_return_address(2), __builtin_return_address(1), __builtin_return_address(0));
}

static void hook_fun_005efcb0(uint32_t address_offset){
	LOG("hooking fun_005efcb0");

	uint8_t intended_trampoline[] = {
		// space for original instruction
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		// MOV eax,0x005efcba
		0xb8, 0xba, 0xfc, 0x5e, 0x00,
		// JMP eax
		0xff, 0xe0
	};
	*(uint32_t *)&intended_trampoline[11] = 0x005efcba + address_offset;
	memcpy((void *)intended_trampoline, (void *)(0x005efcb0 + address_offset), 10);

	uint8_t intended_patch[] = {
		// MOV eax, patched_fun_005efcb0
		0xb8, 0, 0, 0, 0,
		// JMP eax
		0xff, 0xe0,
		// nop nop nop
		0x90, 0x90, 0x90
	};
	*(uint32_t *)&intended_patch[1] = (uint32_t)patched_fun_005efcb0;

	orig_fun_005efcb0 = (void (__attribute__((thiscall)) *)(void*, uint32_t))VirtualAlloc(NULL, sizeof(intended_trampoline), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	memcpy((void *)orig_fun_005efcb0, intended_trampoline, sizeof(intended_trampoline));
	DWORD old_protect;
	VirtualProtect((void *)orig_fun_005efcb0, sizeof(intended_trampoline), PAGE_EXECUTE_READ, &old_protect);

	patch_memory((uint8_t *)(0x005efcb0 + address_offset), intended_patch, sizeof(intended_patch));
}

// TODO find s10 offset, this function is also not used at the moment
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
	LOG_VERBOSE("%s: ctx 0x%08x, param_1 %u, weapon slot switched to %u, 0x%08x -> 0x%08x", __FUNCTION__, ctx, param_1, weapon_slot, __builtin_return_address(1), __builtin_return_address(0));
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

	patch_memory((uint8_t *)0x00b99900, intended_patch, sizeof(intended_patch));
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

	struct ctx_01b29540* actx = fetch_ctx_01b29540();

	// something odd is happening here after porting from s8 to s10, might have to reserve a float register here somehow

	LOG_VERBOSE("%s: ctx 0x%08x, param_1 %f, param_2 %f, param_3 %f", __FUNCTION__, ctx, param_1, param_2, param_3);
	LOG_VERBOSE("%s: called from 0x%08x -> 0x%08x -> 0x%08x", __FUNCTION__, __builtin_return_address(2), __builtin_return_address(1), ret_addr);
	LOG_VERBOSE("%s: actx 0x%08x", __FUNCTION__, actx);
	LOG_VERBOSE("%s: actx->actor_state %u", __FUNCTION__, actx->actor_state);
	LOG_VERBOSE("%s: actx->actor_substate_1 0x%08x", __FUNCTION__, actx->actor_substate_1);
	LOG_VERBOSE("%s: actx->actor_substate_2 0x%08x", __FUNCTION__, actx->actor_substate_2);

	// various fixes

	float y = param_2;
	// in air
	if(ret_addr == (void*)0x0052c487){
		// fly
		static bool was_flying = false;
		bool flying = false;
		if(actx->actor_state == 31){
			flying = true;
		}

		if((actx->actor_state == 39 || actx->actor_state == 25) && (actx->actor_substate_2 & 0xffff) == 0x02ff){
			flying = true;
		}

		if(actx->actor_state == 4 && was_flying){
			flying = true;
		}

		was_flying = flying;

		if(flying && param_2 > 0.0001){
			// scaled, trying not to change the behavior too hard
			// there is something funky with the gradual speed gain vs framerate however
			float modifier = (orig_fixed_frametime / frametime);
			if(frametime < orig_fixed_frametime){
				// whenever there's an increasing curve it gets weird
				// it's basically area of a smoother curve vs a less smooth curve
				float frametime_diff_ratio = (orig_fixed_frametime - frametime) / orig_fixed_frametime;
				modifier = modifier * (1.0 - 0.4 * frametime_diff_ratio);
			}

			y = param_2 * modifier;
			was_flying = true;
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
	}

	// on ground
	if(ret_addr == (void *)0x0052bf2e){
	}

	if(ret_addr == (void *)0x0052c487 || ret_addr == (void*)0x0052bf2e){
		LOG_VERBOSE("%s: ctx 0x%08x, param_1 %f, param_2 %f, param_3 %f", __FUNCTION__, ctx, param_1, param_2, param_3);
		LOG_VERBOSE("%s: called from 0x%08x -> 0x%08x -> 0x%08x", __FUNCTION__, __builtin_return_address(2), __builtin_return_address(1), __builtin_return_address(0));
		LOG_VERBOSE("%s: actx->actor_state %u", __FUNCTION__, actx->actor_state);
	}

	orig_move_actor_by(ctx, param_1, y, param_3);
}

static void hook_move_actor_by(uint32_t address_offset){
	LOG("hooking move_actor_by");

	uint8_t intended_trampoline[] = {
		// space for original instruction
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		// MOV eax,0052103a
		0xb8, 0x3a, 0x10, 0x52, 0x00,
		// JMP eax
		0xff, 0xe0
	};
	*(uint32_t *)&intended_trampoline[11] = 0x0052103a + address_offset;
	memcpy((void *)intended_trampoline, (void *)(0x00521030 + address_offset), 10);

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

	patch_memory((uint8_t *)(0x00521030 + address_offset), intended_patch, sizeof(intended_patch));
}

// TODO find s10 offset, this function is also not used at the moment
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

	patch_memory((uint8_t *)0x007b0180, intended_patch, sizeof(intended_patch));
}

// 9 direct usages of 0x015f4210
float speed_dampeners[9];
static void redirect_speed_dampeners(uint32_t address_offset){
	LOG("%s: redirecting speed dampeners to 0x%08x to 0x%08x", __FUNCTION__, &speed_dampeners[0], &speed_dampeners[8]);

	for(int i = 0;i < sizeof(speed_dampeners) / sizeof(float); i++){
		const uint8_t value[] = {0x8f, 0xc2, 0x75, 0x3c};
		memcpy(&speed_dampeners[i], value, sizeof value);
	}

	PATCH_UINT32(0x0056b25e + address_offset, &speed_dampeners[0]);
	PATCH_UINT32(0x007d040d + address_offset, &speed_dampeners[1]);
	PATCH_UINT32(0x007d047a + address_offset, &speed_dampeners[2]);
	PATCH_UINT32(0x007d0fd4 + address_offset, &speed_dampeners[3]);
	PATCH_UINT32(0x007d0fdc + address_offset, &speed_dampeners[4]);
	PATCH_UINT32(0x007d1743 + address_offset, &speed_dampeners[5]);
	PATCH_UINT32(0x007d1779 + address_offset, &speed_dampeners[6]);
	PATCH_UINT32(0x007d1cac + address_offset, &speed_dampeners[7]);
	PATCH_UINT32(0x007d2133 + address_offset, &speed_dampeners[8]);
}

// function at 00871970, not essentially game tick
static void (__attribute__((thiscall)) *orig_game_tick)(void *);
void __attribute__((thiscall)) patched_game_tick(void *tick_ctx){
	LOG_VERBOSE("game tick function hook fired");


	const static float orig_speed_dampener = 0.015;
	const static double orig_fixed_frametime = 1.66666666666666678509045596002E1;

	static struct time_context tctx;

	struct game_context *ctx = fetch_game_context();
	LOG_VERBOSE("game context at 0x%08x", (uint32_t)ctx);

	bool should_limit = ctx->fps_limiter_toggle != 0;

	pthread_mutex_lock(&config_mutex);
	if(config.max_framerate > 0 && should_limit){
		static struct timespec last_tick = {0};
		struct timespec this_tick;
		clock_gettime(CLOCK_MONOTONIC, &this_tick);

		#if LOG_VERBOSE
		static uint32_t tick_count = 1;
		#endif

		if(last_tick.tv_sec != 0 || last_tick.tv_nsec != 0){
			uint32_t diff_ns = this_tick.tv_nsec - last_tick.tv_nsec;
			while(last_tick.tv_sec == this_tick.tv_sec && diff_ns < target_frametime_ns){
				if(config.framelimiter_full_busy_loop){
					// spin it all
				}else{
					#if SIMPLIFIED_SLEEP
					sleep(0);
					#else
					uint32_t diff_100ns = diff_ns / 100;
					if(target_frametime_100ns > diff_100ns + config.framelimiter_busy_loop_buffer_100ns){
						uint32_t sleep_100ns = target_frametime_100ns - (diff_100ns + config.framelimiter_busy_loop_buffer_100ns);
						#if LOG_VERBOSE
						uint32_t sleep_100ns_pre_correct = sleep_100ns;
						#endif
						// correct to multiple of min_nt_delay_100ns
						sleep_100ns = min_nt_delay_100ns * (sleep_100ns / min_nt_delay_100ns);
						LOG_VERBOSE("need %u pieces of 100ns delay, corrected to %u using %u", sleep_100ns_pre_correct, sleep_100ns, min_nt_delay_100ns);
						if(sleep_100ns > 0){
							LOG_VERBOSE("at tick %u time %u using NtDelayExecution to delay %u pieces of 100ns", tick_count, this_tick.tv_nsec, sleep_100ns);
							LARGE_INTEGER sleep_li;
							sleep_li.QuadPart = sleep_100ns;
							sleep_li.QuadPart *= -1;
							NtDelayExecution(false, &sleep_li);
						}
					}
					#endif
					// spin the rest
				}
				clock_gettime(CLOCK_MONOTONIC, &this_tick);
				diff_ns = this_tick.tv_nsec - last_tick.tv_nsec;
				if(last_tick.tv_sec == this_tick.tv_sec && diff_ns < target_frametime_ns){
					LOG_VERBOSE("spinning, diff_ns %u", diff_ns);
				}else{
					LOG_VERBOSE("overshot by %u + %u ns", this_tick.tv_sec - last_tick.tv_sec, diff_ns - target_frametime_ns);
				}
			}
		}
		last_tick = this_tick;
		#if LOG_VERBOSE
		tick_count++;
		#endif
	}
	pthread_mutex_unlock(&config_mutex);

	uint8_t fps_limiter_toggle_orig = ctx->fps_limiter_toggle;
	ctx->fps_limiter_toggle = 0;
	orig_game_tick(tick_ctx);
	ctx->fps_limiter_toggle = fps_limiter_toggle_orig;

	update_time_delta(&tctx);

	frametime = tctx.delta_t;
	uint32_t frametime_uint = frametime;
	frametime_accumulated = frametime_accumulated + frametime_uint;

	float new_speed_dampener = frametime * orig_speed_dampener / orig_fixed_frametime;
	// some kind of per frame speed filter, filters out smaller movement
	speed_dampeners[3] = new_speed_dampener;
	speed_dampeners[4] = new_speed_dampener;
	// some kind of overall speed dampener
	speed_dampeners[8] = new_speed_dampener;

	LOG_VERBOSE("delta_t: %f, speed_dampener: %f", tctx.delta_t, *speed_dampener);
}
static void hook_game_tick(uint32_t address_offset){
	LOG("hooking game tick");
	uint8_t intended_trampoline[] = {
		// original 9 bytes
		0, 0, 0, 0, 0, 0, 0, 0, 0,
		// MOV EAX,0x0089f409
		0xb8, 0x09, 0xf4, 0x89, 0x00,
		// JMP EAX
		0xff, 0xe0
	};
	*(uint32_t *)&intended_trampoline[10] = 0x0089f409 + address_offset;
	memcpy(intended_trampoline, (void *)(0x0089f400 + address_offset), 9);
	DWORD old_protect;
	orig_game_tick = (void (__attribute__((thiscall)) *)(void *)) VirtualAlloc(NULL, sizeof(intended_trampoline), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

	memcpy((void *)orig_game_tick, intended_trampoline, sizeof(intended_trampoline));
	VirtualProtect((void *)orig_game_tick, sizeof(intended_trampoline), PAGE_EXECUTE_READ, &old_protect);

	uint32_t patched_function_location = (uint32_t)patched_game_tick;
	uint8_t intended_patch[] = {
		// MOV EAX,patched_game_tick
		0xb8, 0, 0, 0, 0,
		// JMP EAX
		0xff, 0xe0,
		// nop nop
		0x90, 0x90
	};
	memcpy((void *)&intended_patch[1], (void *)&patched_function_location, 4);
	patch_memory((uint8_t *)(0x0089f400 + address_offset), intended_patch, sizeof(intended_patch));
}

// TODO this has s8 offset, but unused at the moment
static void patch_min_frametime(double min_frametime){
	LOG("patching minimal frametime to %f", min_frametime);
	double *min_frametime_const = (double *)0x013d33a0;
	*min_frametime_const = min_frametime;
}

static void experinmental_static_patches(uint32_t address_offset){
	LOG("applying experimental patches");
}

static void *main_thread(void *arg){
	LOG("main thread started");
	while(true){
		sleep(2);
		parse_config();
	}
	return NULL;
}

static void prepare_nt_timer(){
	ULONG max_nt_delay_100ns;
	ULONG current_nt_delay_100ns;
	int i;
	NtQueryTimerResolution(&max_nt_delay_100ns, &min_nt_delay_100ns, &current_nt_delay_100ns);
	for(i = 0; i < 10; i++){
		NtSetTimerResolution(min_nt_delay_100ns, true, &current_nt_delay_100ns);
		if(min_nt_delay_100ns == current_nt_delay_100ns){
			break;
		}else{
			LOG("NtSetTimerResolution could not set delay to minimal, trying again");
		}
	}
	LOG("NtDelayExecution now has a %u * 100ns resolution accuracy", min_nt_delay_100ns);
}

static void *delayed_init_thread(void *arg){
	#if ENABLE_LOGGING
	log_file = fopen("s4_league_fps_unlock.log", "w");
	if(pthread_mutex_init(&log_mutex, NULL)){
		printf("logger mutex init failed\n");
		return 0;
	}
	#endif // ENABLE_LOGGING

	if(pthread_mutex_init(&config_mutex, NULL)){
		printf("config mutex init failed\n");
		return 0;
	}
	int delay_sec = 15;
	LOG("delayed init thread started, delaying for %d seconds\n", delay_sec);
	sleep(delay_sec);

	// might need to pause all threads here before hooking

	#if !SIMPLIFIED_SLEEP
	prepare_nt_timer();
	#endif

	parse_config();

	const char *exe_name = "S4Client.exe";
	HMODULE s4client_module = GetModuleHandleA(exe_name);
	if(s4client_module == INVALID_HANDLE_VALUE){
		LOG("failed fetching module handle of %s, terminating!", exe_name);
		exit(1);
	}
	HANDLE current_process = GetCurrentProcess();
	MODULEINFO module_info;
	if(GetModuleInformation(current_process, s4client_module, &module_info, sizeof(module_info)) == 0){
		LOG("failed getting module info of %s, 0x%08x, terminating!", exe_name, GetLastError());
		exit(1);
	}
	uint32_t game_base_address = (uint32_t)module_info.lpBaseOfDll;
	CloseHandle(s4client_module);
	CloseHandle(current_process);
	LOG("game base address is 0x%08x", game_base_address);
	uint32_t address_offset = game_base_address - 0x00400000;

	#if SUSPEND_BEFORE_HOOKING
	// https://learn.microsoft.com/en-us/windows/win32/toolhelp/traversing-the-thread-list
	DWORD this_thread = GetCurrentThreadId();
	DWORD this_process = GetCurrentProcessId();
	HANDLE threads_snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	THREADENTRY32 te32;
	if(threads_snap == INVALID_HANDLE_VALUE){
		LOG("cannot suspend before hooking");
	}else{
		LOG("suspending threads before hooking");
		te32.dwSize = sizeof(THREADENTRY32);
		if(Thread32First(threads_snap, &te32)){
			do{
				if(te32.th32OwnerProcessID == this_process && te32.th32ThreadID != this_thread){
					HANDLE thread_handle = OpenThread(THREAD_SUSPEND_RESUME, 0, te32.th32ThreadID);
					if(thread_handle != INVALID_HANDLE_VALUE){
						int ret = SuspendThread(thread_handle);
						if(ret == -1){
							LOG("failed suspending thread with id %u\n", te32.th32ThreadID);
						}
						CloseHandle(thread_handle);
					}else{
						LOG("failed fetching handle for thread with id %u during suspend\n", te32.th32ThreadID);
					}
				}
			}while(Thread32Next(threads_snap, &te32));
		}else{
			LOG("failed fetching the first thread entry, not suspending threads before hooking");
			CloseHandle(threads_snap);
			threads_snap = INVALID_HANDLE_VALUE;
		}
	}
	#endif

	fetch_game_context = (struct game_context *(*)(void)) ((uint32_t)fetch_game_context + address_offset);
	fetch_ctx_01b29540 = (struct ctx_01b29540 *(*)(void)) ((uint32_t)fetch_ctx_01b29540 + address_offset);

	redirect_speed_dampeners(address_offset);
	hook_game_tick(address_offset);
	//hook_move_actor_exact();
	hook_move_actor_by(address_offset);
	//hook_switch_weapon_slot();
	hook_fun_005efcb0(address_offset);
	hook_fun_00780b20(address_offset);
	hook_calculate_weapon_spread(address_offset);
	//experinmental_static_patches(address_offset);

	#if SUSPEND_BEFORE_HOOKING
	if(threads_snap != INVALID_HANDLE_VALUE){
		LOG("resuming thrads after hooking")
		if(Thread32First(threads_snap, &te32)){
			do{
				if(te32.th32OwnerProcessID == this_process && te32.th32ThreadID != this_thread){
					HANDLE thread_handle = OpenThread(THREAD_SUSPEND_RESUME, 0, te32.th32ThreadID);
					if(thread_handle != INVALID_HANDLE_VALUE){
						int ret = ResumeThread(thread_handle);
						if(ret == -1){
							LOG("failed resuming thread with id %u, terminating!", te32.th32ThreadID);
							exit(1);
						}
						CloseHandle(thread_handle);
					}else{
						LOG("failed fetching handle for thread with id %u during resume, terminating!", te32.th32ThreadID);
						exit(1);
					}
				}
			}while(Thread32Next(threads_snap, &te32));
		}else{
			LOG("failed fetching the first thread entry, cannot resume threads, terminating!");
			exit(1);
		}
	}
	#endif

	LOG("now starting main thread");
	pthread_t thread;
	pthread_create(&thread, NULL, main_thread, NULL);

	return NULL;
}

__attribute__((constructor))
int init(){
	pthread_t thread;
	pthread_create(&thread, NULL, delayed_init_thread, NULL);

	return 0;
}
